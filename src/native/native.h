#ifndef E5_SCREEN_NATIVE_H
#define E5_SCREEN_NATIVE_H
#include <gio/gio.h>
#include <glib-unix.h>
#include <json-c/json.h>
#include <stdbool.h>

typedef struct json_object Json;
static inline void json_free(gpointer p) {
    if (p)
        json_object_put(p);
}
static inline Json *jget(Json *o, const char *key) {
    Json *v = NULL;
    return o && json_object_object_get_ex(o, key, &v) ? v : NULL;
}
static inline const char *jstr(Json *o, const char *key, const char *def) {
    Json *v = jget(o, key);
    return v && json_object_is_type(v, json_type_string) ? json_object_get_string(v) : def;
}
static inline int jint(Json *o, const char *key, int def) {
    Json *v = jget(o, key);
    return v && json_object_is_type(v, json_type_int) ? json_object_get_int(v) : def;
}
static inline double jnum(Json *o, const char *key, double def) {
    Json *v = jget(o, key);
    return v ? json_object_get_double(v) : def;
}
static inline bool jbool(Json *o, const char *key, bool def) {
    Json *v = jget(o, key);
    return v ? json_object_get_boolean(v) : def;
}
static inline void js(Json *o, const char *key, const char *v) {
    json_object_object_add(o, key, v ? json_object_new_string(v) : NULL);
}
static inline void ji(Json *o, const char *key, int v) {
    json_object_object_add(o, key, json_object_new_int(v));
}
static inline void jb(Json *o, const char *key, bool v) {
    json_object_object_add(o, key, json_object_new_boolean(v));
}
static inline void jd(Json *o, const char *key, double v) {
    json_object_object_add(o, key, json_object_new_double(v));
}
static inline const char *encode(Json *o) {
    return json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN);
}
static inline double realtime(void) {
    return g_get_real_time() / 1000000.0;
}
static inline double monotime(void) {
    return g_get_monotonic_time() / 1000000.0;
}
char *native_path(const char *path);
char *read_bytes(const char *path, gsize maximum, gsize *length);
char *read_text(const char *path, gsize maximum);
Json *read_json(const char *path);
bool write_json(const char *path, Json *value);

typedef struct VoiceHardware VoiceHardware;
typedef struct {
    bool live, muted, remote_volume;
    int card, volume, rate, band;
    const char *route;
} AudioRequest;
VoiceHardware *audio_new(GError **error);
Json *audio_apply(VoiceHardware *hw, const AudioRequest *request, GError **error);
void audio_free(VoiceHardware *hw);
bool audio_profile_adjust(unsigned char *data, size_t size, int attenuation, GError **error);
unsigned audio_profile_word(unsigned band, bool speaker, bool bluetooth);

typedef struct ScreenHttp ScreenHttp;
ScreenHttp *screen_http_new(guint16 port, GError **error);
void screen_http_publish(ScreenHttp *server, Json *state, bool wake);
void screen_http_power(ScreenHttp *server);
void screen_http_free(ScreenHttp *server);

#endif
