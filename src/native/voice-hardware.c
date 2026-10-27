#define _GNU_SOURCE
/* Native equivalent of the stock-derived VoiceHardware. Calibration remains
 * recipient firmware data; the AP never copies audio into the hostless PCM. */
#include "native.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

typedef void Ctl;
typedef void Pcm;
typedef void Info;
typedef void Value;
typedef void Params;
typedef struct {
    void *library;
    const char *(*strerror)(int);
    int (*ctl_open)(Ctl **, const char *, int);
    int (*ctl_close)(Ctl *);
    int (*ctl_elem_info_malloc)(Info **);
    void (*ctl_elem_info_free)(Info *);
    void (*ctl_elem_info_set_interface)(Info *, int);
    void (*ctl_elem_info_set_name)(Info *, const char *);
    void (*ctl_elem_info_set_item)(Info *, unsigned);
    const char *(*ctl_elem_info_get_item_name)(Info *);
    unsigned (*ctl_elem_info_get_type)(Info *);
    unsigned (*ctl_elem_info_get_count)(Info *);
    unsigned (*ctl_elem_info_get_items)(Info *);
    unsigned (*ctl_elem_info_get_numid)(Info *);
    int (*ctl_elem_info)(Ctl *, Info *);
    int (*ctl_elem_value_malloc)(Value **);
    void (*ctl_elem_value_free)(Value *);
    void (*ctl_elem_value_set_numid)(Value *, unsigned);
    int (*ctl_elem_read)(Ctl *, Value *);
    int (*ctl_elem_write)(Ctl *, Value *);
    int (*ctl_elem_value_get_boolean)(Value *, unsigned);
    long (*ctl_elem_value_get_integer)(Value *, unsigned);
    unsigned (*ctl_elem_value_get_enumerated)(Value *, unsigned);
    void (*ctl_elem_value_set_boolean)(Value *, unsigned, long);
    void (*ctl_elem_value_set_integer)(Value *, unsigned, long);
    void (*ctl_elem_value_set_enumerated)(Value *, unsigned, unsigned);
    int (*pcm_open)(Pcm **, const char *, int, int);
    int (*pcm_close)(Pcm *);
    int (*pcm_prepare)(Pcm *);
    int (*pcm_start)(Pcm *);
    long (*pcm_forward)(Pcm *, unsigned long);
    int (*pcm_hw_params_malloc)(Params **);
    void (*pcm_hw_params_free)(Params *);
    int (*pcm_hw_params_any)(Pcm *, Params *);
    int (*pcm_hw_params_set_access)(Pcm *, Params *, int);
    int (*pcm_hw_params_set_format)(Pcm *, Params *, int);
    int (*pcm_hw_params_set_channels)(Pcm *, Params *, unsigned);
    int (*pcm_hw_params_set_rate)(Pcm *, Params *, unsigned, int);
    int (*pcm_hw_params_set_period_size_near)(Pcm *, Params *, unsigned long *, int *);
    int (*pcm_hw_params_set_buffer_size_near)(Pcm *, Params *, unsigned long *);
    int (*pcm_hw_params)(Pcm *, Params *);
} Alsa;
typedef struct {
    unsigned type, count;
    Value *value;
    char **choices;
} Element;
struct VoiceHardware {
    Alsa a;
    Ctl *ctl;
    Pcm *pcm[2];
    GHashTable *elements;
    Json *routes, *saved, *gains, *board;
    GPtrArray *controls;
    guint16 curves[256][7];
    unsigned curve_count;
    bool active, speaker, bluetooth, muted, remote, shared, aol;
    int card, device, call_card, band, rate, volume, ceiling;
    int applied_step, applied_gain, applied_mute;
};
static GQuark domain(void) {
    return g_quark_from_static_string("e5-native-audio");
}
static bool fail(GError **error, const char *what) {
    if (error && !*error)
        g_set_error_literal(error, domain(), 1, what);
    return false;
}
static bool check(VoiceHardware *h, int rc, const char *what, GError **error) {
    if (rc >= 0)
        return true;
    if (error && !*error)
        g_set_error(error, domain(), rc, "%s: %s", what, h->a.strerror(rc));
    return false;
}
static bool alsa_load(Alsa *a, GError **error) {
    a->library = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!a->library)
        return fail(error, "Required ALSA library unavailable");
