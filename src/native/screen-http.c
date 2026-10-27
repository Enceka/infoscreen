#define _GNU_SOURCE
/* Loopback SSE and bounded WebSocket GET transport. No shell or remote URL. */
#include "native.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>

typedef struct Client Client;
typedef struct Request Request;
struct ScreenHttp {
    GSocketService *service;
    GHashTable *clients;
    char *payload;
    unsigned port, keepalive;
    guint64 revision;
    bool closing;
};
struct Client {
    int refs;
    ScreenHttp *server;
    GSocketConnection *connection;
    GCancellable *cancel;
    GByteArray *input, *fragments;
    GQueue output, requests;
    GHashTable *pending;
    size_t queued;
    bool writing, closed, websocket, sse, fragmented, close_after_write;
    guint64 revision;
    unsigned active, handshake_timer;
};
struct Request {
    Client *client;
    int64_t id;
    char *path;
    GSocketClient *dialer;
    GSocketConnection *connection;
    GCancellable *cancel;
    GByteArray *reply;
    unsigned timeout;
    bool done;
};
static Client *client_ref(Client *c) {
    c->refs++;
    return c;
}
static void request_free(Request *r);
static void client_unref(Client *c) {
    if (--c->refs)
        return;
    while (!g_queue_is_empty(&c->output))
        g_bytes_unref(g_queue_pop_head(&c->output));
    g_byte_array_unref(c->input);
    g_byte_array_unref(c->fragments);
    g_hash_table_unref(c->pending);
    g_object_unref(c->connection);
    g_object_unref(c->cancel);
    g_free(c);
}
static void close_client(Client *c) {
    if (c->closed)
        return;
    c->closed = true;
    if (c->handshake_timer) {
        g_source_remove(c->handshake_timer);
        c->handshake_timer = 0;
    }
    g_cancellable_cancel(c->cancel);
    g_io_stream_close(G_IO_STREAM(c->connection), NULL, NULL);
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, c->pending);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        Request *r = value;
        g_cancellable_cancel(r->cancel);
    }
    while (!g_queue_is_empty(&c->requests))
        request_free(g_queue_pop_head(&c->requests));
    if (c->server) {
        ScreenHttp *s = c->server;
        c->server = NULL;
        g_hash_table_remove(s->clients, c);
    }
}
typedef struct {
    Client *client;
    GBytes *bytes;
} Write;
static void pump(Client *c);
static void sse(Client *c, const char *value);
static void written(GObject *object, GAsyncResult *res, gpointer data) {
    Write *w = data;
    Client *c = w->client;
    GError *error = NULL;
    gsize size;
    bool ok = g_output_stream_write_all_finish(G_OUTPUT_STREAM(object), res, &size, &error);
    c->queued -= g_bytes_get_size(w->bytes);
    c->writing = false;
    g_bytes_unref(w->bytes);
    g_free(w);
    if (!ok)
        close_client(c);
    else if (!c->closed) {
        if (!c->queued && c->close_after_write)
            close_client(c);
        else if (c->sse && !c->queued && c->server && c->revision != c->server->revision)
            sse(c, c->server->payload);
        else
            pump(c);
    }
    g_clear_error(&error);
    client_unref(c);
}
static void pump(Client *c) {
    if (c->writing || c->closed || g_queue_is_empty(&c->output))
        return;
    Write *w = g_new0(Write, 1);
    w->client = client_ref(c);
    w->bytes = g_queue_pop_head(&c->output);
    c->writing = true;
    gsize n;
    const void *data = g_bytes_get_data(w->bytes, &n);
    g_output_stream_write_all_async(g_io_stream_get_output_stream(G_IO_STREAM(c->connection)), data,
                                    n, G_PRIORITY_DEFAULT, c->cancel, written, w);
}
static void send_bytes(Client *c, const void *data, size_t size) {
    if (c->closed)
        return;
    if (c->queued + size > 4 * 1024 * 1024) {
        close_client(c);
        return;
    }
    c->queued += size;
    g_queue_push_tail(&c->output, g_bytes_new(data, size));
    pump(c);
}
static void frame(Client *c, unsigned opcode, const void *payload, size_t size) {
    unsigned char header[10] = {(unsigned char)(128 | opcode), 0};
    size_t n = 2;
    if (size < 126)
        header[1] = (unsigned char)size;
    else if (size < 65536) {
        header[1] = 126;
        header[2] = (unsigned char)(size >> 8);
        header[3] = (unsigned char)size;
        n = 4;
    } else {
        header[1] = 127;
        for (unsigned i = 0; i < 8; i++)
            header[2 + i] = (unsigned char)((uint64_t)size >> (56 - i * 8));
        n = 10;
    }
    GByteArray *wire = g_byte_array_sized_new(n + size);
    g_byte_array_append(wire, header, n);
    g_byte_array_append(wire, payload, size);
    send_bytes(c, wire->data, wire->len);
    g_byte_array_unref(wire);
}
static void json_frame(Client *c, Json *value) {
    const char *s = encode(value);
    frame(c, 1, s, strlen(s));
}
static void sse(Client *c, const char *value) {
    if (c->queued)
        return; /* one current state, never a per-client event history */
    c->revision = c->server->revision;
    char *text = g_strdup_printf("data: %s\n\n", value);
    send_bytes(c, text, strlen(text));
    g_free(text);
}
static GHashTable *headers(const char *text) {
    GHashTable *out = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    char **lines = g_strsplit(text, "\r\n", -1);
    for (unsigned i = 1; lines[i] && *lines[i]; i++) {
        char *colon = strchr(lines[i], ':');
        if (!colon)
            continue;
        *colon = 0;
        char *name = g_ascii_strdown(lines[i], -1);
        /* Duplicate security/framing headers are not accepted. */
        if (g_hash_table_contains(out, name)) {
            g_free(name);
            g_hash_table_unref(out);
            g_strfreev(lines);
            return NULL;
        }
        g_hash_table_insert(out, name, g_strdup(g_strstrip(colon + 1)));
    }
    g_strfreev(lines);
    return out;
}
static const char *header(GHashTable *h, const char *key) {
    const char *v = h ? g_hash_table_lookup(h, key) : NULL;
    return v ? v : "";
}
static void start_requests(Client *c);
static void request_free(Request *r) {
    if (r->timeout)
        g_source_remove(r->timeout);
    if (r->connection)
        g_object_unref(r->connection);
    if (r->dialer)
        g_object_unref(r->dialer);
    if (r->reply)
        g_byte_array_unref(r->reply);
    g_hash_table_remove(r->client->pending, &r->id);
    g_object_unref(r->cancel);
    Client *c = r->client;
    g_free(r->path);
    g_free(r);
    client_unref(c);
}
static bool unchunk(const guint8 *data, size_t size, GByteArray *out) {
    size_t offset = 0;
    while (offset < size) {
        size_t end = offset;
        while (end + 1 < size && !(data[end] == '\r' && data[end + 1] == '\n'))
            end++;
        if (end + 1 >= size || end - offset > 128)
            return false;
        char *line = g_strndup((const char *)data + offset, end - offset), *tail;
        errno = 0;
        uint64_t n = g_ascii_strtoull(line, &tail, 16);
        bool valid = tail != line && (!*tail || *tail == ';') && !errno;
        g_free(line);
        if (!valid || n > 2 * 1024 * 1024 || out->len + n > 2 * 1024 * 1024)
            return false;
        offset = end + 2;
        if (n == 0) {
            /* Only complete CRLF-delimited trailers, never arbitrary tail bytes. */
            while (offset + 1 < size) {
                if (data[offset] == '\r' && data[offset + 1] == '\n')
                    return offset + 2 == size;
                size_t stop = offset;
                while (stop + 1 < size && !(data[stop] == '\r' && data[stop + 1] == '\n'))
                    stop++;
                if (stop + 1 >= size || !memchr(data + offset, ':', stop - offset))
                    return false;
                offset = stop + 2;
            }
            return false;
        }
        if (n > size - offset || size - offset - n < 2 || data[offset + n] != '\r' ||
            data[offset + n + 1] != '\n')
            return false;
        g_byte_array_append(out, data + offset, n);
        offset += (size_t)n + 2;
    }
    return false;
}
static Json *http_reply(Request *r) {
    Json *out = json_object_new_object();
    json_object_object_add(out, "id", json_object_new_int64(r->id));
    size_t length = r->reply->len, split = 0;
    for (size_t i = 0; i + 3 < length && i < 8192; i++)
        if (!memcmp(r->reply->data + i, "\r\n\r\n", 4)) {
            split = i + 4;
            break;
        }
    bool ok = false;
    GHashTable *h = NULL;
    GByteArray *body = NULL;
    char *head = NULL;
    if (!split)
        goto done;
    head = g_strndup((char *)r->reply->data, split);
    h = headers(head);
    if (!h)
        goto done;
    int status = 0;
    if (sscanf(head, "HTTP/%*u.%*u %d", &status) != 1 || status < 100 || status > 599)
        goto done;
    char *type = g_ascii_strdown(header(h, "content-type"), -1);
    bool json = strstr(type, "application/json") != NULL;
    g_free(type);
    if (!json)
        goto done;
    body = g_byte_array_new();
    const char *te = header(h, "transfer-encoding");
    if (*te) {
        if (g_ascii_strcasecmp(te, "chunked") ||
            !unchunk(r->reply->data + split, length - split, body))
            goto done;
    } else {
        if (length - split > 2 * 1024 * 1024)
            goto done;
        const char *cl = header(h, "content-length");
        char *end = NULL;
        if (*cl) {
            guint64 expected = g_ascii_strtoull(cl, &end, 10);
            if (end == cl || *end || expected != length - split)
                goto done;
        }
        g_byte_array_append(body, r->reply->data + split, length - split);
    }
    if (!g_utf8_validate((char *)body->data, body->len, NULL))
        goto done;
    ji(out, "status", status);
    js(out, "type", header(h, "content-type"));
    json_object_object_add(out, "body", json_object_new_string_len((char *)body->data, body->len));
    ok = true;
done:
    if (!ok)
        jb(out, "fallback", true); /* existing bounded GET transport contract */
    if (body)
        g_byte_array_unref(body);
    if (h)
        g_hash_table_unref(h);
    g_free(head);
    return out;
}
static void finish_request(Request *r, bool ok) {
    if (r->done)
        return;
    r->done = true;
    Client *c = client_ref(r->client);
    if (!c->closed) {
        Json *value = ok ? http_reply(r) : json_object_new_object();
        if (!ok) {
            json_object_object_add(value, "id", json_object_new_int64(r->id));
            jb(value, "fallback", true);
        }
        json_frame(c, value);
        json_object_put(value);
    }
    c->active--;
    request_free(r);
    if (!c->closed)
        start_requests(c);
    client_unref(c);
}
static gboolean request_timeout(gpointer data) {
    Request *r = data;
    r->timeout = 0;
    g_cancellable_cancel(r->cancel);
    return G_SOURCE_REMOVE;
}
static void response_read(GObject *object, GAsyncResult *res, gpointer data) {
    Request *r = data;
    GError *error = NULL;
    GBytes *bytes = g_input_stream_read_bytes_finish(G_INPUT_STREAM(object), res, &error);
    if (!bytes) {
        g_clear_error(&error);
        finish_request(r, false);
        return;
    }
    gsize n;
    const void *value = g_bytes_get_data(bytes, &n);
    if (r->reply->len + n > 2 * 1024 * 1024 + 8192) {
        g_bytes_unref(bytes);
        finish_request(r, false);
        return;
    }
    g_byte_array_append(r->reply, value, n);
    g_bytes_unref(bytes);
    if (!n) {
        finish_request(r, true);
        return;
    }
    g_input_stream_read_bytes_async(G_INPUT_STREAM(object), 65536, G_PRIORITY_DEFAULT, r->cancel,
                                    response_read, r);
}
static void http_written(GObject *object, GAsyncResult *res, gpointer data) {
    Request *r = data;
    GError *error = NULL;
    gsize n;
    bool ok = g_output_stream_write_all_finish(G_OUTPUT_STREAM(object), res, &n, &error);
    char *wire = g_object_steal_data(object, "e5-wire");
    g_free(wire);
    if (!ok) {
        g_clear_error(&error);
        finish_request(r, false);
        return;
    }
    g_input_stream_read_bytes_async(g_io_stream_get_input_stream(G_IO_STREAM(r->connection)), 65536,
                                    G_PRIORITY_DEFAULT, r->cancel, response_read, r);
}
static void http_connected(GObject *object, GAsyncResult *res, gpointer data) {
    Request *r = data;
    GError *error = NULL;
    r->connection = g_socket_client_connect_to_host_finish(G_SOCKET_CLIENT(object), res, &error);
    if (!r->connection) {
        g_clear_error(&error);
        finish_request(r, false);
        return;
    }
    char *wire =
        g_strdup_printf("GET %s HTTP/1.0\r\nHost: 127.0.0.1:8088\r\nAccept: "
                        "application/json\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n",
                        r->path);
    GOutputStream *stream = g_io_stream_get_output_stream(G_IO_STREAM(r->connection));
    g_object_set_data_full(G_OBJECT(stream), "e5-wire", wire, g_free);
    g_output_stream_write_all_async(stream, wire, strlen(wire), G_PRIORITY_DEFAULT, r->cancel,
                                    http_written, r);
}
static void start_requests(Client *c) {
    while (c->active < 4 && !g_queue_is_empty(&c->requests) && !c->closed) {
        Request *r = g_queue_pop_head(&c->requests);
        c->active++;
        r->dialer = g_socket_client_new();
        g_socket_client_set_enable_proxy(r->dialer, FALSE);
        g_socket_client_set_timeout(r->dialer, 8);
        r->timeout = g_timeout_add_seconds(8, request_timeout, r);
        g_socket_client_connect_to_host_async(r->dialer, "127.0.0.1", 8088, r->cancel,
                                              http_connected, r);
    }
}
static bool request(Client *c, const guint8 *bytes, size_t size) {
    if (!g_utf8_validate((const char *)bytes, size, NULL))
        return false;
    struct json_tokener *tok = json_tokener_new_ex(16);
    Json *value = json_tokener_parse_ex(tok, (const char *)bytes, (int)size);
    size_t used = json_tokener_get_parse_end(tok);
    while (used < size && g_ascii_isspace(bytes[used]))
        used++;
    bool valid = json_tokener_get_error(tok) == json_tokener_success && used == size;
    json_tokener_free(tok);
    Json *id = value ? jget(value, "id") : NULL;
    const char *path = value ? jstr(value, "path", "") : "";
    int64_t number = id ? json_object_get_int64(id) : 0;
    valid = valid && id && json_object_is_type(id, json_type_int) && number > 0 &&
            number < INT64_C(9007199254740992) && g_str_has_prefix(path, "/api/") &&
            strlen(path) <= 4096 && !strchr(path, '#') && !strchr(path, '\\') &&
            !strstr(path, "/..");
    for (const unsigned char *p = (const unsigned char *)path; valid && *p; p++)
        if (*p < 33 || *p > 126)
            valid = false;
    if (valid && g_hash_table_contains(c->pending, &number))
        valid = false;
    if (valid && g_hash_table_size(c->pending) >= 8) {
        Json *out = json_object_new_object();
        json_object_object_add(out, "id", json_object_new_int64(number));
        jb(out, "fallback", true);
        json_frame(c, out);
        json_object_put(out);
    } else if (valid) {
        Request *r = g_new0(Request, 1);
        r->client = client_ref(c);
        r->id = number;
        r->path = g_strdup(path);
        r->cancel = g_cancellable_new();
        r->reply = g_byte_array_new();
        g_hash_table_insert(c->pending, &r->id, r);
        g_queue_push_tail(&c->requests, r);
        start_requests(c);
    }
    if (value)
        json_object_put(value);
    return valid;
}
static bool websocket(Client *c) {
    while (c->input->len >= 2) {
        guint8 *data = c->input->data;
        bool final = data[0] & 128;
        unsigned opcode = data[0] & 15;
        uint64_t n = data[1] & 127;
        size_t head = 2;
        if ((data[0] & 112) || !(data[1] & 128))
            return false;
        if (n == 126) {
            if (c->input->len < 4)
                return true;
            n = ((unsigned)data[2] << 8) | data[3];
            head = 4;
            if (n < 126)
                return false;
        } else if (n == 127) {
            if (c->input->len < 10)
                return true;
            n = 0;
            for (unsigned i = 0; i < 8; i++)
                n = (n << 8) | data[2 + i];
            head = 10;
            if (n < 65536)
                return false;
        }
        if (n > 8192 || (opcode >= 8 && (!final || n > 125)))
            return false;
        if (c->input->len < head + 4 + n)
            return true;
        guint8 payload[8192];
        for (size_t i = 0; i < n; i++)
            payload[i] = data[head + 4 + i] ^ data[head + (i % 4)];
        g_byte_array_remove_range(c->input, 0, head + 4 + (size_t)n);
        if (opcode == 8) {
            if (n == 1 || (n > 2 && !g_utf8_validate((char *)payload + 2, n - 2, NULL)))
                return false;
            c->close_after_write = true;
            frame(c, 8, payload, n);
            return true;
        }
        if (opcode == 9) {
            frame(c, 10, payload, n);
            continue;
        }
        if (opcode == 10)
            continue;
        if (opcode == 1 && !c->fragmented)
            g_byte_array_set_size(c->fragments, 0);
        else if (opcode != 0 || !c->fragmented)
            return false;
        if (c->fragments->len + n > 8192)
            return false;
        g_byte_array_append(c->fragments, payload, n);
        c->fragmented = !final;
        if (final && !request(c, c->fragments->data, c->fragments->len))
            return false;
    }
    return true;
}
static bool handshake(Client *c) {
    size_t split = 0;
    for (size_t i = 0; i + 3 < c->input->len; i++)
        if (!memcmp(c->input->data + i, "\r\n\r\n", 4)) {
            split = i + 4;
            break;
        }
    if (!split)
        return c->input->len <= 8192;
    if (split > 8192 || memchr(c->input->data, 0, split))
        return false;
    char *text = g_strndup((char *)c->input->data, split);
    GHashTable *h = headers(text);
    char path[32], version[16];
    bool ok = h && sscanf(text, "GET %31s %15s", path, version) == 2;
    char *host = g_strdup_printf("127.0.0.1:%u", c->server->port);
    ok = ok && !strcmp(header(h, "host"), host) &&
         !strcmp(header(h, "origin"), "http://127.0.0.1:8088");
    g_free(host);
    if (ok && !strcmp(path, "/events")) {
        c->sse = true;
        const char *reply = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: "
                            "no-store\r\nAccess-Control-Allow-Origin: "
                            "http://127.0.0.1:8088\r\nVary: Origin\r\nConnection: close\r\n\r\n";
        send_bytes(c, reply, strlen(reply));
        c->revision = c->server->revision;
        char *initial = g_strdup_printf("data: %s\n\n", c->server->payload);
        send_bytes(c, initial, strlen(initial));
        g_free(initial);
    } else if (ok && !strcmp(path, "/rpc")) {
        const char *key = header(h, "sec-websocket-key");
        char *upgrade = g_ascii_strdown(header(h, "upgrade"), -1),
             *connection = g_ascii_strdown(header(h, "connection"), -1);
        char **tokens = g_strsplit(connection, ",", -1);
        bool token = false;
        for (unsigned i = 0; tokens[i]; i++)
            if (!strcmp(g_strstrip(tokens[i]), "upgrade"))
                token = true;
        ok = !strcmp(version, "HTTP/1.1") && !strcmp(upgrade, "websocket") && token &&
             !strcmp(header(h, "sec-websocket-version"), "13") &&
             g_regex_match_simple("^[A-Za-z0-9+/]{22}==$", key, 0, 0);
        g_strfreev(tokens);
        g_free(upgrade);
        g_free(connection);
        if (ok) {
            char *seed = g_strconcat(key, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", NULL);
            GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA1);
            g_checksum_update(sum, (guchar *)seed, strlen(seed));
            g_free(seed);
            guint8 digest[20];
            gsize n = 20;
            g_checksum_get_digest(sum, digest, &n);
            g_checksum_free(sum);
            char *accept = g_base64_encode(digest, n);
            char *reply = g_strdup_printf(
                "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: "
                "Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n",
                accept);
            g_free(accept);
            send_bytes(c, reply, strlen(reply));
            g_free(reply);
            c->websocket = true;
        }
    } else
        ok = false;
    g_free(text);
    if (h)
        g_hash_table_unref(h);
    if (ok) {
        g_byte_array_remove_range(c->input, 0, split);
        if (c->handshake_timer) {
            g_source_remove(c->handshake_timer);
            c->handshake_timer = 0;
        }
        if (c->websocket)
            ok = websocket(c);
    }
    return ok;
}
static void client_read(GObject *object, GAsyncResult *res, gpointer data) {
    Client *c = data;
    GError *error = NULL;
    GBytes *bytes = g_input_stream_read_bytes_finish(G_INPUT_STREAM(object), res, &error);
    gsize n = 0;
    const void *value = bytes ? g_bytes_get_data(bytes, &n) : NULL;
    if (!n || c->closed) {
        if (!c->closed)
            close_client(c);
        goto out;
    }
    if (c->input->len + n > 16384) {
        close_client(c);
        goto out;
    }
    g_byte_array_append(c->input, value, n);
    if (c->sse || !(c->websocket ? websocket(c) : handshake(c))) {
        close_client(c);
        goto out;
    }
    if (!c->close_after_write)
        g_input_stream_read_bytes_async(G_INPUT_STREAM(object), 4096, G_PRIORITY_DEFAULT, c->cancel,
                                        client_read, client_ref(c));
out:
    if (bytes)
        g_bytes_unref(bytes);
    g_clear_error(&error);
    client_unref(c);
}
static gboolean handshake_timeout(gpointer data) {
    Client *c = data;
    c->handshake_timer = 0;
    close_client(c);
    return G_SOURCE_REMOVE;
}
static gboolean incoming(GSocketService *service, GSocketConnection *connection, GObject *source,
                         gpointer data) {
    (void)service;
    (void)source;
    ScreenHttp *s = data;
    if (g_hash_table_size(s->clients) >= 6) {
        g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
        return true;
    }
    Client *c = g_new0(Client, 1);
    c->refs = 1;
    c->server = s;
    c->connection = g_object_ref(connection);
    c->cancel = g_cancellable_new();
    c->input = g_byte_array_new();
    c->fragments = g_byte_array_new();
    c->pending = g_hash_table_new(g_int64_hash, g_int64_equal);
    g_hash_table_add(s->clients, c);
    c->handshake_timer = g_timeout_add_seconds_full(G_PRIORITY_DEFAULT, 3, handshake_timeout,
                                                    client_ref(c), (GDestroyNotify)client_unref);
    g_input_stream_read_bytes_async(g_io_stream_get_input_stream(G_IO_STREAM(connection)), 4096,
                                    G_PRIORITY_DEFAULT, c->cancel, client_read, client_ref(c));
    return true;
}
static gboolean keepalive(gpointer data) {
    ScreenHttp *s = data;
    GHashTableIter iter;
    gpointer c;
    g_hash_table_iter_init(&iter, s->clients);
    /* Sending may close a blocked peer; iterate a referenced snapshot. */
    GPtrArray *clients = g_ptr_array_new_with_free_func((GDestroyNotify)client_unref);
    while (g_hash_table_iter_next(&iter, &c, NULL))
        g_ptr_array_add(clients, client_ref(c));
    for (unsigned i = 0; i < clients->len; i++) {
        Client *p = g_ptr_array_index(clients, i);
        if (p->sse && !p->queued)
            send_bytes(p, ": alive\n\n", 9);
    }
    g_ptr_array_unref(clients);
    return G_SOURCE_CONTINUE;
}
ScreenHttp *screen_http_new(guint16 port, GError **error) {
    ScreenHttp *s = g_new0(ScreenHttp, 1);
    s->port = port;
    s->service = g_socket_service_new();
    s->clients =
        g_hash_table_new_full(g_direct_hash, g_direct_equal, (GDestroyNotify)client_unref, NULL);
    s->payload = g_strdup("{\"error\":\"Call observer is starting\",\"calls\":[]}");
    GInetAddress *ip = g_inet_address_new_from_string("127.0.0.1");
    GSocketAddress *address = g_inet_socket_address_new(ip, port);
    g_object_unref(ip);
    bool ok =
        g_socket_listener_add_address(G_SOCKET_LISTENER(s->service), address, G_SOCKET_TYPE_STREAM,
                                      G_SOCKET_PROTOCOL_TCP, NULL, NULL, error);
    g_object_unref(address);
    if (!ok) {
        screen_http_free(s);
        return NULL;
    }
    g_signal_connect(s->service, "incoming", G_CALLBACK(incoming), s);
    g_socket_service_start(s->service);
    s->keepalive = g_timeout_add_seconds(30, keepalive, s);
    return s;
}
static Json *stable(Json *value, unsigned depth) {
    if (!value || depth > 24)
        return NULL;
    if (json_object_is_type(value, json_type_object)) {
        Json *out = json_object_new_object();
        json_object_object_foreach(value, key, child) {
            if (strcmp(key, "updated"))
                json_object_object_add(out, key, stable(child, depth + 1));
        }
        return out;
    }
    if (json_object_is_type(value, json_type_array)) {
        Json *out = json_object_new_array();
        for (size_t i = 0; i < json_object_array_length(value); i++)
            json_object_array_add(out, stable(json_object_array_get_idx(value, i), depth + 1));
        return out;
    }
    return json_object_get(value);
}
void screen_http_publish(ScreenHttp *s, Json *state, bool wake) {
    Json *value = json_object_new_object(), *calls = jget(state, "calls");
    json_object_object_add(value, "calls",
                           calls ? json_object_get(calls) : json_object_new_array());
    json_object_object_add(value, "audio", json_object_get(state));
    jb(value, "wake", wake);
    js(value, "error",
       jbool(state, "available", false) && calls
           ? NULL
           : jstr(state, "error", "Call observer unavailable"));
    Json *clean = stable(value, 0);
    json_object_put(value);
    const char *payload = encode(clean);
    if (strlen(payload) > 65536 || !strcmp(payload, s->payload)) {
        json_object_put(clean);
        return;
    }
    g_free(s->payload);
    s->payload = g_strdup(payload);
    s->revision++;
    json_object_put(clean);
    GPtrArray *clients = g_ptr_array_new_with_free_func((GDestroyNotify)client_unref);
    GHashTableIter iter;
    gpointer c;
    g_hash_table_iter_init(&iter, s->clients);
    while (g_hash_table_iter_next(&iter, &c, NULL))
        g_ptr_array_add(clients, client_ref(c));
    for (unsigned i = 0; i < clients->len; i++) {
        Client *p = g_ptr_array_index(clients, i);
        if (p->sse)
            sse(p, s->payload);
    }
    g_ptr_array_unref(clients);
}
void screen_http_power(ScreenHttp *s) {
    GPtrArray *clients = g_ptr_array_new_with_free_func((GDestroyNotify)client_unref);
    GHashTableIter iter;
    gpointer c;
    g_hash_table_iter_init(&iter, s->clients);
    while (g_hash_table_iter_next(&iter, &c, NULL))
        g_ptr_array_add(clients, client_ref(c));
    for (unsigned i = 0; i < clients->len; i++) {
        Client *p = g_ptr_array_index(clients, i);
        if (p->websocket)
            frame(p, 1, "{\"event\":\"power-supply-change\"}",
                  strlen("{\"event\":\"power-supply-change\"}"));
    }
    g_ptr_array_unref(clients);
}
void screen_http_free(ScreenHttp *s) {
    if (!s)
        return;
    s->closing = true;
    if (s->keepalive)
        g_source_remove(s->keepalive);
    if (s->service) {
        g_socket_service_stop(s->service);
        g_socket_listener_close(G_SOCKET_LISTENER(s->service));
        g_object_unref(s->service);
    }
    while (g_hash_table_size(s->clients)) {
        GHashTableIter iter;
        gpointer c;
        g_hash_table_iter_init(&iter, s->clients);
        g_hash_table_iter_next(&iter, &c, NULL);
        client_ref(c);
        close_client(c);
        client_unref(c);
    }
    g_hash_table_unref(s->clients);
    g_free(s->payload);
    g_free(s);
}
