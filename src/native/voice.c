#define _GNU_SOURCE
/* MM owns calls. This service observes them; it never sends call-control AT,
 * creates/answers/deletes calls, or ends a call on an elapsed-time heuristic. */
#include "native.h"
#include <errno.h>
#include <fcntl.h>
#include <gio/gunixsocketaddress.h>
#include <glob.h>
#include <linux/netlink.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define MM "org.freedesktop.ModemManager1"
#define ROOT "/org/freedesktop/ModemManager1"
#define CALL ROOT "/Call/"
#define VOICE MM ".Modem.Voice"
#define CALL_IF MM ".Call"
#define RUN "/run/e5-voice-audio/"
typedef struct App App;
typedef struct {
    char *path, *member;
    GVariant *value;
} Event;
typedef struct {
    GSocketConnection *connection;
    GPtrArray *sinks;
    int rate;
} Lease;
struct App {
    GMainLoop *loop;
    ScreenHttp *http;
    GDBusConnection *bus;
    char *owner, *observer_error, *settings_error, *config_error, *route_error, *failed_key,
        *desired_key;
    Json *objects, *calls, *settings, *status, *bt_status, *power;
    GQueue events;
    guint epoch, object_revision;
    GHashTable *call_reads;
    bool loading, valid, overflow, closing, dirty, working, config_dirty, bt_working,
        hardware_ready, bus_connecting, bt_retry_after_pcm;
    VoiceHardware *hardware;
    int band, volume;
    bool ring_sound, ring_vibrate, ring_screen;
    char *ring_tone;
    double headset_intent;
    Lease *lease;
    GCancellable *bt_cancel;
    GSource *lease_source;
    GSubprocess *sound, *vibration;
    double next_ring;
    guint heartbeat, retry, ring_timer;
    pthread_t dsp_thread;
    bool dsp_started;
    int dsp_write;
    int notify, dsp, uevent;
    guint notify_source, dsp_source, power_source;
    unsigned char dsp_partial[20];
    size_t dsp_size;
};
static void update(App *a);
static void load_objects(App *a);
static void load_call(App *a, const char *path);
static void bt_release(App *a);
static GQuark domain(void) {
    return g_quark_from_static_string("e5-voice-native");
}
static void cancel_child(GCancellable *parent, gpointer child) {
    (void)parent;
    g_cancellable_cancel(child);
}
static gboolean cancel_timeout(gpointer data) {
    g_cancellable_cancel(data);
    return G_SOURCE_REMOVE;
}
typedef struct {
    bool done, ok;
    char *out, *err;
    GError *error;
} CommandResult;
static void command_finished(GObject *object, GAsyncResult *result, gpointer data) {
    CommandResult *reply = data;
    reply->ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(object), result, &reply->out,
                                                     &reply->err, &reply->error);
    reply->done = true;
}
static char *command(const char *const *argv, unsigned timeout, GCancellable *parent,
                     int *exit_status, GError **error) {
    GSubprocess *process = g_subprocess_newv(
        argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, error);
    if (!process)
        return NULL;
    /* The deadline must also run during shutdown, when the application's
     * main loop has stopped. A private context avoids main-loop reentrancy
     * and requires neither a polling loop nor another timeout thread. */
    GMainContext *context = g_main_context_new();
    g_main_context_push_thread_default(context);
    GCancellable *cancel = g_cancellable_new();
    gulong watch = parent ? g_cancellable_connect(parent, G_CALLBACK(cancel_child),
                                                  g_object_ref(cancel), g_object_unref)
                          : 0;
    GSource *timer = g_timeout_source_new(timeout * 1000);
    g_source_set_callback(timer, cancel_timeout, g_object_ref(cancel), g_object_unref);
    g_source_attach(timer, context);
    CommandResult reply = {0};
    g_subprocess_communicate_utf8_async(process, NULL, cancel, command_finished, &reply);
    while (!reply.done)
        g_main_context_iteration(context, TRUE);
    bool ok = reply.ok;
    char *out = reply.out;
    if (reply.error)
        g_propagate_error(error, reply.error);
    if (!ok) {
        g_subprocess_force_exit(process);
        g_subprocess_wait(process, NULL, NULL);
        g_free(out);
        out = NULL;
    }
    if (exit_status)
        *exit_status =
            ok && g_subprocess_get_if_exited(process) ? g_subprocess_get_exit_status(process) : -1;
    g_source_destroy(timer);
    g_source_unref(timer);
    g_main_context_pop_thread_default(context);
    g_main_context_unref(context);
    if (parent && watch)
        g_cancellable_disconnect(parent, watch);
    g_object_unref(cancel);
    g_object_unref(process);
    g_free(reply.err);
    return out;
}
static void media(bool suspended) {
    const char *args[] = {"pactl", "suspend-sink", "speaker", suspended ? "1" : "0", NULL};
    GError *error = NULL;
    int rc;
    char *out = command(args, 5, NULL, &rc, &error);
    g_free(out);
    if (error || rc)
        g_warning("Cannot %s media sink", suspended ? "suspend" : "resume");
    g_clear_error(&error);
}
static void lease_free(Lease *lease) {
    if (!lease)
        return;
    if (lease->connection) {
        g_io_stream_close(G_IO_STREAM(lease->connection), NULL, NULL);
        g_object_unref(lease->connection);
    }
    for (unsigned i = 0; i < lease->sinks->len; i++) {
        const char *args[] = {"pactl", "suspend-sink", g_ptr_array_index(lease->sinks, i), "0",
                              NULL};
        GError *e = NULL;
        char *out = command(args, 3, NULL, NULL, &e);
        g_free(out);
        g_clear_error(&e);
    }
    g_ptr_array_unref(lease->sinks);
    g_free(lease);
}
static void lease_worker(GTask *task, gpointer object, gpointer data, GCancellable *cancel) {
    (void)object;
    (void)data;
    Lease *lease = g_new0(Lease, 1);
    lease->sinks = g_ptr_array_new_with_free_func(g_free);
    GError *error = NULL;
    int rc = 0;
    const char *list[] = {"pactl", "list", "sinks", "short", NULL};
    char *out = command(list, 3, cancel, &rc, &error);
    if (error)
        goto failed;
    if (!rc && out) {
        char **lines = g_strsplit(out, "\n", 65);
        for (unsigned i = 0; lines[i] && !g_cancellable_is_cancelled(cancel); i++) {
            char **fields = g_strsplit_set(lines[i], " \t", -1);
            const char *name = NULL;
            unsigned words = 0;
            for (unsigned n = 0; fields[n]; n++)
                if (*fields[n] && ++words == 2) {
                    name = fields[n];
                    break;
                }
            if (name && g_str_has_prefix(name, "bluez_sink.")) {
                const char *args[] = {"pactl", "suspend-sink", name, "1", NULL};
                char *reply = command(args, 3, cancel, &rc, &error);
                g_free(reply);
                if (!error && !rc)
                    g_ptr_array_add(lease->sinks, g_strdup(name));
                if (error) {
                    g_strfreev(fields);
                    break;
                }
            }
            g_strfreev(fields);
        }
        g_strfreev(lines);
    }
    g_free(out);
    out = NULL;
    if (error)
        goto failed;
    GSocketClient *client = g_socket_client_new();
    g_socket_client_set_timeout(client, 9);
    char *path = native_path("/run/e5-hfp/audio.sock");
    GSocketAddress *address = g_unix_socket_address_new(path);
    g_free(path);
    lease->connection =
        g_socket_client_connect(client, G_SOCKET_CONNECTABLE(address), cancel, &error);
    g_object_unref(address);
    g_object_unref(client);
    if (!lease->connection)
        goto failed;
    const char request[] = "{\"acquire\":true}\n";
    if (!g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(lease->connection)),
                                   request, sizeof(request) - 1, NULL, cancel, &error))
        goto failed;
    GString *wire = g_string_new(NULL);
    bool ok = false;
    while (wire->len < 2048) {
        char buf[256];
        gssize n = g_input_stream_read(g_io_stream_get_input_stream(G_IO_STREAM(lease->connection)),
                                       buf, MIN(sizeof(buf), 2048 - wire->len), cancel, &error);
        if (n <= 0)
            break;
        g_string_append_len(wire, buf, n);
        if (strchr(wire->str, '\n')) {
            ok = true;
            break;
        }
    }
    Json *reply = ok ? json_tokener_parse(wire->str) : NULL;
    g_string_free(wire, TRUE);
    if (reply && jbool(reply, "ok", false)) {
        lease->rate = jint(reply, "rate", 0);
        ok = lease->rate == 8000 || lease->rate == 16000;
    } else
        ok = false;
    if (reply)
        json_object_put(reply);
    if (!ok) {
        if (!error)
            g_set_error_literal(&error, domain(), 1, "SCO negotiation failed");
        goto failed;
    }
    if (g_cancellable_is_cancelled(cancel)) {
        g_set_error_literal(&error, domain(), 1, "SCO request canceled");
        goto failed;
    }
    g_task_return_pointer(task, lease, NULL);
    return;