#define LOAD(name)                                                                                 \
    do {                                                                                           \
        *(void **)(&a->name) = dlsym(a->library, "snd_" #name);                                    \
        if (!a->name)                                                                              \
            return fail(error, "Required ALSA symbol unavailable: " #name);                        \
    } while (0)
    LOAD(strerror);
    LOAD(ctl_open);
    LOAD(ctl_close);
    LOAD(ctl_elem_info_malloc);
    LOAD(ctl_elem_info_free);
    LOAD(ctl_elem_info_set_interface);
    LOAD(ctl_elem_info_set_name);
    LOAD(ctl_elem_info_set_item);
    LOAD(ctl_elem_info_get_item_name);
    LOAD(ctl_elem_info_get_type);
    LOAD(ctl_elem_info_get_count);
    LOAD(ctl_elem_info_get_items);
    LOAD(ctl_elem_info_get_numid);
    LOAD(ctl_elem_info);
    LOAD(ctl_elem_value_malloc);
    LOAD(ctl_elem_value_free);
    LOAD(ctl_elem_value_set_numid);
    LOAD(ctl_elem_read);
    LOAD(ctl_elem_write);
    LOAD(ctl_elem_value_get_boolean);
    LOAD(ctl_elem_value_get_integer);
    LOAD(ctl_elem_value_get_enumerated);
    LOAD(ctl_elem_value_set_boolean);
    LOAD(ctl_elem_value_set_integer);
    LOAD(ctl_elem_value_set_enumerated);
    LOAD(pcm_open);
    LOAD(pcm_close);
    LOAD(pcm_prepare);
    LOAD(pcm_start);
    LOAD(pcm_forward);
    LOAD(pcm_hw_params_malloc);
    LOAD(pcm_hw_params_free);
    LOAD(pcm_hw_params_any);
    LOAD(pcm_hw_params_set_access);
    LOAD(pcm_hw_params_set_format);
    LOAD(pcm_hw_params_set_channels);
    LOAD(pcm_hw_params_set_rate);
    LOAD(pcm_hw_params_set_period_size_near);
    LOAD(pcm_hw_params_set_buffer_size_near);
    LOAD(pcm_hw_params);
#undef LOAD
    return true;
}
static void element_free(VoiceHardware *h, Element *e) {
    if (e->value)
        h->a.ctl_elem_value_free(e->value);
    g_strfreev(e->choices);
    g_free(e);
}
static Element *element(VoiceHardware *h, const char *name, GError **error) {
    Element *e = g_hash_table_lookup(h->elements, name);
    if (e)
        return e;
    Info *info = NULL;
    e = g_new0(Element, 1);
    if (!check(h, h->a.ctl_elem_info_malloc(&info), "Allocate mixer metadata", error))
        goto failed;
    h->a.ctl_elem_info_set_interface(info, 2);
    h->a.ctl_elem_info_set_name(info, name);
    if (!check(h, h->a.ctl_elem_info(h->ctl, info), name, error))
        goto failed;
    e->type = h->a.ctl_elem_info_get_type(info);
    e->count = h->a.ctl_elem_info_get_count(info);
    if (e->type < 1 || e->type > 3 || !e->count || e->count > 128) {
        fail(error, "Unsupported mixer metadata");
        goto failed;
    }
    unsigned id = h->a.ctl_elem_info_get_numid(info);
    if (e->type == 3) {
        unsigned n = h->a.ctl_elem_info_get_items(info);
        if (n > 65536) {
            fail(error, "Invalid mixer enum count");
            goto failed;
        }
        e->choices = g_new0(char *, n + 1);
        for (unsigned i = 0; i < n; i++) {
            h->a.ctl_elem_info_set_item(info, i);
            if (!check(h, h->a.ctl_elem_info(h->ctl, info), name, error))
                goto failed;
            e->choices[i] = g_strdup(h->a.ctl_elem_info_get_item_name(info));
        }
    }
    if (!check(h, h->a.ctl_elem_value_malloc(&e->value), "Allocate mixer value", error))
        goto failed;
    h->a.ctl_elem_value_set_numid(e->value, id);
    h->a.ctl_elem_info_free(info);
    g_hash_table_insert(h->elements, g_strdup(name), e);
    return e;
failed:
    if (info)
        h->a.ctl_elem_info_free(info);
    element_free(h, e);
    return NULL;
}
static char *get(VoiceHardware *h, const char *name, GError **error) {
    Element *e = element(h, name, error);
    if (!e || !check(h, h->a.ctl_elem_read(h->ctl, e->value), name, error))
        return NULL;
    GString *s = g_string_new(NULL);
    for (unsigned i = 0; i < e->count; i++) {
        if (i)
            g_string_append_c(s, ',');
        if (e->type == 1)
            g_string_append(s, h->a.ctl_elem_value_get_boolean(e->value, i) ? "on" : "off");
        else
            g_string_append_printf(s, "%ld",
                                   e->type == 2
                                       ? h->a.ctl_elem_value_get_integer(e->value, i)
                                       : (long)h->a.ctl_elem_value_get_enumerated(e->value, i));
    }
    return g_string_free(s, FALSE);
}
static bool put(VoiceHardware *h, const char *name, const char *setting, GError **error) {
    Element *e = element(h, name, error);
    if (!e)
        return false;
    char **fields = g_strsplit(setting, ",", -1);
    unsigned count = g_strv_length(fields);
    bool ok = false;
    if (count != 1 && count != e->count) {
        fail(error, "Wrong mixer value count");
        goto out;
    }
    for (unsigned i = 0; i < e->count; i++) {
        const char *s = fields[count == 1 ? 0 : i];
        long value = 0;
        bool found = false;
        if (e->type == 3)
            for (unsigned n = 0; e->choices[n]; n++)
                if (!strcmp(s, e->choices[n])) {
                    value = n;
                    found = true;
                    break;
                }
        if (e->type == 1 && (!strcmp(s, "on") || !strcmp(s, "off"))) {
            value = !strcmp(s, "on");
            found = true;
        }
        if (!found) {
            char *end;
            errno = 0;
            value = strtol(s, &end, 0);
            if (errno || !*s || *end) {
                fail(error, "Invalid mixer value");
                goto out;
            }
        }
        if ((e->type == 1 && value != 0 && value != 1) ||
            (e->type == 3 && (value < 0 || (unsigned long)value >= g_strv_length(e->choices)))) {
            fail(error, "Mixer value out of range");
            goto out;
        }
        if (e->type == 1)
            h->a.ctl_elem_value_set_boolean(e->value, i, value);
        else if (e->type == 2)
            h->a.ctl_elem_value_set_integer(e->value, i, value);
        else
            h->a.ctl_elem_value_set_enumerated(e->value, i, (unsigned)value);
    }
    ok = check(h, h->a.ctl_elem_write(h->ctl, e->value), name, error);
out:
    g_strfreev(fields);
    return ok;
}
static bool puti(VoiceHardware *h, const char *name, unsigned value, GError **error) {
    char s[32];
    snprintf(s, sizeof(s), "%u", value);
    return put(h, name, s, error);
}
static bool group(VoiceHardware *h, Json *values, const char *skip, GError **error) {
    if (!values)
        return fail(error, "Missing native voice routing group");
    json_object_object_foreach(values, name, value) {
        if (skip && !strcmp(name, skip))
            continue;
        if (!put(h, name, json_object_get_string(value), error))
            return false;
    }
    return true;
}
static bool route_group(VoiceHardware *h, const char *key, const char *skip, GError **error) {
    return group(h, jget(h->routes, key), skip, error);
}
static bool on(const char *name, const char *value) {
    return g_str_has_prefix(name, "S_VOICE_") && (!strcmp(value, "on") || !strcmp(value, "1"));
}
static bool restore(VoiceHardware *h, Json *values, GError **error) {
    if (!values)
        return true;
    GPtrArray *names = g_ptr_array_new();
    GError *first = NULL;
    json_object_object_foreach(values, name, value) {
        const char *s = json_object_get_string(value);
        g_ptr_array_add(names, (void *)name);
        if (on(name, s)) {
            GError *e = NULL;
            put(h, name, s, &e);
            if (e) {
                if (!first)
                    first = e;
                else
                    g_error_free(e);
            }
        }
    }
    for (int i = (int)names->len - 1; i >= 0; i--) {
        const char *name = g_ptr_array_index(names, i), *value = jstr(values, name, "");
        if (on(name, value))
            continue;
        GError *e = NULL;
        put(h, name, value, &e);
        if (e) {
            if (!first)
                first = e;
            else
                g_error_free(e);
        }
    }
    g_ptr_array_unref(names);
    if (first) {
        if (error && !*error)
            *error = first;
        else
            g_error_free(first);
        return false;
    }
    return true;
}
static Json *snapshot(VoiceHardware *h, GError **error) {
    Json *out = json_object_new_object();
    for (unsigned i = 0; i < h->controls->len; i++) {
        const char *name = g_ptr_array_index(h->controls, i);
        char *value = get(h, name, error);
        if (!value) {
            json_object_put(out);
            return NULL;
        }
        js(out, name, value);
        g_free(value);
    }
    return out;
}
unsigned audio_profile_word(unsigned band, bool speaker, bool bluetooth) {
    static const unsigned modes[3][4] = {{0, 2, 4, 5}, {7, 9, 11, 12}, {28, 30, 32, 33}};
    if (band > 3)
        return 0;
    unsigned mode = modes[bluetooth ? 2 : speaker ? 1 : 0][band];
    return (mode << 24) | (mode << 16) | 5;
}
static bool speaker_path(VoiceHardware *h) {
    return !h->bluetooth && (h->speaker || h->shared);
}
static int dsp_volume(VoiceHardware *h) {
    if (h->bluetooth)
        return 7;
    if (!h->volume)
        return 1;
    double level = h->speaker ? h->volume : 1 + (h->volume - 1) * (h->ceiling - 1) / 14.0;
    if (level == 15)
        return 7;
    unsigned mode = audio_profile_word(h->band, h->speaker, false) >> 24;
    guint16 *gains = h->curves[mode];
    double low = INFINITY, high = log(gains[6]);
    for (unsigned i = 0; i < 7; i++)
        if (gains[i] && gains[i] <= gains[6])
            low = MIN(low, log(gains[i]));
    double target = low + (high - low) * (level - 1) / 14, best = INFINITY;
    int step = 1;
    for (unsigned i = 0; i < 7; i++)
        if (gains[i] && gains[i] <= gains[6]) {
            double distance = fabs(log(gains[i]) - target);
            if (distance < best) {
                best = distance;
                step = (int)i + 1;
            }
        }
    return step;
}
static bool volume(VoiceHardware *h, bool force, GError **error) {
    int step = dsp_volume(h), effective = h->bluetooth && !h->remote ? h->volume : 15,
        gain = effective ? 24 - (15 - effective) : 0, mute = !h->volume;
    if ((force || h->applied_step != step) && !puti(h, "VBC_VOLUME", step, error))
        return false;
    if (force || h->applied_gain != gain) {
        char s[32];
        snprintf(s, sizeof(s), "%d,%d", gain, gain);
        if (!put(h, "VBC DAC1 DG Set", s, error))
            return false;
    }
    if (force || h->applied_mute != mute)
        if (!put(h, "VBC DAC1 DSP MDG Set", mute ? "1,1024" : "0,1024", error) ||
            !put(h, "VBC_DL_MUTE", mute ? "enable" : "disable", error))
            return false;
    h->applied_step = step;
    h->applied_gain = gain;
    h->applied_mute = mute;
    return true;
}
static bool board_output(VoiceHardware *h, GError **error) {
    if (!h->board)
        return true;
    if (h->bluetooth)
        return put(h, "S_VOICE_P_HIFI SWITCH", "off", error);
    if (speaker_path(h))
        return group(h, jget(h->board, "receiver_off"), NULL, error) &&
               group(h, jget(h->board, "digital"), NULL, error) &&
               group(h, jget(h->board, "pa"), NULL, error) &&
               put(h, "S_VOICE_P_HIFI SWITCH", "on", error) &&
               put(h, "S_VOICE_P_CODEC SWITCH", "off", error);
    return put(h, "S_VOICE_P_CODEC SWITCH", "on", error) &&
           put(h, "S_VOICE_P_HIFI SWITCH", "off", error) &&
           group(h, jget(h->board, "pa_off"), NULL, error) &&
           group(h, jget(h->board, "codec"), NULL, error) &&
           group(h, jget(h->board, "receiver"), NULL, error);
}
static bool profiles(VoiceHardware *h, GError **error) {
    Json *names = jget(h->routes, "PROFILES");
    unsigned word = audio_profile_word(h->band, h->speaker, h->bluetooth);
    for (size_t i = 0; i < json_object_array_length(names); i++)
        if (!puti(h, json_object_get_string(json_object_array_get_idx(names, i)), word, error))
            return false;
    if (!puti(h, "VBC_VOLUME", dsp_volume(h), error))
        return false;
    if (h->bluetooth) {
        if (!puti(h, "VBC_SRC_BT_DAC", h->rate, error) ||
            !puti(h, "VBC_SRC_BT_ADC", h->rate, error))
            return false;
    } else {
        if (!route_group(h, "CODEC_IIS", NULL, error))
            return false;
        unsigned mode = audio_profile_word(h->band, speaker_path(h), false) >> 24;
        char key[16];
        snprintf(key, sizeof(key), "%u", mode);
        Json *g = jget(h->gains, key);
        bool ao = speaker_path(h) || h->aol;
        int dacs = jint(g, ao ? "dacs_playback_volume" : "dacl_playback_volume", ao ? 0 : 3);
        static const int ao_speaker[] = {3, 3, 3, 4}, ear[] = {2, 2, 2, 4};
        if (!puti(h, "DAC Gain DAC Playback Volume", dacs, error) ||
            !puti(h, "AO Gain AO Playback Volume",
                  jint(g, "ao_playback_volume", speaker_path(h) ? ao_speaker[h->band] : 6), error))
            return false;
        if (!ao && !puti(h, "EAR Gain EAR Playback Volume",
                         jint(g, "ear_playback_volume", ear[h->band]), error))
            return false;
    }
    return board_output(h, error);
}
static bool output(VoiceHardware *h, GError **error) {
    if (!put(h, "Speaker Function", "off", error) || !put(h, "Earpiece Function", "off", error))
        return false;
    if (h->bluetooth) {
        if (!route_group(h, "BLUETOOTH", NULL, error))
            return false;
    } else if (!h->board && !route_group(h,
                                         speaker_path(h) ? "SPEAKER"
                                         : h->aol        ? "RECEIVER_AOL"
                                                         : "RECEIVER",
                                         NULL, error))
        return false;
    return board_output(h, error);
}
static bool pcm_open(VoiceHardware *h, int direction, GError **error) {
    Params *p = NULL;
    char device[40];
    snprintf(device, sizeof(device), "hw:%d,%d", h->card, h->device);
    bool ok = false;
    if (!check(h, h->a.pcm_open(&h->pcm[direction], device, direction, 1), "Open hostless PCM",
               error))
        goto out;
    if (!check(h, h->a.pcm_hw_params_malloc(&p), "PCM parameter allocation", error))
        goto out;
    if (!check(h, h->a.pcm_hw_params_any(h->pcm[direction], p), "PCM defaults", error) ||
        !check(h, h->a.pcm_hw_params_set_access(h->pcm[direction], p, 3), "PCM access", error) ||
        !check(h, h->a.pcm_hw_params_set_format(h->pcm[direction], p, 2), "PCM format", error) ||
        !check(h, h->a.pcm_hw_params_set_channels(h->pcm[direction], p, 1), "PCM channels",
               error) ||
        !check(h, h->a.pcm_hw_params_set_rate(h->pcm[direction], p, 8000, 0), "PCM rate", error))
        goto out;
    unsigned long period = 640, buffer = 1280;
    int rounding = 0;
    if (!check(h, h->a.pcm_hw_params_set_period_size_near(h->pcm[direction], p, &period, &rounding),
               "PCM period", error) ||
        !check(h, h->a.pcm_hw_params_set_buffer_size_near(h->pcm[direction], p, &buffer),
               "PCM buffer", error) ||
        !check(h, h->a.pcm_hw_params(h->pcm[direction], p), "PCM parameters", error) ||
        !check(h, h->a.pcm_prepare(h->pcm[direction]), "Prepare PCM", error))
        goto out;
    if (!direction) {
        long moved = h->a.pcm_forward(h->pcm[direction], buffer);
        if (moved <= 0) {
            fail(error, "Hostless playback pointer did not advance");
            goto out;
        }
    }
    ok = true;
out:
    if (p)
        h->a.pcm_hw_params_free(p);
    return ok;
}
static bool stop_audio(VoiceHardware *h, GError **error) {
    for (int i = 1; i >= 0; i--)
        if (h->pcm[i]) {
            h->a.pcm_close(h->pcm[i]);
            h->pcm[i] = NULL;
        }
    Json *saved = h->saved;
    h->saved = NULL;
    h->active = h->speaker = h->bluetooth = h->muted = false;
    h->rate = 8000;
    h->call_card = -1;
    h->applied_step = h->applied_gain = h->applied_mute = -1;
    bool ok = restore(h, saved, error);
    if (saved)
        json_object_put(saved);
    return ok;
}
static bool start_audio(VoiceHardware *h, GError **error) {
    h->saved = snapshot(h, error);
    if (!h->saved)
        return false;
    bool ok = profiles(h, error) && route_group(h, "ROUTES", NULL, error) && output(h, error) &&
              pcm_open(h, 0, error) && pcm_open(h, 1, error);
    for (int i = 0; ok && i < 2; i++)
        ok = check(h, h->a.pcm_start(h->pcm[i]), "Start PCM", error);
    ok = ok && profiles(h, error) && volume(h, true, error);
    if (!ok) {
        GError *restore_error = NULL;
        stop_audio(h, &restore_error);
        if (restore_error) {
            g_warning("Audio rollback: %s", restore_error->message);
            g_error_free(restore_error);
        }
        return false;
    }
    h->active = true;
    return true;
}
static bool set_route(VoiceHardware *h, const char *route, int rate, GError **error) {
    bool speaker = !strcmp(route, "speaker"), bt = !strcmp(route, "bluetooth");
    if ((!speaker && !bt && strcmp(route, "receiver")) || (rate != 8000 && rate != 16000))
        return fail(error, "Unsupported voice route");
    if (speaker == h->speaker && bt == h->bluetooth && (!bt || rate == h->rate))
        return true;
    if (!h->active) {
        h->speaker = speaker;
        h->bluetooth = bt;
        h->rate = rate;
        return true;
    }
    Json *before = snapshot(h, error);
    if (!before)
        return false;
    bool old_speaker = h->speaker, old_bt = h->bluetooth;
    int old_rate = h->rate;
    bool ok =
        put(h, "VBC_DL_MUTE", "enable", error) && put(h, "VBC DAC1 DSP MDG Set", "1,1024", error);
    h->speaker = speaker;
    h->bluetooth = bt;
    h->rate = rate;
    if (ok && old_bt && !bt) {
        const char *groups[] = {"BLUETOOTH", "BT_SRC"};
        for (unsigned g = 0; ok && g < 2; g++) {
            Json *names = jget(h->routes, groups[g]);
            if (g == 0) {
                json_object_object_foreach(names, name, value) {
                    (void)value;
                    if (!g_str_has_prefix(name, "S_VOICE_") && jget(h->saved, name) &&
                        !put(h, name, jstr(h->saved, name, ""), error)) {
                        ok = false;
                        break;
                    }
                }
            } else
                for (size_t i = 0; ok && i < json_object_array_length(names); i++) {
                    const char *name = json_object_get_string(json_object_array_get_idx(names, i));
                    if (jget(h->saved, name))
                        ok = put(h, name, jstr(h->saved, name, ""), error);
                }
        }
        ok = ok && route_group(h, "ROUTES", "VBC ADC2 DG Set", error) &&
             put(h, "S_VOICE_P_BT SWITCH", "off", error) &&
             put(h, "S_VOICE_C_BT SWITCH", "off", error);
    }
    ok = ok && output(h, error) && profiles(h, error) &&
         put(h, "VBC ADC2 DG Set", jstr(before, "VBC ADC2 DG Set", "24,24"), error) &&
         volume(h, true, error);
    if (!ok) {
        h->speaker = old_speaker;
        h->bluetooth = old_bt;
        h->rate = old_rate;
        h->applied_step = h->applied_gain = h->applied_mute = -1;
        GError *e = NULL;
        restore(h, before, &e);
        if (e) {
            g_warning("Route rollback: %s", e->message);
            g_error_free(e);
        }
    }
    json_object_put(before);
    return ok;
}
bool audio_profile_adjust(unsigned char *data, size_t size, int attenuation, GError **error) {
    if (size < 24 || memcmp(data, "audio_profile\0\0\0", 16) || attenuation < 0 || attenuation > 36)
        return fail(error, "Invalid audio profile/attenuation");
    guint32 count, record;
    memcpy(&count, data + 16, 4);
    memcpy(&record, data + 20, 4);
    count = GUINT32_FROM_LE(count);
    record = GUINT32_FROM_LE(record);
    if (count < 13 || count > 256 || record != 0x2da || size != 24 + (size_t)count * record)
        return fail(error, "Unsupported audio structure layout");
    static const unsigned modes[] = {0, 2, 4, 5, 7, 9, 11, 12};
    for (unsigned m = 0; m < G_N_ELEMENTS(modes); m++) {
        size_t off = 24 + modes[m] * record + 0x244;
        guint16 gains[7], adjusted[7];
        for (unsigned i = 0; i < 7; i++) {
            memcpy(&gains[i], data + off + 2 * i, 2);
            gains[i] = GUINT16_FROM_LE(gains[i]);
            if (!gains[i])
                return fail(error, "Invalid local-call gain table");
        }
        for (unsigned i = 0; i < 7; i++)
            adjusted[i] = (guint16)MIN(
                MIN(gains[i], gains[6]),
                MAX(1, nearbyint(gains[i] * pow(10, -attenuation * (6 - (int)i) / 120.0))));
        for (int i = 5; i >= 0; i--)
            adjusted[i] = MIN(adjusted[i], adjusted[i + 1]);
        for (unsigned i = 0; i < 7; i++) {
            guint16 le = GUINT16_TO_LE(adjusted[i]);
            memcpy(data + off + 2 * i, &le, 2);
        }
    }
    return true;
}
typedef struct {
    Json *gains, *board;
    GPtrArray *stack;
    int mode;
    unsigned mode_depth;
} Xml;
static const char *attr(const char **names, const char **values, const char *wanted) {
    for (unsigned i = 0; names && names[i]; i++)
        if (!strcmp(names[i], wanted))
            return values[i];
    return NULL;
}
static bool number(const char *s, long *v) {
    if (!s || !*s)
        return false;
    char *end;
    errno = 0;
    *v = strtol(s, &end, 0);
    return !errno && !*end;
}
static void xml_start(GMarkupParseContext *ctx, const gchar *tag, const gchar **names,
                      const gchar **values, gpointer data, GError **error) {
    (void)ctx;
    Xml *x = data;
    g_ptr_array_add(x->stack, g_strdup(tag));
    const char *mode = attr(names, values, "mode");
    long m;
    if (x->gains && mode) {
        if (!number(mode, &m) || m < 0 || m > 255) {
            fail(error, "Invalid codec mode");
            return;
        }
        x->mode = (int)m;
        x->mode_depth = x->stack->len;
        char key[16];
        snprintf(key, sizeof(key), "%d", x->mode);
        if (!jget(x->gains, key))
            json_object_object_add(x->gains, key, json_object_new_object());
    }
    if (x->gains && x->mode >= 0 && !strcmp(tag, "volume6") && x->stack->len > 1) {
        const char *parent = g_ptr_array_index(x->stack, x->stack->len - 2);
        static const char *fields[] = {"dacl_playback_volume", "dacs_playback_volume",
                                       "ao_playback_volume", "ear_playback_volume"};
        static const int maximum[] = {3, 3, 7, 15};
        for (unsigned i = 0; i < 4; i++)
            if (!strcmp(parent, fields[i])) {
                long value;
                if (!number(attr(names, values, "val"), &value)) {
                    fail(error, "Invalid codec gain");
                    return;
                }
                if (value >= 0 && value <= maximum[i]) {
                    char key[16];
                    snprintf(key, sizeof(key), "%d", x->mode);
                    ji(jget(x->gains, key), parent, (int)value);
                }
            }
    }
    if (x->board && !strcmp(tag, "ctl")) {
        GString *path = g_string_new(NULL);
        for (unsigned i = 0; i < x->stack->len; i++) {
            g_string_append_c(path, '/');
            g_string_append(path, g_ptr_array_index(x->stack, i));
        }
        const char *paths[] = {"/vbc_iis_mux/only_smart_spk_p/on/ctl",
                               "/devices/handset/on/ctl",
                               "/devices/handset/off/ctl",
                               "/vbc_iis_mux/only_smart_spk_p/off/ctl",
                               "/vbc_iis_mux/only_codec_p/on/ctl",
                               "/usecase_device/default/on/ctl"};
        const char *keys[] = {"digital", "receiver", "receiver_off", "pa_off", "codec", "pa"};
        for (unsigned i = 0; i < G_N_ELEMENTS(paths); i++)
            if (g_str_has_suffix(path->str, paths[i])) {
                const char *name = attr(names, values, "name"), *value = attr(names, values, "val"),
                           *n = attr(names, values, "count");
                long count = 1;
                if (!name || !value || strlen(name) > 256 || strlen(value) > 1024 ||
                    (n && !number(n, &count)) || count < 1 || count > 128) {
                    fail(error, "Invalid board route control");
                    break;
                }
                char *setting = !strcmp(name, "VBC_IIS_MST_WIDTH_SET")
                                    ? g_strconcat("MST_", value, NULL)
                                    : g_strdup(value);
                GString *repeat = g_string_new(NULL);
                for (long k = 0; k < count; k++) {
                    if (k)
                        g_string_append_c(repeat, ',');
                    g_string_append(repeat, setting);
                }
                js(jget(x->board, keys[i]), name, repeat->str);
                g_string_free(repeat, TRUE);
                g_free(setting);
            }
        g_string_free(path, TRUE);
    }
}
static void xml_end(GMarkupParseContext *ctx, const gchar *tag, gpointer data, GError **error) {
    (void)ctx;
    (void)tag;
    (void)error;
    Xml *x = data;
    if (x->stack->len == x->mode_depth)
        x->mode = -1;
    if (x->stack->len)
        g_ptr_array_remove_index(x->stack, x->stack->len - 1);
}
static bool parse_xml(const char *path, Xml *x, GError **error) {
    gsize size = 0;
    char *data = read_bytes(path, 4 * 1024 * 1024, &size);
    if (!data)
        return fail(error, "Recipient audio XML missing or too large");
    if (strstr(data, "<!DOCTYPE") || strstr(data, "<!ENTITY")) {
        g_free(data);
        return fail(error, "Unsafe recipient audio XML");
    }
    GMarkupParser parser = {.start_element = xml_start, .end_element = xml_end};
    x->stack = g_ptr_array_new_with_free_func(g_free);
    x->mode = -1;
    GMarkupParseContext *ctx = g_markup_parse_context_new(&parser, 0, x, NULL);
    bool ok = g_markup_parse_context_parse(ctx, data, size, error) &&
              g_markup_parse_context_end_parse(ctx, error);
    g_markup_parse_context_free(ctx);
    g_ptr_array_unref(x->stack);
    g_free(data);
    return ok;
}
static bool curves(VoiceHardware *h, GError **error) {
    gsize size = 0;
    char *data = read_bytes("/lib/firmware/audio_structure", 4 * 1024 * 1024, &size);
    if (!data)
        return fail(error, "Recipient audio structure missing");
    bool ok = false;
    guint32 count = 0, record = 0;
    if (size < 24 || memcmp(data, "audio_profile\0\0\0", 16)) {
        fail(error, "Invalid audio structure header");
        goto out;
    }
    memcpy(&count, data + 16, 4);
    memcpy(&record, data + 20, 4);
    count = GUINT32_FROM_LE(count);
    record = GUINT32_FROM_LE(record);
    if (!count || count > 256 || record != 0x2da || size != 24 + (size_t)count * record) {
        fail(error, "Unsupported audio structure layout");
        goto out;
    }
    char *setting = read_text("/etc/e5/call-low-volume-attenuation-db", 32);
    if (setting) {
        long attenuation;
        if (!number(g_strstrip(setting), &attenuation) || attenuation < 0 || attenuation > 36) {
            g_free(setting);
            fail(error, "Invalid low-volume attenuation");
            goto out;
        }
        g_free(setting);
        char *desired = g_memdup2(data, size);
        if (!audio_profile_adjust((unsigned char *)desired, size, (int)attenuation, error)) {
            g_free(desired);
            goto out;
        }
        char *path = native_path("/dev/vbc_turning0");
        struct stat st;
        int fd = -1;
        if (stat(path, &st) == 0 && S_ISCHR(st.st_mode))
            fd = open(path, O_RDWR | O_CLOEXEC);
        g_free(path);
        if (fd < 0) {
            g_free(desired);
            fail(error, "Audio tuning interface unavailable");
            goto out;
        }
        char *current = g_malloc(size);
        ssize_t n = read(fd, current, size);
        if (n != (ssize_t)size || (memcmp(current, data, size) && memcmp(current, desired, size))) {
            close(fd);
            g_free(current);
            g_free(desired);
            fail(error, "Live audio calibration differs from recipient firmware");
            goto out;
        }
        if (memcmp(current, desired, size)) {
            if (write(fd, desired, size) != (ssize_t)size ||
                read(fd, current, size) != (ssize_t)size || memcmp(current, desired, size)) {
                close(fd);
                g_free(current);
                g_free(desired);
                fail(error, "Audio tuning write/readback failed");
                goto out;
            }
        }
        close(fd);
        g_free(current);
        g_free(data);
        data = desired;
    }
    h->curve_count = count;
    for (unsigned mode = 0; mode < count; mode++)
        for (unsigned i = 0; i < 7; i++) {
            guint16 value;
            memcpy(&value, data + 24 + mode * record + 0x244 + 2 * i, 2);
            h->curves[mode][i] = GUINT16_FROM_LE(value);
        }
    static const unsigned modes[] = {0, 2, 4, 5, 7, 9, 11, 12};
    for (unsigned i = 0; i < G_N_ELEMENTS(modes); i++)
        if (modes[i] >= count || !h->curves[modes[i]][6]) {
            fail(error, "Required recipient voice gain table unavailable");
            goto out;
        }
    ok = true;
out:
    g_free(data);
    return ok;
}
static void add_control(VoiceHardware *h, const char *name) {
    for (unsigned i = 0; i < h->controls->len; i++)
        if (!strcmp(g_ptr_array_index(h->controls, i), name))
            return;
    g_ptr_array_add(h->controls, g_strdup(name));
}
VoiceHardware *audio_new(GError **error) {
    VoiceHardware *h = g_new0(VoiceHardware, 1);
    h->call_card = -1;
    h->volume = h->ceiling = 15;
    h->rate = 8000;
    h->applied_step = h->applied_gain = h->applied_mute = -1;
    h->controls = g_ptr_array_new_with_free_func(g_free);
    h->elements = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    h->routes = read_json("/usr/share/e5/voice-routes.json");
    if (!h->routes) {
        fail(error, "Native voice route manifest missing");
        goto failed;
    }
    char *cards = read_text("/proc/asound/cards", 65536),
         *pcms = read_text("/proc/asound/pcm", 65536);
    if (!cards || !strstr(cards, "sprdphone-sc2730") || !pcms) {
        g_free(cards);
        g_free(pcms);
        fail(error, "Unsupported recipient sound card");
        goto failed;
    }
    g_free(cards);
    GRegex *regex = g_regex_new("^(\\d+)-(\\d+): FE_ST_VOICE ", G_REGEX_MULTILINE, 0, NULL);
    GMatchInfo *match = NULL;
    bool found = g_regex_match(regex, pcms, 0, &match);
    if (found) {
        char *a = g_match_info_fetch(match, 1), *b = g_match_info_fetch(match, 2);
        h->card = atoi(a);
        h->device = atoi(b);
        g_free(a);
        g_free(b);
    }
    g_match_info_free(match);
    g_regex_unref(regex);
    g_free(pcms);
    if (!found) {
        fail(error, "FE_ST_VOICE PCM unavailable");
        goto failed;
    }
    const char *groups[] = {"ROUTES",    "RECEIVER", "SPEAKER",   "GAINS",       "PROFILES",
                            "BLUETOOTH", "BT_SRC",   "CODEC_IIS", "RECEIVER_AOL"};
    for (unsigned i = 0; i < G_N_ELEMENTS(groups); i++) {
        Json *group = jget(h->routes, groups[i]);
        if (!group) {
            fail(error, "Incomplete native voice route manifest");
            goto failed;
        }
        if (json_object_is_type(group, json_type_array))
            for (size_t n = 0; n < json_object_array_length(group); n++)
                add_control(h, json_object_get_string(json_object_array_get_idx(group, n)));
        else {
            json_object_object_foreach(group, name, value) {
                (void)value;
                add_control(h, name);
            }
        }
    }
    char *setting = read_text("/etc/e5/receiver-output", 32);
    if (setting) {
        g_strstrip(setting);
        if (strcmp(setting, "receiver") && strcmp(setting, "speaker")) {
            g_free(setting);
            fail(error, "Invalid receiver output profile");
            goto failed;
        }
        h->shared = !strcmp(setting, "speaker");
        g_free(setting);
    }
    setting = read_text("/etc/e5/receiver-wiring", 32);
    if (setting) {
        g_strstrip(setting);
        if (strcmp(setting, "hpl") && strcmp(setting, "aol")) {
            g_free(setting);
            fail(error, "Invalid receiver wiring");
            goto failed;
        }
        h->aol = !strcmp(setting, "aol");
        g_free(setting);
    }
    setting = read_text("/etc/e5/receiver-volume-ceiling", 32);
    if (setting) {
        long ceiling;
        if (!number(g_strstrip(setting), &ceiling) || ceiling < 1 || ceiling > 15) {
            g_free(setting);
            fail(error, "Invalid receiver volume ceiling");
            goto failed;
        }
        h->ceiling = (int)ceiling;
        g_free(setting);
    }
    h->gains = json_object_new_object();
    Xml xml = {.gains = h->gains};
    /* Older deployed firmware has no codec.xml. Its established analog gains
     * are the defaults in profiles(); this is independent of the C backend. */
    char *codec = native_path("/lib/firmware/codec.xml");
    bool has_codec = g_file_test(codec, G_FILE_TEST_EXISTS);
    g_free(codec);
    if ((has_codec && !parse_xml("/lib/firmware/codec.xml", &xml, error)) || !curves(h, error))
        goto failed;
    char *profile = read_text("/etc/e5/device-profile", 64);
    if (profile && !strcmp(g_strstrip(profile), "yuanhang41s")) {
        h->board = json_object_new_object();
        const char *keys[] = {"digital", "receiver", "receiver_off", "pa_off", "codec", "pa"};
        for (unsigned i = 0; i < G_N_ELEMENTS(keys); i++)
            json_object_object_add(h->board, keys[i], json_object_new_object());
        Xml board = {.board = h->board};
        if (!parse_xml("/lib/firmware/audio_route.xml", &board, error)) {
            g_free(profile);
            goto failed;
        }
        for (unsigned i = 0; i < G_N_ELEMENTS(keys); i++) {
            Json *values = jget(h->board, keys[i]);
            if (!json_object_object_length(values)) {
                g_free(profile);
                fail(error, "Required 41S voice route missing");
                goto failed;
            }
            json_object_object_foreach(values, name, value) {
                (void)value;
                add_control(h, name);
            }
        }
        add_control(h, "S_VOICE_P_HIFI SWITCH");
        if (strcmp(jstr(jget(h->board, "digital"), "SYS_IIS3", ""), "vbc_iism0") ||
            strcmp(jstr(jget(h->board, "pa"), "aw_dev_0_prof", ""), "Music")) {
            g_free(profile);
            fail(error, "Unrecognized 41S PA wiring");
            goto failed;
        }
    }
    g_free(profile);
    if (!alsa_load(&h->a, error))
        goto failed;
    char card[40];
    snprintf(card, sizeof(card), "hw:%d", h->card);
    if (!check(h, h->a.ctl_open(&h->ctl, card, 0), "Open mixer", error))
        goto failed;
    return h;
failed:
    audio_free(h);
    return NULL;
}
Json *audio_apply(VoiceHardware *h, const AudioRequest *r, GError **error) {
    bool ok = true;
    int volume_before = h->volume;
    bool remote_before = h->remote;
    h->volume = CLAMP(r->volume, 0, 15);
    h->remote = r->remote_volume;
    /* A confirmed hangup must close PCM even if an unrelated pending volume
     * or bandwidth change cannot be written. Restore errors are still reported. */
    if (r->live && h->active && (volume_before != h->volume || remote_before != h->remote) &&
        !volume(h, false, error)) {
        h->volume = volume_before;
        h->remote = remote_before;
        ok = false;
    }
    if (ok && r->live && h->active && r->card >= 0 && h->call_card >= 0 && r->card != h->call_card)
        ok = stop_audio(h, error);
    if (ok && r->band >= 0 && r->band <= 3 && r->band != h->band) {
        h->band = r->band;
        if (r->live && h->active)
            ok = profiles(h, error) && volume(h, true, error);
    }
    if (ok && r->live) {
        bool route_changed = strcmp(r->route, h->bluetooth ? "bluetooth"
                                              : h->speaker ? "speaker"
                                                           : "receiver") != 0;
        ok = set_route(h, r->route, r->rate, error);
        if (ok && !h->active) {
            ok = start_audio(h, error);
            if (ok) {
                h->call_card = r->card;
                h->muted = false;
            }
        }
        if (ok && h->active && (h->muted != r->muted || route_changed)) {
            ok = put(h, "VBC ADC2 DG Set", r->muted ? "0,0" : "24,24", error);
            if (ok)
                h->muted = r->muted;
        }
    } else if (ok && h->active)
        ok = stop_audio(h, error);
    Json *out = json_object_new_object();
    jb(out, "available", true);
    jb(out, "active", h->active);
    jb(out, "speaker", h->speaker);
    jb(out, "muted", h->muted);
    js(out, "route", h->bluetooth ? "bluetooth" : h->speaker ? "speaker" : "receiver");
    js(out, "error", !ok && error && *error ? (*error)->message : NULL);
    char pcm[40];
    snprintf(pcm, sizeof(pcm), "hw:%d,%d", h->card, h->device);
    js(out, "pcm", pcm);
    ji(out, "volume", h->volume);
    ji(out, "volume_max", 15);
    if (h->call_card >= 0) {
        ji(out, "card", h->call_card);
        js(out, "sim", h->call_card == 0 ? "SIM1" : "SIM2");
    } else {
        json_object_object_add(out, "card", NULL);
        js(out, "sim", NULL);
    }
    return out;
}
void audio_free(VoiceHardware *h) {
    if (!h)
        return;
    if (h->saved) {
        GError *error = NULL;
        stop_audio(h, &error);
        if (error) {
            g_warning("Audio shutdown restore: %s", error->message);
            g_error_free(error);
        }
    }
    if (h->elements) {
        GHashTableIter iter;
        gpointer value;
        g_hash_table_iter_init(&iter, h->elements);
        while (g_hash_table_iter_next(&iter, NULL, &value))
            element_free(h, value);
        g_hash_table_unref(h->elements);
    }
    if (h->ctl)
        h->a.ctl_close(h->ctl);
    if (h->a.library)
        dlclose(h->a.library);
    if (h->routes)
        json_object_put(h->routes);
    if (h->gains)
        json_object_put(h->gains);
    if (h->board)
        json_object_put(h->board);
    if (h->controls)
        g_ptr_array_unref(h->controls);
    g_free(h);
}
