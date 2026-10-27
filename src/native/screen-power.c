#define _GNU_SOURCE
/* Input-core inhibition only. Never reset/unbind a touch controller. */
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <linux/netlink.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *root;
static volatile sig_atomic_t stopped;
static char lights[32][PATH_MAX], touches[32][PATH_MAX];
static unsigned nl, nt;
static void stop(int sig) {
    (void)sig;
    stopped = 1;
}
static void path(char *out, const char *suffix) {
    if (snprintf(out, PATH_MAX, "%s%s", root, suffix) >= PATH_MAX) {
        errno = ENAMETOOLONG;
        perror("screen-power");
        exit(1);
    }
}
static int read_number(const char *name, int base, long *value) {
    char buf[128], *end;
    int fd = open(name, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    int saved = errno;
    close(fd);
    errno = saved;
    if (n <= 0)
        return -1;
    buf[n] = 0;
    errno = 0;
    *value = strtol(buf, &end, base);
    while (*end == ' ' || *end == '\n' || *end == '\t')
        end++;
    if (errno || end == buf || *end) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}
static int write_number(const char *name, long value) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%ld\n", value);
#ifdef SCREEN_POWER_TEST
    const char *trace = getenv("SCREEN_POWER_TRACE");
    if (trace) {
        FILE *f = fopen(trace, "a");
        if (f) {
            fprintf(f, "%s=%ld\n", strrchr(name, '/') + 1, value);
            fclose(f);
        }
    }
#endif
    int fd = open(name, O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t written = write(fd, buf, (size_t)n);
    int saved = errno;
    close(fd);
    errno = saved;
    if (written != n) {
        errno = EIO;
        return -1;
    }
    return 0;
}
static int discover(void) {
    glob_t list = {0};
    char pattern[PATH_MAX];
    nl = nt = 0;
    path(pattern, "/sys/class/backlight/*/brightness");
    int rc = glob(pattern, 0, NULL, &list);
    if (rc != 0 && rc != GLOB_NOMATCH)
        return -1;
    for (size_t i = 0; i < list.gl_pathc && nl < 32; i++)
        snprintf(lights[nl++], PATH_MAX, "%s", list.gl_pathv[i]);
    globfree(&list);
    memset(&list, 0, sizeof(list));
    path(pattern, "/sys/class/input/input*/properties");
    rc = glob(pattern, 0, NULL, &list);
    if (rc != 0 && rc != GLOB_NOMATCH)
        return -1;
    for (size_t i = 0; i < list.gl_pathc && nt < 32; i++) {
        long props;
        char name[PATH_MAX];
        if (read_number(list.gl_pathv[i], 16, &props) < 0 || !(props & 2))
            continue;
        snprintf(name, sizeof(name), "%s", list.gl_pathv[i]);
        char *last = strrchr(name, '/');
        if (!last)
            continue;
        strcpy(last, "/inhibited");
        if (access(name, F_OK) == 0)
            snprintf(touches[nt++], PATH_MAX, "%s", name);
    }
    globfree(&list);
    if (!nl || !nt) {
        errno = ENODEV;
        return -1;
    }
    return 0;
}
static int allowed(void) {
    /* libuci's command also merges uncommitted /tmp/.uci deltas. Only called
     * on startup or an actual configuration event, never on a periodic tick. */
    int p[2];
    if (pipe2(p, O_CLOEXEC) < 0)
        return -1;
    pid_t child = fork();
    if (child < 0) {
        close(p[0]);
        close(p[1]);
        return -1;
    }
    if (!child) {
        dup2(p[1], STDOUT_FILENO);
        close(p[0]);
        close(p[1]);
        int null = open("/dev/null", O_WRONLY);
        if (null >= 0) {
            dup2(null, STDERR_FILENO);
            close(null);
        }
        execlp("uci", "uci", "-q", "get", "e5-infoscreen.main.touch", NULL);
        _exit(127);
    }
    close(p[1]);
    struct pollfd fd = {p[0], POLLIN, 0};
    char value[64] = {0};
    int ready;
    do {
        ready = poll(&fd, 1, 3000);
    } while (ready < 0 && errno == EINTR && !stopped);
    ssize_t n = ready > 0 ? read(p[0], value, sizeof(value) - 1) : -1;
    close(p[0]);
    if (ready <= 0)
        kill(child, SIGKILL);
    int status;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    if (ready <= 0 || n < 0) {
        errno = ETIMEDOUT;
        return -1;
    }
    return strcmp(value, "0\n") != 0 && strcmp(value, "0") != 0;
}
static int inhibit(bool off) {
    for (unsigned i = 0; i < nt; i++) {
        long value;
        if (read_number(touches[i], 10, &value) < 0)
            return -1;
        if (value != (long)off && write_number(touches[i], off) < 0)
            return -1;
        if (read_number(touches[i], 10, &value) < 0 || value != (long)off) {
            errno = EIO;
            return -1;
        }
    }
    return 0;
}
static int reconcile(int level, int touch) {
    char lockpath[PATH_MAX];
    path(lockpath, "/run/t551m-screen-power.lock");
    int lock = open(lockpath, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock < 0)
        return -1;
    int rc = -1;
    if (flock(lock, LOCK_EX) < 0)
        goto out;
    if (level >= 0) {
        if (inhibit(!(level > 0 && touch)) < 0)
            goto out;
        for (unsigned i = 0; i < nl; i++) {
            long value;
            if (read_number(lights[i], 10, &value) < 0)
                goto out;
            /* Avoid running the driver's backlight update and notifying its
             * watchers again when only touch policy needed reconciliation. */
            if (value != level && write_number(lights[i], level) < 0)
                goto out;
        }
    }
    bool on = false;
    for (unsigned i = 0; i < nl; i++) {
        long value;
        if (read_number(lights[i], 10, &value) < 0)
            goto out;
        on |= value > 0;
    }
    rc = inhibit(!(on && touch));
out:
    {
        int saved = errno;
        close(lock);
        errno = saved;
    }
    return rc;
}
static int kernel_events(void) {
    int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
    struct sockaddr_nl address = {.nl_family = AF_NETLINK, .nl_groups = 1};
    if (fd >= 0 && bind(fd, (void *)&address, sizeof(address)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}
static void config_watches(int fd) {
    char name[PATH_MAX];
    const char *dirs[] = {"/etc/config", "/tmp/.uci", "/tmp"};
    for (unsigned i = 0; i < 3; i++) {
        path(name, dirs[i]);
        inotify_add_watch(fd, name,
                          IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE | IN_CREATE | IN_DELETE_SELF |
                              IN_MOVE_SELF);
    }
    /* Regular-file fixtures exercise external writes; sysfs uses the kernel
     * backlight uevent and POLLPRI on actual_brightness, not inotify. */
    if (*root)
        for (unsigned i = 0; i < nl; i++)
            inotify_add_watch(fd, lights[i], IN_CLOSE_WRITE);
}
static int watch(int touch) {
    int notify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC), events = kernel_events();
    if (notify < 0 || events < 0) {
        perror("screen-power event subscription");
        return 1;
    }
    config_watches(notify);
    struct pollfd fds[34] = {{notify, POLLIN, 0}, {events, POLLIN, 0}};
    unsigned count = 2;
    for (unsigned i = 0; i < nl; i++) {
        char name[PATH_MAX];
        snprintf(name, sizeof(name), "%s", lights[i]);
        strcpy(strrchr(name, '/'), "/actual_brightness");
        int fd = open(name, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        if (fd >= 0) {
            char buf[128];
            (void)read(fd, buf, sizeof(buf));
            fds[count++] = (struct pollfd){fd, POLLPRI | POLLERR, 0};
        }
    }
    while (!stopped) {
        int rc = poll(fds, count, -1);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            perror("screen-power poll");
            return 1;
        }
        bool changed = false, reload = false;
        if (fds[0].revents) {
            union {
                struct inotify_event align;
                char buf[16384];
            } data;
            ssize_t n;
            while ((n = read(notify, data.buf, sizeof(data.buf))) > 0)
                for (ssize_t pos = 0; pos < n;) {
                    struct inotify_event *ev = (void *)(data.buf + pos);
                    pos += (ssize_t)sizeof(*ev) + ev->len;
                    if (!ev->len || !strcmp(ev->name, "e5-infoscreen") || !strcmp(ev->name, ".uci"))
                        reload = true;
                    if (*root)
                        changed = true;
                }
        }
        if (fds[1].revents) {
            char buf[16384];
            struct sockaddr_nl from;
            ssize_t n;
            socklen_t size;
            while (size = sizeof(from),
                   (n = recvfrom(events, buf, sizeof(buf) - 1, 0, (void *)&from, &size)) > 0) {
                if (from.nl_pid != 0)
                    continue;
                buf[n] = 0;
                for (ssize_t p = 0; p < n; p += (ssize_t)strlen(buf + p) + 1) {
                    if (!strcmp(buf + p, "SUBSYSTEM=backlight"))
                        changed = true;
                    if (!strcmp(buf + p, "SUBSYSTEM=input")) {
                        if (discover() < 0)
                            return 1;
                        changed = true;
                    }
                }
            }
        }
        for (unsigned i = 2; i < count; i++)
            if (fds[i].revents) {
                char buf[128];
                lseek(fds[i].fd, 0, SEEK_SET);
                (void)read(fds[i].fd, buf, sizeof(buf));
                changed = true;
            }
        if (reload) {
            config_watches(notify);
            touch = allowed();
            if (touch < 0)
                return 1;
            changed = true;
        }
        if (changed && reconcile(-1, touch) < 0) {
            perror("screen-power reconcile");
            return 1;
        }
    }
    for (unsigned i = 0; i < count; i++)
        close(fds[i].fd);
    return 0;
}
int main(int argc, char **argv) {
    root = getenv("E5_SCREEN_ROOT");
    if (!root)
        root = "";
    umask(077);
    signal(SIGTERM, stop);
    signal(SIGINT, stop);
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: t551m-screen-power watch|sync|set LEVEL\n");
        return 2;
    }
    int level = -1;
    if (!strcmp(argv[1], "set") && argc == 3) {
        char *end;
        errno = 0;
        long v = strtol(argv[2], &end, 10);
        if (errno || !*argv[2] || *end)
            return 2;
        level = v < 0 ? 0 : v > 255 ? 255 : (int)v;
    } else if (argc != 2 || (strcmp(argv[1], "watch") && strcmp(argv[1], "sync")))
        return 2;
    if (discover() < 0) {
        perror("screen-power devices");
        return 1;
    }
    int touch = allowed();
    if (touch < 0 || reconcile(level, touch) < 0) {
        perror("screen-power reconcile");
        return 1;
    }
    return !strcmp(argv[1], "watch") ? watch(touch) : 0;
}
