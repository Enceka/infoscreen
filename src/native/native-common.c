#define _GNU_SOURCE
#include "native.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

char *native_path(const char *name) {
    const char *root = g_getenv("E5_NATIVE_ROOT");
    return g_strconcat(root ? root : "", name, NULL);
}
char *read_bytes(const char *name, gsize maximum, gsize *length) {
    char *path = native_path(name);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    g_free(path);
    if (fd < 0)
        return NULL;
    GString *value = g_string_sized_new(256);
    char chunk[4096];
    ssize_t n;
    while ((n = read(fd, chunk, sizeof(chunk))) > 0) {
        if (value->len + (size_t)n > maximum) {
            g_string_free(value, TRUE);
            close(fd);
            return NULL;
        }
        g_string_append_len(value, chunk, n);
    }
    close(fd);
    if (n < 0) {
        g_string_free(value, TRUE);
        return NULL;
    }
    if (length)
        *length = value->len;
    return g_string_free(value, FALSE);
}
char *read_text(const char *name, gsize maximum) {
    return read_bytes(name, maximum, NULL);
}
Json *read_json(const char *path) {
    char *value = read_text(path, 2 * 1024 * 1024);
    if (!value)
        return NULL;
    Json *out = json_tokener_parse(value);
    g_free(value);
    return out;
}
bool write_json(const char *name, Json *value) {
    char *path = native_path(name), *dir = g_path_get_dirname(path),
         *tmp = g_strconcat(path, ".part", NULL);
    bool ok = false;
    int fd = -1;
    if (g_mkdir_with_parents(dir, 0700) < 0)
        goto out;
    fd = open(tmp, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
    if (fd < 0)
        goto out;
    const char *data = encode(value);
    size_t length = strlen(data), offset = 0;
    while (offset < length) {
        ssize_t n = write(fd, data + offset, length - offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            goto out;
        offset += (size_t)n;
    }
    if (close(fd) < 0) {
        fd = -1;
        goto out;
    }
    fd = -1;
    ok = rename(tmp, path) == 0;
out:
    if (fd >= 0)
        close(fd);
    if (!ok)
        unlink(tmp);
    g_free(tmp);
    g_free(dir);
    g_free(path);
    return ok;
}