failed:
    g_free(out);
    lease_free(lease);
    if (!error)
        g_set_error_literal(&error, domain(), 1, "SCO connection unavailable");
    g_task_return_error(task, error);
}
static void released(GObject *object, GAsyncResult *res, gpointer data) {
    (void)object;
    App *a = data;
    g_task_propagate_boolean(G_TASK(res), NULL);
    a->bt_working = false;
    if (!a->closing)
        update(a);
}
static void release_worker(GTask *task, gpointer object, gpointer data, GCancellable *cancel) {
    (void)object;
    (void)cancel;
    lease_free(data);
    g_task_return_boolean(task, true);
}
static void bt_release(App *a) {
    if (a->bt_cancel)
        g_cancellable_cancel(a->bt_cancel);
    if (a->lease_source) {
        g_source_destroy(a->lease_source);
        g_source_unref(a->lease_source);
        a->lease_source = NULL;
    }
    if (a->lease) {
        Lease *lease = a->lease;
        a->lease = NULL;
        a->bt_working = true;
        GTask *task = g_task_new(NULL, NULL, released, a);
        g_task_set_task_data(task, lease, NULL);
        g_task_run_in_thread(task, release_worker);
        g_object_unref(task);
    }
}
static gboolean lease_lost(GSocket *socket, GIOCondition condition, gpointer data) {
    (void)socket;
    (void)condition;
    App *a = data;
    g_free(a->failed_key);
    a->failed_key = g_strdup(a->desired_key);
    g_free(a->route_error);
    a->route_error = g_strdup("Bluetooth call audio disconnected; using phone");
    bt_release(a);
    update(a);
    return G_SOURCE_REMOVE;
}
static void acquired(GObject *object, GAsyncResult *res, gpointer data) {
    (void)object;
    App *a = data;
    GError *error = NULL;
    Lease *lease = g_task_propagate_pointer(G_TASK(res), &error);
    bool canceled = a->closing || g_cancellable_is_cancelled(a->bt_cancel);
    g_clear_object(&a->bt_cancel);
    a->bt_working = false;
    if (error || canceled) {
        if (lease) {
            a->lease = lease;
            bt_release(a);
        }
        if (!canceled) {
            g_free(a->failed_key);
            a->failed_key = g_strdup(a->desired_key);
            g_free(a->route_error);
            a->route_error = g_strdup("Bluetooth call audio unavailable; using phone");
        }
        g_clear_error(&error);
    } else {
        a->lease = lease;
        a->lease_source = g_socket_create_source(g_socket_connection_get_socket(lease->connection),
                                                 G_IO_IN | G_IO_HUP | G_IO_ERR, NULL);
        g_source_set_callback(a->lease_source, G_SOURCE_FUNC(lease_lost), a, NULL);
        g_source_attach(a->lease_source, NULL);
        g_clear_pointer(&a->route_error, g_free);
        g_clear_pointer(&a->failed_key, g_free);
    }
    if (!a->closing)
        update(a);
}
static void bt_acquire(App *a) {
    if (a->bt_working || a->lease || a->closing)
        return;
    a->bt_working = true;
    a->bt_cancel = g_cancellable_new();
    GTask *task = g_task_new(NULL, a->bt_cancel, acquired, a);
    g_task_set_check_cancellable(task, FALSE);
    g_task_run_in_thread(task, lease_worker);
    g_object_unref(task);
}
static void kill_alert(GSubprocess **process) {
    if (!*process)
        return;
    const char *id = g_subprocess_get_identifier(*process);
    if (id) {
        long pid = strtol(id, NULL, 10);
        if (pid > 1)
            kill(-(pid_t)pid, SIGTERM);
    }
    g_clear_object(process);
}
static void child_setup(gpointer data) {
    (void)data;
    setsid();
}
static void child_waited(GObject *object, GAsyncResult *res, gpointer data) {
    (void)data;
    g_subprocess_wait_finish(G_SUBPROCESS(object), res, NULL);
    g_object_unref(object);
}
static GSubprocess *alert_spawn(const char *const *args) {
    GSubprocessLauncher *launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                                              G_SUBPROCESS_FLAGS_STDERR_SILENCE);
    g_subprocess_launcher_set_child_setup(launcher, child_setup, NULL, NULL);
    GSubprocess *p = g_subprocess_launcher_spawnv(launcher, args, NULL);
    g_object_unref(launcher);
    if (p)
        g_subprocess_wait_async(p, NULL, child_waited, g_object_ref(p));
    return p;
}
static bool child_alive(GSubprocess *p) {
    const char *id = p ? g_subprocess_get_identifier(p) : NULL;
    return id && kill((pid_t)strtol(id, NULL, 10), 0) == 0;
}
static bool live_state(const char *state) {
    return !strcmp(state, "dialing") || !strcmp(state, "ringing-out") || !strcmp(state, "active") ||
           !strcmp(state, "held");
}
static void ring(App *a);
static gboolean ring_tick(gpointer data) {
    App *a = data;
    a->ring_timer = 0;
    a->next_ring = 0;
    ring(a);
    return G_SOURCE_REMOVE;
}
static void ring(App *a) {
    bool ringing = false, live = false;
    if (a->valid)
        for (size_t i = 0; i < json_object_array_length(a->calls); i++) {
            const char *s = jstr(json_object_array_get_idx(a->calls, i), "state", "");
            ringing |= !strcmp(s, "ringing-in") || !strcmp(s, "waiting");
            live |= live_state(s);
        }
    if (!ringing) {
        kill_alert(&a->sound);
        kill_alert(&a->vibration);
        a->next_ring = 0;
        if (a->ring_timer) {
            g_source_remove(a->ring_timer);
            a->ring_timer = 0;
        }
        return;
    }
    if (!a->ring_sound || live || jbool(a->status, "active", false))
        kill_alert(&a->sound);
    if (!a->ring_vibrate)
        kill_alert(&a->vibration);
    if (monotime() < a->next_ring)
        return;
    a->next_ring = monotime() + 2.5;
    if (!a->ring_timer)
        a->ring_timer = g_timeout_add(2500, ring_tick, a);
    if (a->ring_sound && !live && !jbool(a->status, "active", false) && !child_alive(a->sound)) {
        g_clear_object(&a->sound);
        const char *args[] = {"/usr/libexec/e5-notification-sound", "ring", a->ring_tone, NULL};
        a->sound = alert_spawn(args);
    }
    if (a->ring_vibrate && !child_alive(a->vibration)) {
        g_clear_object(&a->vibration);
        const char *args[] = {"e5-vibrate", "250", "150", "250", NULL};
        a->vibration = alert_spawn(args);
    }
}
static const char *default_settings = "{\"speaker\":false,\"muted\":false}";
static void publish(App *a) {
    Json *value = json_object_new_object();
    if (a->status) {
        json_object_object_foreach(a->status, key, v) {
            json_object_object_add(value, key, json_object_get(v));
        }
    } else {
        jb(value, "available", false);
        jb(value, "active", false);
        js(value, "error", "Audio firmware is starting");
    }
    if (a->observer_error)
        js(value, "error", a->observer_error);
    else if (a->settings_error)
        js(value, "error", a->settings_error);
    else if (a->config_error)
        js(value, "error", a->config_error);
    else if (a->route_error && !jget(value, "error"))
        js(value, "error", a->route_error);
    json_object_object_add(value, "calls", json_object_get(a->calls));
    jb(value, "observer_ready", a->valid);
    json_object_object_add(value, "bluetooth",
                           a->bt_status ? json_object_get(a->bt_status) : json_object_new_object());
    jb(value, "route_pending", a->bt_working && !a->lease);
    jd(value, "updated", realtime());
    write_json(RUN "status.json", value);
    screen_http_publish(a->http, value, a->ring_screen);
    json_object_put(value);
}
typedef struct {
    App *app;
    AudioRequest request;
    char *route;
    Json *settings;
    bool reload_config;
} AudioJob;
static char *uci_value(const char *text, const char *key) {
    char **lines = g_strsplit(text ? text : "", "\n", -1);
    char *out = NULL;
    for (unsigned i = 0; lines[i]; i++) {
        char *eq = strchr(lines[i], '=');
        if (!eq)
            continue;
        *eq = 0;
        const char *name = strrchr(lines[i], '.');
        const char *section = strstr(lines[i], ".main.");
        if (name && section && section + 6 == name + 1 && !strcmp(name + 1, key)) {
            const char *v = eq + 1;
            size_t n = strlen(v);
            out =
                n >= 2 && v[0] == '\'' && v[n - 1] == '\'' ? g_strndup(v + 1, n - 2) : g_strdup(v);
            break;
        }
    }
    g_strfreev(lines);
    return out;
}
static char *read_config(const char *package, GError **error) {
    const char *args[] = {"uci", "-q", "show", package, NULL};
    int status = 0;
    char *data = command(args, 3, NULL, &status, error);
    if (error && *error)
        return data;
    if (status) {
        char *suffix = g_strconcat("/etc/config/", package, NULL);
        char *path = native_path(suffix);
        bool exists = g_file_test(path, G_FILE_TEST_EXISTS);
        g_free(path);
        g_free(suffix);
        suffix = g_strconcat("/tmp/.uci/", package, NULL);
        path = native_path(suffix);
        exists |= g_file_test(path, G_FILE_TEST_EXISTS);
        g_free(path);
        g_free(suffix);
        /* Missing packages retain their established defaults. A present but
         * unreadable/malformed package must not overwrite the user's choices. */
        if (exists)
            g_set_error(error, domain(), 1, "Cannot read %s configuration (uci exit %d)", package,
                        status);
    }
    return data;
}
static Json *config(GError **error) {
    Json *out = json_object_new_object();
    ji(out, "volume", 15);
    jb(out, "sound", true);
    jb(out, "screen", true);
    jb(out, "vibrate", true);
    js(out, "tone", "call");
    char *data = read_config("e5-audio", error);
    if (error && *error) {
        g_free(data);
        json_object_put(out);
        return NULL;
    }
    char *volume = uci_value(data, "call_volume");
    g_free(data);
    if (volume) {
        char *end;
        long v = strtol(volume, &end, 10);
        if (*volume && !*end)
            ji(out, "volume", CLAMP(v, 0, 15));
        g_free(volume);
    }
    data = read_config("e5-plugin-phone", error);
    if (error && *error) {
        g_free(data);
        json_object_put(out);
        return NULL;
    }
    const char *keys[] = {"ring_sound", "ring_screen", "ring_vibrate", "ring_tone"},
               *fields[] = {"sound", "screen", "vibrate", "tone"};
    for (unsigned i = 0; i < 4; i++) {
        char *v = uci_value(data, keys[i]);
        if (v) {
            if (i == 3)
                js(out, fields[i], !strcmp(v, "beep") || !strcmp(v, "custom") ? v : "call");
            else
                jb(out, fields[i], i == 1 ? strcmp(v, "0") != 0 : !strcmp(v, "1"));
            g_free(v);
        }
    }
    g_free(data);
    return out;
}
static void audio_worker(GTask *task, gpointer object, gpointer data, GCancellable *cancel) {
    (void)object;
    (void)cancel;
    AudioJob *job = data;
    App *a = job->app;
    GError *error = NULL;
    Json *result = json_object_new_object();
    if (job->reload_config) {
        Json *settings = config(&error);
        if (settings) {
            json_object_object_add(result, "config", settings);
            job->request.volume = jint(settings, "volume", 15);
        } else {
            js(result, "config_error", error ? error->message : "Cannot read audio configuration");
            g_clear_error(&error);
        }
    }
    if (!a->hardware) {
        char *ready = native_path("/run/e5-audio.ready");
        bool initialized = g_file_test(ready, G_FILE_TEST_EXISTS);
        g_free(ready);
        if (initialized)
            a->hardware = audio_new(&error);
        else
            g_set_error_literal(&error, domain(), 1, "Audio firmware is starting");
    }
    Json *state = NULL;
    if (a->hardware) {
        bool was_active = jbool(a->status, "active", false),
             new_card = job->request.live && job->request.card >= 0 &&
                        jint(a->status, "card", -1) >= 0 &&
                        job->request.card != jint(a->status, "card", -1);
        if (job->request.live && (!was_active || new_card))
            media(true);
        state = audio_apply(a->hardware, &job->request, &error);
        if (!jbool(state, "active", false) && (was_active || job->request.live))
            media(false);
    } else {
        state = json_object_new_object();
        jb(state, "available", false);
        jb(state, "active", false);
        js(state, "error", error ? error->message : "Audio unavailable");
    }
    g_clear_error(&error);
    json_object_object_add(result, "state", state);
    g_task_return_pointer(task, result, json_free);
}
static void job_free(AudioJob *job) {
    g_free(job->route);
    if (job->settings)
        json_object_put(job->settings);
    g_free(job);
}
static gboolean retry_audio(gpointer data) {
    App *a = data;
    a->retry = 0;
    update(a);
    return G_SOURCE_REMOVE;
}
static void schedule_audio_retry(App *a, bool failed) {
    bool live = false;
    for (size_t i = 0; i < json_object_array_length(a->calls); i++)
        live |= live_state(jstr(json_object_array_get_idx(a->calls, i), "state", ""));
    /* Retry audio errors without restoring idle MM/AT polling. */
    if (failed && a->hardware_ready && a->valid && live && !a->retry)
        a->retry = g_timeout_add(1000, retry_audio, a);
}
static void audio_done(GObject *object, GAsyncResult *res, gpointer data) {
    (void)object;
    App *a = data;
    Json *result = g_task_propagate_pointer(G_TASK(res), NULL);
    bool before = jbool(a->status, "active", false);
    a->working = false;
    a->hardware_ready = a->hardware != NULL;
    Json *settings = jget(result, "config");
    if (jget(result, "config_error")) {
        g_free(a->config_error);
        a->config_error = g_strdup(jstr(result, "config_error", "Configuration unavailable"));
        a->config_dirty = true;
    } else if (settings)
        g_clear_pointer(&a->config_error, g_free);
    if (settings) {
        a->volume = jint(settings, "volume", 15);
        a->ring_sound = jbool(settings, "sound", true);
        a->ring_screen = jbool(settings, "screen", true);
        a->ring_vibrate = jbool(settings, "vibrate", true);
        g_free(a->ring_tone);
        a->ring_tone = g_strdup(jstr(settings, "tone", "call"));
    }
    if (a->status)
        json_object_put(a->status);
    a->status = json_object_get(jget(result, "state"));
    json_object_put(result);
    bool failed = jget(a->status, "error") != NULL;
    if (failed && !jbool(a->status, "active", false) && (a->lease || a->bt_cancel)) {
        /* A failed PCM start must release SCO and the A2DP suspension too.
         * Retry local PCM first, then negotiate Bluetooth after recovery. */
        a->bt_retry_after_pcm = true;
        g_free(a->failed_key);
        a->failed_key = g_strdup(a->desired_key);
        bt_release(a);
    } else if (!failed && jbool(a->status, "active", false) && a->bt_retry_after_pcm) {
        a->bt_retry_after_pcm = false;
        if (!g_strcmp0(a->failed_key, a->desired_key)) {
            g_clear_pointer(&a->failed_key, g_free);
            a->dirty = true;
        }
    }
    if (before && !jbool(a->status, "active", false)) {
        Json *defaults = json_tokener_parse(default_settings);
        write_json(RUN "settings.json", defaults);
        json_object_put(defaults);
        a->headset_intent = 0;
        g_clear_pointer(&a->failed_key, g_free);
    }
    if (!a->closing) {
        ring(a);
        publish(a);
        schedule_audio_retry(a, failed);
        if (a->dirty)
            update(a);
    }
}
static void update(App *a) {
    if (a->closing)
        return;
    if (a->retry) {
        g_source_remove(a->retry);
        a->retry = 0;
    }
    a->dirty = true;
    if (a->working || !a->valid)
        return;
    Json *settings = read_json(RUN "settings.json");
    if (!settings || !json_object_is_type(settings, json_type_object)) {
        if (settings)
            json_object_put(settings);
        g_free(a->settings_error);
        a->settings_error = g_strdup("Cannot read audio settings");
        publish(a);
        return;
    }
    g_clear_pointer(&a->settings_error, g_free);
    Json *bt = read_json("/run/e5-hfp/status.json");
    if (!bt || realtime() - jnum(bt, "updated", 0) < 0 ||
        realtime() - jnum(bt, "updated", 0) >= 5) {
        if (bt)
            json_object_put(bt);
        bt = json_object_new_object();
        jb(bt, "connected", false);
    }
    if (a->bt_status)
        json_object_put(a->bt_status);
    a->bt_status = bt;
    Json *chosen = NULL;
    for (size_t i = 0; i < json_object_array_length(a->calls); i++) {
        Json *call = json_object_array_get_idx(a->calls, i);
        const char *state = jstr(call, "state", "");
        if (live_state(state) && (!chosen || !strcmp(state, "active")))
            chosen = call;
    }
    const char *requested =
        jstr(settings, "route", jbool(settings, "speaker", false) ? "speaker" : "auto");
    double stamp = jnum(settings, "route_updated", 0);
    Json *intent = read_json("/run/e5-hfp/intent.json");
    if (intent && realtime() - jnum(intent, "updated", 0) < 10)
        for (size_t i = 0; i < json_object_array_length(a->calls); i++) {
            Json *call = json_object_array_get_idx(a->calls, i);
            if (live_state(jstr(call, "state", "")) &&
                !strcmp(jstr(intent, "call", ""), jstr(call, "id", "?")))
                a->headset_intent = MAX(a->headset_intent, jnum(intent, "updated", 0));
        }
    if (intent)
        json_object_put(intent);
    if (a->headset_intent > stamp)
        requested = "bluetooth";
    bool connected = jbool(bt, "connected", false);
    const char *route =
        !strcmp(requested, "auto") ? (connected ? "bluetooth" : "receiver") : requested;
    if (strcmp(route, "receiver") && strcmp(route, "speaker") && strcmp(route, "bluetooth"))
        route = "receiver";
    char *key = g_strdup_printf("%s|%s|%.9f|%.9f", jstr(bt, "connection", ""), requested, stamp,
                                a->headset_intent);
    g_free(a->desired_key);
    a->desired_key = key;
    if (chosen && !strcmp(route, "bluetooth")) {
        if (!connected || (a->failed_key && !strcmp(a->failed_key, key))) {
            bt_release(a);
            route = "receiver";
            if (!strcmp(requested, "bluetooth") && !a->route_error)
                a->route_error = g_strdup("Bluetooth call audio unavailable; using phone");
        } else {
            bt_acquire(a);
            if (!a->lease) {
                route = jstr(a->status, "route", "receiver");
                if (!strcmp(route, "bluetooth"))
                    route = "receiver";
            }
        }
    } else {
        bt_release(a);
        g_clear_pointer(&a->route_error, g_free);
        if (!chosen) {
            a->headset_intent = 0;
            a->bt_retry_after_pcm = false;
            g_clear_pointer(&a->failed_key, g_free);
        }
    }
    AudioJob *job = g_new0(AudioJob, 1);
    job->app = a;
    job->settings = settings;
    job->route = g_strdup(route);
    job->request = (AudioRequest){.live = chosen != NULL,
                                  .card = chosen ? jint(chosen, "card", -1) : -1,
                                  .volume = a->volume,
                                  .muted = jbool(settings, "muted", false),
                                  .route = job->route,
                                  .rate = a->lease ? a->lease->rate : 8000,
                                  .remote_volume = jbool(bt, "remote_volume", false),
                                  .band = a->band};
    job->reload_config = a->config_dirty;
    a->config_dirty = false;
    a->dirty = false;
    a->working = true;
    GTask *task = g_task_new(NULL, NULL, audio_done, a);
    g_task_set_task_data(task, job, (GDestroyNotify)job_free);
    g_task_run_in_thread(task, audio_worker);
    g_object_unref(task);
}
static Json *variant(GVariant *v, unsigned depth) {
    if (!v || depth > 24)
        return NULL;
    const char *type = g_variant_get_type_string(v);
    if (strchr("sog", *type))
        return json_object_new_string(g_variant_get_string(v, NULL));
    if (*type == 'v') {
        GVariant *child = g_variant_get_variant(v);
        Json *out = variant(child, depth + 1);
        g_variant_unref(child);
        return out;
    }
    if (*type == 'b')
        return json_object_new_boolean(g_variant_get_boolean(v));
    if (*type == 'u')
        return json_object_new_int64(g_variant_get_uint32(v));
    if (*type == 'i')
        return json_object_new_int(g_variant_get_int32(v));
    if (*type == 't')
        return json_object_new_uint64(g_variant_get_uint64(v));
    if (*type == 'x')
        return json_object_new_int64(g_variant_get_int64(v));
    if (*type == 'y')
        return json_object_new_int(g_variant_get_byte(v));
    if (*type == 'n')
        return json_object_new_int(g_variant_get_int16(v));
    if (*type == 'q')
        return json_object_new_int(g_variant_get_uint16(v));
    if (*type == 'd')
        return json_object_new_double(g_variant_get_double(v));
    if (g_str_has_prefix(type, "a{")) {
        Json *out = json_object_new_object();
        for (gsize i = 0; i < g_variant_n_children(v); i++) {
            GVariant *pair = g_variant_get_child_value(v, i),
                     *key = g_variant_get_child_value(pair, 0),
                     *value = g_variant_get_child_value(pair, 1);
            json_object_object_add(out, g_variant_get_string(key, NULL), variant(value, depth + 1));
            g_variant_unref(key);
            g_variant_unref(value);
            g_variant_unref(pair);
        }
        return out;
    }
    if (*type == 'a' || *type == '(') {
        Json *out = json_object_new_array();
        for (gsize i = 0; i < g_variant_n_children(v); i++) {
            GVariant *child = g_variant_get_child_value(v, i);
            json_object_array_add(out, variant(child, depth + 1));
            g_variant_unref(child);
        }
        return out;
    }
    return NULL;
}
static void observed(App *a) {
    const char *modem = NULL;
    json_object_object_foreach(a->objects, path, interfaces) {
        if (jget(interfaces, VOICE) && (!modem || strcmp(path, modem) < 0))
            modem = path;
    }
    if (!modem) {
        a->valid = false;
        g_free(a->observer_error);
        a->observer_error = g_strdup("No voice modem available");
        publish(a);
        return;
    }
    Json *paths = jget(jget(jget(a->objects, modem), VOICE), "Calls"),
         *calls = json_object_new_array();
    if (paths && !json_object_is_type(paths, json_type_array)) {
        json_object_put(calls);
        return;
    }
    static const char *states[] = {"unknown", "dialing", "ringing-out", "ringing-in",
                                   "active",  "held",    "waiting",     "terminated"};
    static const char *reasons[] = {"unknown",  "outgoing-started",   "incoming-new",
                                    "accepted", "terminated",         "refused-or-busy",
                                    "error",    "audio-setup-failed", "transferred",
                                    "deflected"};
    for (size_t i = 0; paths && i < json_object_array_length(paths); i++) {
        const char *path = json_object_get_string(json_object_array_get_idx(paths, i));
        if (!path || !g_str_has_prefix(path, CALL) || !*(path + strlen(CALL)))
            continue;
        const char *id = path + strlen(CALL);
        bool numeric = true;
        for (const char *p = id; *p; p++)
            if (!g_ascii_isdigit(*p))
                numeric = false;
        if (!numeric)
            continue;
        Json *p = jget(jget(a->objects, path), CALL_IF);
        if (!p || !jget(p, "State") || !jget(p, "Direction")) {
            json_object_put(calls);
            a->valid = false;
            g_free(a->observer_error);
            a->observer_error = g_strdup("Waiting for complete call state");
            publish(a);
            /* MM exports Call objects separately from its managed modem
             * objects. Repeating GetManagedObjects cannot fill this gap. */
            load_call(a, path);
            return;
        }
        int state = jint(p, "State", 0), reason = jint(p, "StateReason", 0),
            slot = jint(p, "SimSlot", 0);
        if (state == 7)
            continue;
        Json *call = json_object_new_object();
        js(call, "id", id);
        js(call, "state", state >= 0 && state < 8 ? states[state] : "unknown");
        js(call, "reason", reason >= 0 && reason < 10 ? reasons[reason] : "unknown");
        js(call, "number", jstr(p, "Number", ""));
        int direction = jint(p, "Direction", 0);
        js(call, "direction",
           direction == 1   ? "incoming"
           : direction == 2 ? "outgoing"
                            : "unknown");
        if (slot == 1 || slot == 2) {
            ji(call, "card", slot - 1);
            js(call, "sim", slot == 1 ? "SIM1" : "SIM2");
        } else {
            json_object_object_add(call, "card", NULL);
            js(call, "sim", NULL);
        }
        json_object_array_add(calls, call);
    }
    if (a->calls)
        json_object_put(a->calls);
    a->calls = calls;
    a->valid = true;
    g_clear_pointer(&a->observer_error, g_free);
    ring(a);
    update(a);
    publish(a);
}
typedef struct {
    App *app;
    guint epoch, revision;
    char *path;
} CallRead;
static void call_done(GObject *object, GAsyncResult *res, gpointer data) {
    CallRead *read = data;
    App *a = read->app;
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(object), res, &error);
    if (g_hash_table_lookup(a->call_reads, read->path) == read)
        g_hash_table_remove(a->call_reads, read->path);
    if (a->closing || read->epoch != a->epoch)
        goto out;
    if (read->revision != a->object_revision) {
        /* A signal or newer modem snapshot overtook this read. Never put
         * an old ringing/active state back over a newer call transition. */
        observed(a);
        goto out;
    }
    if (error || !reply) {
        /* The existing bounded recovery timer retries incomplete state;
         * an unavailable Call must not create an immediate request loop. */
        g_free(a->observer_error);
        a->observer_error = g_strdup("Call properties unavailable");
        publish(a);
        goto out;
    }
    GVariant *value = g_variant_get_child_value(reply, 0);
    Json *properties = variant(value, 0);
    g_variant_unref(value);
    if (!properties || !json_object_is_type(properties, json_type_object) ||
        !jget(properties, "State") || !jget(properties, "Direction")) {
        json_free(properties);
        goto out;
    }
    Json *interfaces = jget(a->objects, read->path);
    if (!interfaces) {
        interfaces = json_object_new_object();
        json_object_object_add(a->objects, read->path, interfaces);
    }
    json_object_object_add(interfaces, CALL_IF, properties);
    observed(a);
