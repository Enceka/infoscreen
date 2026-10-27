#define _GNU_SOURCE
#include "native.h"
#include <gio/gunixsocketaddress.h>
#include <math.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopped;
static void stop(int sig) {
    (void)sig;
    stopped = 1;
}
static GQuark domain(void) {
    return g_quark_from_static_string("e5-sim-status");
}
static char *query(int card, const char *command, GError **error) {
    GSocketClient *client = g_socket_client_new();
    g_socket_client_set_timeout(client, 6);
    char *suffix = g_strdup_printf("/run/t551m-at%d/cmd.sock", 6 + card),
         *path = native_path(suffix);
    g_free(suffix);
    GSocketAddress *address = g_unix_socket_address_new(path);
    g_free(path);
    GSocketConnection *connection =
        g_socket_client_connect(client, G_SOCKET_CONNECTABLE(address), NULL, error);
    g_object_unref(address);
    g_object_unref(client);
    if (!connection)
        return NULL;
    Json *request = json_object_new_object();
    char *cmd = g_strdup_printf("AT+SPACTCARD=%d;%s", card, command);
    js(request, "command", cmd);
    ji(request, "timeout", 2);
    jd(request, "deadline", monotime() + 5);
    g_free(cmd);
    char *wire = g_strconcat(encode(request), "\n", NULL);
    json_object_put(request);
    bool ok = g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(connection)),
                                        wire, strlen(wire), NULL, NULL, error);
    g_free(wire);
    GString *reply = g_string_new(NULL);
    char buf[4096];
    gssize n = 0;
    while (ok && !strchr(reply->str, '\n')) {
        n = g_input_stream_read(g_io_stream_get_input_stream(G_IO_STREAM(connection)), buf,
                                sizeof(buf), NULL, error);
        if (n <= 0) {
            ok = false;
            break;
        }
        if (reply->len + (size_t)n > 131072) {
            g_set_error_literal(error, domain(), 1, "AT response too large");
            ok = false;
            break;
        }
        g_string_append_len(reply, buf, n);
    }
    g_object_unref(connection);
    char *out = NULL;
    Json *parsed = ok ? json_tokener_parse(reply->str) : NULL;
    if (parsed && jbool(parsed, "ok", false))
        out = g_strdup(jstr(parsed, "response", ""));
    else if (!*error)
        g_set_error_literal(error, domain(), 1, "AT owner unavailable");
    if (parsed)
        json_object_put(parsed);
    g_string_free(reply, TRUE);
    return out;
}
static GPtrArray *field(const char *reply, const char *prefix) {
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    char **lines = g_strsplit(reply ? reply : "", "\n", -1);
    for (unsigned i = 0; lines[i]; i++) {
        if (!g_str_has_prefix(lines[i], prefix) || lines[i][strlen(prefix)] != ':')
            continue;
        const char *p = lines[i] + strlen(prefix) + 1;
        do {
            while (*p == ' ' || *p == '\t')
                p++;
            GString *value = g_string_new(NULL);
            bool quoted = *p == '"';
            if (quoted)
                p++;
            while (*p && *p != '\r') {
                if (quoted && *p == '"') {
                    if (p[1] == '"') {
                        g_string_append_c(value, '"');
                        p += 2;
                        continue;
                    }
                    p++;
                    break;
                }
                if (!quoted && *p == ',')
                    break;
                g_string_append_c(value, *p++);
            }
            g_ptr_array_add(out, g_string_free(value, FALSE));
            while (*p && *p != ',' && *p != '\r')
                p++;
            if (*p != ',')
                break;
            p++;
        } while (true);
        break;
    }
    g_strfreev(lines);
    return out;
}
static const char *at(GPtrArray *a, unsigned i) {
    return i < a->len ? g_ptr_array_index(a, i) : "";
}
static bool integer(const char *s, int *value) {
    if (!s || !*s)
        return false;
    char *end;
    long n = strtol(s, &end, 10);
    while (g_ascii_isspace(*end))
        end++;
    if (*end || n < INT_MIN || n > INT_MAX)
        return false;
    *value = (int)n;
    return true;
}
static int registration(const char *s, const char *prefix) {
    GPtrArray *a = field(s, prefix);
    int value = -1;
    if (a->len >= 2 && !integer(at(a, 1), &value))
        value = -1;
    g_ptr_array_unref(a);
    return value;
}
static char *operator_name(const char *s) {
    GPtrArray *cops = field(s, "+COPS"), *charset = field(s, "+CSCS");
    char *name = NULL;
    if (cops->len >= 3 && *at(cops, 2)) {
        name = g_strdup(at(cops, 2));
        if ((!strcmp(at(cops, 1), "0") || !strcmp(at(cops, 1), "1")) &&
            !g_ascii_strcasecmp(at(charset, 0), "UCS2") && strlen(name) % 4 == 0) {
            gsize size = strlen(name) / 2;
            guint8 *bytes = g_malloc(size);
            bool valid = true;
            for (gsize i = 0; i < size; i++) {
                int a = g_ascii_xdigit_value(name[i * 2]),
                    b = g_ascii_xdigit_value(name[i * 2 + 1]);
                if (a < 0 || b < 0) {
                    valid = false;
                    break;
                }
                bytes[i] = (guint8)((a << 4) | b);
            }
            gsize decoded_size = 0;
            char *decoded =
                valid ? g_convert((char *)bytes, size, "UTF-8", "UTF-16BE", NULL, &decoded_size, NULL)
                      : NULL;
            g_free(bytes);
            if (decoded) {
                /* An embedded U+0000 must not turn the remaining name into
                 * an unchecked/truncated C string. Preserve the raw reply. */
                if (memchr(decoded, 0, decoded_size)) valid = false;
                for (char *p = decoded; *p; p = g_utf8_next_char(p)) {
                    gunichar c = g_utf8_get_char(p);
                    if (c < 32 || (c >= 127 && c <= 159)) {
                        valid = false;
                        break;
                    }
                }
                if (valid) {
                    g_free(name);
                    name = decoded;
                } else
                    g_free(decoded);
            }
        }
    }
    g_ptr_array_unref(cops);
    g_ptr_array_unref(charset);
    return name;
}
static char *replace(const char *s, const char *from, const char *to) {
    char **parts = g_strsplit(s, from, -1);
    char *out = g_strjoinv(to, parts);
    g_strfreev(parts);
    return out;
}
static bool lte_sinr(const char *reply, double *sinr) {
    char **lines = g_strsplit(reply ? reply : "", "\n", -1);
    bool ok = false;
    for (unsigned i = 0; lines[i]; i++) {
        char *a = replace(g_strstrip(lines[i]), " ", ""), *b = replace(a, ",-", ",+");
        g_free(a);
        a = replace(b, "--", "-+");
        g_free(b);
        if (a[0] == '-') {
            b = g_strconcat("+", a + 1, NULL);
            g_free(a);
            a = b;
        }
        char **parts = g_strsplit(a, "-", -1);
        unsigned count = g_strv_length(parts);
        int v0, v1, v;
        if (count >= 7 && integer(parts[0], &v0) && integer(parts[1], &v1) && v0 >= 0 && v1 >= 0) {
            char *comma = strchr(parts[2], ',');
            if (comma)
                *comma = 0;
            if (integer(parts[2], &v) && v >= 0) {
                if (parts[2][0] == '+')
                    v = -v;
                if (v >= -2000 && v <= 4000) {
                    *sinr = nearbyint(v / 10.0) / 10.0;
                    ok = true;
                }
            }
        }
        g_strfreev(parts);
        g_free(a);
        if (ok)
            break;
    }
    g_strfreev(lines);
    return ok;
}
typedef char *(*Reader)(int, const char *, GError **);
static Json *collect(int card, Reader read) {
    Json *row = json_object_new_object();
    ji(row, "slot", card);
    jd(row, "updated", monotime());
    js(row, "registration", "unknown");
    js(row, "operator", NULL);
    js(row, "tech", NULL);
    js(row, "radio_label", NULL);
    js(row, "error", NULL);
    json_object_object_add(row, "present", NULL);
    json_object_object_add(row, "signal", json_object_new_object());
    GError *error = NULL;
    char *reply = read(card, "+CPIN?", &error);
    GPtrArray *a = field(reply, "+CPIN"), *cme = field(reply, "+CME ERROR");
    if (!strcmp(at(cme, 0), "10") || !strcmp(at(a, 0), "NOT INSERTED") ||
        !strcmp(at(a, 0), "SIM NOT INSERTED")) {
        jb(row, "present", false);
        js(row, "registration", "absent");
        goto done;
    }
    if (error || !a->len) {
        if (!error)
            g_set_error_literal(&error, domain(), 1, "SIM status unavailable");
        goto done;
    }
    jb(row, "present", true);
    if (strcmp(at(a, 0), "READY")) {
        js(row, "registration", "locked");
        js(row, "pin", at(a, 0));
        goto done;
    }
    g_free(reply);
    reply = read(card, "+CFUN?", &error);
    g_ptr_array_unref(a);
    a = field(reply, "+CFUN");
    if (error)
        goto done;
    if (a->len && strcmp(at(a, 0), "1")) {
        js(row, "registration", "disabled");
        goto done;
    }
    g_free(reply);
    reply = read(card, "+CSCS?;+COPS?", &error);
    if (error)
        goto done;
    char *name = operator_name(reply);
    js(row, "operator", name);
    g_free(name);
    g_ptr_array_unref(a);
    a = field(reply, "+COPS");
    int act = -1;
    if (a->len >= 4)
        integer(at(a, 3), &act);
    if (act >= 0)
        ji(row, "act", act);
    else
        json_object_object_add(row, "act", NULL);
    g_free(reply);
    reply = read(card, "+CEREG?", &error);
    if (error)
        goto done;
    int reg = registration(reply, "+CEREG");
    g_free(reply);
    reply = read(card, "+C5GREG?", &error);
    if (error)
        goto done;
    int reg5 = registration(reply, "+C5GREG");
    if (reg >= 0)
        ji(row, "reg", reg);
    else
        json_object_object_add(row, "reg", NULL);
    if (reg5 >= 0)
        ji(row, "reg5g", reg5);
    else
        json_object_object_add(row, "reg5g", NULL);
    g_free(reply);
    reply = read(card, "+CESQ", &error);
    if (error)
        goto done;
    g_ptr_array_unref(a);
    a = field(reply, "+CESQ");
    int v[9];
    for (unsigned i = 0; i < 9; i++) {
        v[i] = 255;
        if (i < a->len && !integer(at(a, i), &v[i])) {
            g_set_error_literal(&error, domain(), 1, "Invalid CESQ response");
            goto done;
        }
    }
    bool lte = v[5] >= 0 && v[5] <= 97, nr = v[7] >= 0 && v[7] <= 126;
    const char *tech = NULL, *label = NULL;
    if (nr) {
        tech = "5gnr";
        label = lte ? "5G NSA" : "5G SA";
    } else if (act == 13) {
        tech = "5gnr";
        label = "5G NSA";
    } else if (act == 11 || act == 18 || act == 19) {
        tech = "5gnr";
        label = "5G";
    } else if (act == 7 || act == 10) {
        tech = "lte";
        label = "4G";
    } else if (act >= 2 && act <= 6) {
        tech = "umts";
        label = "3G";
    } else if (act == 0 || act == 1) {
        tech = "gsm";
        label = "2G";
    }
    js(row, "tech", tech);
    js(row, "radio_label", label);
    Json *signal = json_object_new_object();
    js(signal, "rsrp", NULL);
    js(signal, "rsrq", NULL);
    if (tech && !strcmp(tech, "5gnr")) {
        js(signal, "snr", NULL);
        if (nr)
            jd(signal, "rsrp", v[7] - 157);
        if (v[6] >= 0 && v[6] <= 127)
            jd(signal, "rsrq", v[6] * .5 - 43);
        if (v[8] >= 0 && v[8] <= 127)
            jd(signal, "snr", v[8] * .5 - 23);
    } else {
        if (lte)
            jd(signal, "rsrp", v[5] - 141);
        if (v[4] >= 0 && v[4] <= 34)
            jd(signal, "rsrq", v[4] * .5 - 20);
        if (tech && !strcmp(tech, "lte") && lte) {
            js(signal, "snr", NULL);
            g_free(reply);
            reply = read(card, "+SPENGMD=0,0,6", &error);
            double snr;
            if (!error && lte_sinr(reply, &snr))
                jd(signal, "snr", snr);
            g_clear_error(&error); /* optional PHY data never erases registration */
        }
    }
    json_object_object_add(row, "signal", signal);
    int state = (reg5 == 1 || reg5 == 5) ? reg5 : reg;
    const char *states[] = {"idle", "home", "searching", "denied", "unknown", "roaming"};
    js(row, "registration",
       state >= 0 && state <= 5 ? states[state]
       : state == 8             ? "emergency"
                                : "unknown");
    if (state < 0 && *jstr(row, "operator", "") && jget(signal, "rsrp"))
        js(row, "registration", "camped");
done:
    if (error) {
        js(row, "error", error->message);
        js(row, "registration", "unknown");
        js(row, "tech", NULL);
        js(row, "radio_label", NULL);
        json_object_object_add(row, "signal", json_object_new_object());
        g_clear_error(&error);
    }
    g_free(reply);
    g_ptr_array_unref(a);
    g_ptr_array_unref(cme);
    return row;
}
#ifndef E5_SIM_TEST
int main(void) {
    umask(077);
    signal(SIGTERM, stop);
    signal(SIGINT, stop);
    while (!stopped) {
        double started = monotime();
        char *card = read_text("/sys/module/sipc_wwan/parameters/card", 16);
        int active = -1;
        if (card)
            integer(g_strstrip(card), &active);
        g_free(card);
        if (active == 0 || active == 1) {
            Json *rows = json_object_new_array();
            json_object_array_add(rows, collect(0, query));
            json_object_array_add(rows, collect(1, query));
            card = read_text("/sys/module/sipc_wwan/parameters/card", 16);
            int current = -1;
            if (card)
                integer(g_strstrip(card), &current);
            g_free(card);
            if (current == active) {
                Json *value = json_object_new_object();
                jd(value, "updated", monotime());
                ji(value, "active", active);
                json_object_object_add(value, "sims", rows);
                write_json("/run/t551m-sims.json", value);
                json_object_put(value);
            } else
                json_object_put(rows);
        }
        double wait = MAX(.1, 5 - (monotime() - started));
        struct timespec duration = {(time_t)wait, (long)((wait - (time_t)wait) * 1e9)};
        while (!stopped && nanosleep(&duration, &duration) < 0) {
        }
    }
    return 0;
}
#endif