out:
    if (reply)
        g_variant_unref(reply);
    g_clear_error(&error);
    g_free(read->path);
    g_free(read);
}
static void load_call(App *a, const char *path) {
    if (a->closing || !a->bus || !a->owner || !*a->owner ||
        g_hash_table_contains(a->call_reads, path))
        return;
    CallRead *read = g_new0(CallRead, 1);
    read->app = a;
    read->epoch = a->epoch;
    read->revision = a->object_revision;
    read->path = g_strdup(path);
    g_hash_table_insert(a->call_reads, g_strdup(path), read);
    g_dbus_connection_call(a->bus, a->owner, path, "org.freedesktop.DBus.Properties",
                          "GetAll", g_variant_new("(s)", CALL_IF),
                          G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NO_AUTO_START,
                          3000, NULL, call_done, read);
}
static void event_free(Event *e) {
    g_free(e->path);
    g_free(e->member);
    g_variant_unref(e->value);
    g_free(e);
}
static void apply_event(App *a, Event *event) {
    a->object_revision++;
    GVariant *v = event->value;
    if (!strcmp(event->member, "InterfacesAdded")) {
        const char *path;
        GVariant *interfaces;
        g_variant_get(v, "(&o@a{sa{sv}})", &path, &interfaces);
        Json *properties = variant(interfaces, 0);
        g_variant_unref(interfaces);
        Json *old = jget(a->objects, path);
        if (!old) {
            json_object_object_add(a->objects, path, properties);
        } else {
            json_object_object_foreach(properties, key, value) {
                json_object_object_add(old, key, json_object_get(value));
            }
            json_object_put(properties);
        }
    } else if (!strcmp(event->member, "InterfacesRemoved")) {
        const char *path;
        GVariant *removed;
        g_variant_get(v, "(&o@as)", &path, &removed);
        Json *old = jget(a->objects, path);
        GVariantIter iter;
        const char *name;
        g_variant_iter_init(&iter, removed);
        while (g_variant_iter_next(&iter, "&s", &name))
            if (old)
                json_object_object_del(old, name);
        g_variant_unref(removed);
    } else {
        const char *interface;
        GVariant *changed, *invalid;
        g_variant_get(v, "(&s@a{sv}@as)", &interface, &changed, &invalid);
        Json *interfaces = jget(a->objects, event->path);
        if (!interfaces) {
            interfaces = json_object_new_object();
            json_object_object_add(a->objects, event->path, interfaces);
        }
        Json *properties = jget(interfaces, interface);
        if (!properties) {
            properties = json_object_new_object();
            json_object_object_add(interfaces, interface, properties);
        }
        Json *update = variant(changed, 0);
        json_object_object_foreach(update, key, value) {
            json_object_object_add(properties, key, json_object_get(value));
        }
        json_object_put(update);
        if (g_variant_n_children(invalid))
            a->overflow = true;
        g_variant_unref(changed);
        g_variant_unref(invalid);
    }
}
typedef struct {
    App *app;
    unsigned epoch;
    char *owner;
} Snapshot;
static void snapshot_done(GObject *object, GAsyncResult *res, gpointer data) {
    Snapshot *s = data;
    App *a = s->app;
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(object), res, &error);
    if (s->epoch != a->epoch || a->closing)
        goto out;
    a->loading = false;
    if (error || !reply) {
        a->valid = false;
        g_free(a->observer_error);
        a->observer_error = g_strdup("ModemManager snapshot unavailable");
        publish(a);
        goto out;
    }
    GVariant *objects = g_variant_get_child_value(reply, 0);
    Json *current = variant(objects, 0);
    g_variant_unref(objects);
    if (!current || !json_object_is_type(current, json_type_object)) {
        if (current)
            json_object_put(current);
        goto out;
    }
    if (a->objects)
        json_object_put(a->objects);
    a->object_revision++;
    a->objects = current;
    while (!g_queue_is_empty(&a->events)) {
        Event *event = g_queue_pop_head(&a->events);
        apply_event(a, event);
        event_free(event);
    }
    if (a->overflow) {
        a->overflow = false;
        load_objects(a);
    } else
        observed(a);
out:
    if (reply)
        g_variant_unref(reply);
    g_clear_error(&error);
    g_free(s->owner);
    g_free(s);
}
static void load_objects(App *a) {
    if (a->loading || !a->bus || !a->owner || !*a->owner || a->closing)
        return;
    a->loading = true;
    Snapshot *snapshot = g_new0(Snapshot, 1);
    snapshot->app = a;
    snapshot->epoch = a->epoch;
    snapshot->owner = g_strdup(a->owner);
    g_dbus_connection_call(a->bus, a->owner, ROOT, "org.freedesktop.DBus.ObjectManager",
                           "GetManagedObjects", NULL, G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
                           G_DBUS_CALL_FLAGS_NO_AUTO_START, 3000, NULL, snapshot_done, snapshot);
}
static void owner(App *a, const char *name) {
    a->epoch++;
    a->loading = false;
    a->valid = false;
    g_free(a->owner);
    a->owner = g_strdup(name);
    g_free(a->observer_error);
    a->observer_error = g_strdup("ModemManager is reconnecting");
    while (!g_queue_is_empty(&a->events))
        event_free(g_queue_pop_head(&a->events));
    a->overflow = false;
    ring(a);
    publish(a);
    if (*name)
        load_objects(a);
}
static void signal_received(GDBusConnection *connection, const gchar *sender, const gchar *path,
                            const gchar *interface, const gchar *member, GVariant *parameters,
                            gpointer data) {
    (void)connection;
    App *a = data;
    if (a->closing)
        return;
    if (!strcmp(interface, "org.freedesktop.DBus") && !strcmp(sender, "org.freedesktop.DBus") &&
        !strcmp(member, "NameOwnerChanged")) {
        const char *name, *old, *next;
        g_variant_get(parameters, "(&s&s&s)", &name, &old, &next);
        (void)old;
        if (!strcmp(name, MM))
            owner(a, next);
        return;
    }
    if (!a->owner || strcmp(sender, a->owner) || !g_str_has_prefix(path, ROOT))
        return;
    if (strcmp(member, "PropertiesChanged") && strcmp(member, "InterfacesAdded") &&
        strcmp(member, "InterfacesRemoved"))
        return;
    Event *event = g_new0(Event, 1);
    event->path = g_strdup(path);
    event->member = g_strdup(member);
    event->value = g_variant_ref(parameters);
    if (a->loading) {
        if (g_queue_get_length(&a->events) < 256)
            g_queue_push_tail(&a->events, event);
        else {
            event_free(event);
            a->overflow = true;
        }
        return;
    }
    apply_event(a, event);
    event_free(event);
    if (a->overflow) {
        a->overflow = false;
        load_objects(a);
    } else
        observed(a);
}
static void initial_owner(GObject *object, GAsyncResult *res, gpointer data) {
    Snapshot *s = data;
    App *a = s->app;
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(object), res, &error);
    if (reply && a->epoch == s->epoch && !a->closing) {
        const char *name;
        g_variant_get(reply, "(&s)", &name);
        owner(a, name);
    }
    if (reply)
        g_variant_unref(reply);
    g_clear_error(&error);
    g_free(s);
}
static void connected(GObject *object, GAsyncResult *res, gpointer data) {
    (void)object;
    App *a = data;
    a->bus_connecting = false;
    GError *error = NULL;
    a->bus = g_bus_get_finish(res, &error);
    if (!a->bus) {
        g_clear_error(&error);
        return;
    }
    g_dbus_connection_set_exit_on_close(a->bus, FALSE);
    g_dbus_connection_signal_subscribe(a->bus, "org.freedesktop.DBus", "org.freedesktop.DBus",
                                       "NameOwnerChanged", "/org/freedesktop/DBus", MM, 0,
                                       signal_received, a, NULL);
    g_dbus_connection_signal_subscribe(a->bus, NULL, "org.freedesktop.DBus.ObjectManager", NULL,
                                       ROOT, NULL, 0, signal_received, a, NULL);
    g_dbus_connection_signal_subscribe(a->bus, NULL, "org.freedesktop.DBus.Properties",
                                       "PropertiesChanged", NULL, NULL, 0, signal_received, a,
                                       NULL);
    Snapshot *s = g_new0(Snapshot, 1);
    s->app = a;
    s->epoch = a->epoch;
    g_dbus_connection_call(a->bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                           "org.freedesktop.DBus", "GetNameOwner", g_variant_new("(s)", MM),
                           G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 3000, NULL,
                           initial_owner, s);
}
static void watches(App *a) {
    const char *paths[] = {RUN, "/run/e5-hfp", "/run", "/etc/config", "/tmp/.uci", "/tmp"};
    for (unsigned i = 0; i < G_N_ELEMENTS(paths); i++) {
        char *path = native_path(paths[i]);
        inotify_add_watch(a->notify, path,
                          IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE | IN_DELETE_SELF |
                              IN_MOVE_SELF);
        g_free(path);
    }
}
static gboolean changed(gint fd, GIOCondition condition, gpointer data) {
    (void)condition;
    App *a = data;
    union {
        struct inotify_event align;
        char bytes[16384];
    } input;
    ssize_t n;
    bool update_needed = false;
    while ((n = read(fd, input.bytes, sizeof(input.bytes))) > 0)
        for (ssize_t pos = 0; pos < n;) {
            struct inotify_event *e = (void *)(input.bytes + pos);
            pos += (ssize_t)sizeof(*e) + e->len;
            if (!e->len) {
                watches(a);
                continue;
            }
            if (!strcmp(e->name, "e5-audio") || !strcmp(e->name, "e5-plugin-phone")) {
                a->config_dirty = true;
                update_needed = true;
            }
            if (!strcmp(e->name, "settings.json") || !strcmp(e->name, "intent.json") ||
                !strcmp(e->name, "status.json") || !strcmp(e->name, "e5-audio.ready")) {
                /* Our status output must not feed back into its own observer. */
                char *hfp = native_path("/run/e5-hfp");
                int hfp_watch = inotify_add_watch(fd, hfp,
                                                  IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE |
                                                      IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF);
                g_free(hfp);
                if (strcmp(e->name, "status.json") || e->wd == hfp_watch)
                    update_needed = true;
            }
            if (!strcmp(e->name, "e5-hfp") || !strcmp(e->name, ".uci")) {
                watches(a);
                a->config_dirty = true;
                update_needed = true;
            }
        }
    if (update_needed)
        update(a);
    return G_SOURCE_CONTINUE;
}
/* This vendor character driver has no .poll and ignores O_NONBLOCK on
 * reads (audiopipe/audio-pipe.c). A sleeping reader must never run on the
 * main loop. Only its bounded pipe is polled; there is no sampling timer. */
static void close_reader(gpointer data) {
    int fd = *(int *)data;
    if (fd >= 0)
        close(fd);
}
static void *dsp_reader(void *data) {
    App *a = data;
    char *path = native_path("/dev/audio_pipe_voice");
    int fd = open(path, O_RDWR | O_CLOEXEC);
    g_free(path);
    pthread_cleanup_push(close_reader, &fd);
    if (fd >= 0)
        for (;;) {
            unsigned char message[20];
            ssize_t n = read(fd, message, sizeof(message));
            if (n < 0 && errno == EINTR)
                continue;
            if (n != (ssize_t)sizeof(message))
                break;
            if (write(a->dsp_write, message, sizeof(message)) != (ssize_t)sizeof(message))
                break;
        }
    pthread_cleanup_pop(1);
    /* The main loop also owns this FD so shutdown never races a descriptor
     * being reused. The callback joins us before closing either pipe end. */
    unsigned char ended[20] = {0xff};
    ssize_t sent = write(a->dsp_write, ended, sizeof(ended));
    if (sent != (ssize_t)sizeof(ended))
        g_warning("Voice reader exit notification failed");
    return NULL;
}
static gboolean dsp(gint fd, GIOCondition condition, gpointer data) {
    (void)condition;
    App *a = data;
    ssize_t n;
    while ((n = read(fd, a->dsp_partial + a->dsp_size, 20 - a->dsp_size)) > 0) {
        a->dsp_size += (size_t)n;
        if (a->dsp_size < 20)
            continue;
        if (a->dsp_partial[0] == 0xff) {
            pthread_join(a->dsp_thread, NULL);
            a->dsp_started = false;
            a->dsp_source = 0;
            close(a->dsp);
            close(a->dsp_write);
            a->dsp = a->dsp_write = -1;
            a->dsp_size = 0;
            g_warning("Voice DSP notification reader disconnected");
            return G_SOURCE_REMOVE;
        }
        guint16 command, channel;
        guint32 band;
        memcpy(&command, a->dsp_partial, 2);
        memcpy(&channel, a->dsp_partial + 2, 2);
        memcpy(&band, a->dsp_partial + 8, 4);
        if (GUINT16_FROM_LE(command) == 0 && GUINT16_FROM_LE(channel) == 2 &&
            GUINT32_FROM_LE(band) < 4) {
            int b = (int)GUINT32_FROM_LE(band);
            if (a->band != b) {
                a->band = b;
                update(a);
            }
        }
        a->dsp_size = 0;
    }
    return G_SOURCE_CONTINUE;
}
static void start_dsp(App *a) {
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) < 0)
        return;
    a->dsp = fds[0];
    a->dsp_write = fds[1];
    fcntl(a->dsp, F_SETFL, O_NONBLOCK);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    int rc = pthread_create(&a->dsp_thread, &attr, dsp_reader, a);
    pthread_attr_destroy(&attr);
    if (rc) {
        close(a->dsp);
        close(a->dsp_write);
        a->dsp = a->dsp_write = -1;
        return;
    }
    a->dsp_started = true;
    a->dsp_source = g_unix_fd_add(a->dsp, G_IO_IN | G_IO_ERR | G_IO_HUP, dsp, a);
}
static Json *power_snapshot(void) {
    Json *out = json_object_new_object();
    char *pattern = native_path("/sys/class/power_supply/*");
    glob_t list = {0};
    if (glob(pattern, 0, NULL, &list) == 0)
        for (size_t i = 0; i < list.gl_pathc; i++) {
            const char *root = g_getenv("E5_NATIVE_ROOT");
            const char *suffix = list.gl_pathv[i] + (root ? strlen(root) : 0);
            char *type_path = g_strconcat(suffix, "/type", NULL), *type = read_text(type_path, 64);
            g_free(type_path);
            if (type && (!strcmp(g_strstrip(type), "Mains") || !strcmp(type, "Wireless") ||
                         g_str_has_prefix(type, "USB"))) {
                char *online_path = g_strconcat(suffix, "/online", NULL),
                     *online = read_text(online_path, 16);
                g_free(online_path);
                if (online) {
                    const char *name = strrchr(suffix, '/') + 1;
                    jb(out, name, !strcmp(g_strstrip(online), "1"));
                    g_free(online);
                }
            }
            g_free(type);
        }
    globfree(&list);
    g_free(pattern);
    return out;
}
static gboolean power_event(gint fd, GIOCondition condition, gpointer data) {
    (void)condition;
    App *a = data;
    char bytes[16384];
    struct sockaddr_nl from;
    socklen_t size;
    ssize_t n;
    while (size = sizeof(from),
           (n = recvfrom(fd, bytes, sizeof(bytes) - 1, 0, (void *)&from, &size)) > 0) {
        if (from.nl_pid)
            continue;
        bytes[n] = 0;
        const char *subsystem = NULL, *name = NULL, *online = NULL;
        for (ssize_t i = 0; i < n; i += (ssize_t)strlen(bytes + i) + 1) {
            if (g_str_has_prefix(bytes + i, "SUBSYSTEM="))
                subsystem = bytes + i + 10;
            if (g_str_has_prefix(bytes + i, "POWER_SUPPLY_NAME="))
                name = bytes + i + 18;
            if (g_str_has_prefix(bytes + i, "POWER_SUPPLY_ONLINE="))
                online = bytes + i + 20;
        }
        if (!subsystem || strcmp(subsystem, "power_supply"))
            continue;
        Json *current = power_snapshot();
        if (name && online && jget(current, name) && (!strcmp(online, "0") || !strcmp(online, "1")))
            jb(current, name, !strcmp(online, "1"));
        bool changed = false;
        json_object_object_foreach(current, key, value) {
            if (jget(a->power, key) &&
                json_object_get_boolean(value) != jbool(a->power, key, false))
                changed = true;
        }
        if (a->power)
            json_object_put(a->power);
        a->power = current;
        if (changed)
            screen_http_power(a->http);
    }
    return G_SOURCE_CONTINUE;
}
static gboolean heartbeat(gpointer data) {
    App *a = data;
    if (a->closing)
        return G_SOURCE_REMOVE;
    if (!a->valid && !a->loading && !g_hash_table_size(a->call_reads) &&
        a->owner && *a->owner)
        load_objects(a);
    char *ready = native_path("/run/e5-audio.ready");
    bool audio_ready = g_file_test(ready, G_FILE_TEST_EXISTS);
    g_free(ready);
    if (a->dsp < 0 && audio_ready)
        start_dsp(a);
    if (a->bus && g_dbus_connection_is_closed(a->bus)) {
        g_clear_object(&a->bus);
        owner(a, "");
    }
    if (!a->bus && !a->bus_connecting) {
        a->bus_connecting = true;
        g_bus_get(G_BUS_TYPE_SYSTEM, NULL, connected, a);
    }
    /* Fresh file metadata for existing consumers, without querying MM or AT.
     * Hardware is only touched on a real input transition or startup retry. */
    if ((!a->hardware_ready || a->config_dirty) && !a->working)
        update(a);
    ring(a);
    publish(a);
    return G_SOURCE_CONTINUE;
}
static gboolean stop_service(gpointer data) {
    App *a = data;
    a->closing = true;
    if (a->retry) {
        g_source_remove(a->retry);
        a->retry = 0;
    }
    kill_alert(&a->sound);
    kill_alert(&a->vibration);
    bt_release(a);
    g_main_loop_quit(a->loop);
    return G_SOURCE_REMOVE;
}
int main(void) {
    umask(077);
    App a = {0};
    a.dsp = a.dsp_write = a.notify = a.uevent = -1;
    a.band = 0;
    a.volume = 15;
    a.ring_sound = a.ring_vibrate = a.ring_screen = true;
    a.ring_tone = g_strdup("call");
    a.config_dirty = true;
    char *directory = native_path(RUN);
    g_mkdir_with_parents(directory, 0700);
    g_free(directory);
    char *lockpath = native_path(RUN "owner.lock");
    int lock = open(lockpath, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    g_free(lockpath);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0) {
        g_printerr("Native voice observer already running or lock unavailable\n");
        return 1;
    }
    GError *error = NULL;
    a.loop = g_main_loop_new(NULL, FALSE);
    a.objects = json_object_new_object();
    a.call_reads = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    a.calls = json_object_new_array();
    Json *defaults = json_tokener_parse(default_settings);
    if (!write_json(RUN "settings.json", defaults)) {
        json_object_put(defaults);
        return 1;
    }
    json_object_put(defaults);
    a.http = screen_http_new(8089, &error);
    if (!a.http) {
        g_printerr("Screen event server: %s\n", error->message);
        g_error_free(error);
        return 1;
    }
    a.notify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (a.notify < 0)
        return 1;
    watches(&a);
    a.notify_source = g_unix_fd_add(a.notify, G_IO_IN, changed, &a);
    a.uevent =
        socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
    struct sockaddr_nl address = {.nl_family = AF_NETLINK, .nl_groups = 1};
    if (a.uevent < 0 || bind(a.uevent, (void *)&address, sizeof(address)) < 0) {
        g_printerr("Power event subscription failed\n");
        return 1;
    }
    a.power = power_snapshot();
    a.power_source = g_unix_fd_add(a.uevent, G_IO_IN, power_event, &a);
    a.bus_connecting = true;
    g_bus_get(G_BUS_TYPE_SYSTEM, NULL, connected, &a);
    a.heartbeat = g_timeout_add_seconds(1, heartbeat, &a);
    g_unix_signal_add(SIGTERM, stop_service, &a);
    g_unix_signal_add(SIGINT, stop_service, &a);
    publish(&a);
    g_message("Native voice observer ready; no call-control commands");
    g_main_loop_run(a.loop);
    /* Complete owned asynchronous work before releasing mixer state or memory. */
    while (a.working || a.bt_working || g_hash_table_size(a.call_reads))
        g_main_context_iteration(NULL, TRUE);
    if (a.dsp_started) {
        pthread_cancel(a.dsp_thread);
        pthread_join(a.dsp_thread, NULL);
        a.dsp_started = false;
    }
    if (a.dsp >= 0)
        close(a.dsp);
    if (a.dsp_write >= 0)
        close(a.dsp_write);
    if (a.hardware) {
        audio_free(a.hardware);
        a.hardware = NULL;
        media(false);
    }
    if (a.status) {
        jb(a.status, "available", false);
        jb(a.status, "active", false);
        jd(a.status, "updated", realtime());
        write_json(RUN "status.json", a.status);
    }
    screen_http_free(a.http);
    if (a.bus)
        g_object_unref(a.bus);
    g_hash_table_unref(a.call_reads);
    close(lock);
    return 0;
}
