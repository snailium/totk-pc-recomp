// Linux implementation of platform.h: pthreads, CLOCK_MONOTONIC, futex-backed mutex/cond pairs, POSIX file I/O,
// /proc and /sys for machine load, fork/exec for child processes.
//
// This file is the only place the Linux build touches the OS. It is written to be a drop-in for win32.c: the same
// header, the same semantics, including the two rules the runtime relies on — zero-initialised storage is a valid
// unlocked mutex and an empty condition variable, so both are created lazily here.
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#include "platform.h"

_Static_assert(sizeof(PlatMutex) == sizeof(void *), "PlatMutex holds a pointer to a pthread_mutex_t");
_Static_assert(sizeof(PlatCond) == sizeof(void *), "PlatCond holds a pointer to a pthread_cond_t");

// ---- threads

struct PlatThread {
    pthread_t t;
    PlatThreadFn fn;
    void *arg;
    char name[32];
};

static void *thread_main(void *p) {
    PlatThread *t = p;
    if (t->name[0]) pthread_setname_np(pthread_self(), t->name);   // 16 bytes on GNU/Linux; longer names are cut
    t->fn(t->arg);
    return NULL;
}

PlatThread *plat_thread_new(PlatThreadFn fn, void *arg, const char *name) {
    PlatThread *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->fn = fn;
    t->arg = arg;
    if (name) snprintf(t->name, sizeof t->name, "%s", name);
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 8u << 20);   // the Windows default; the runtime's threads recurse in lifted code
    int r = pthread_create(&t->t, &a, thread_main, t);
    pthread_attr_destroy(&a);
    if (r) { free(t); return NULL; }
    return t;
}

void plat_thread_join(PlatThread *t) {
    pthread_join(t->t, NULL);
    free(t);
}

// Detached threads keep their PlatThread allocated: thread_main still reads t->fn and t->arg after this returns.
void plat_thread_detach(PlatThread *t) {
    pthread_detach(t->t);
}

uint64_t plat_thread_affinity(PlatThread *t, uint64_t mask) {
    cpu_set_t set, old;
    CPU_ZERO(&set);
    for (int i = 0; i < 64; i++)
        if (mask >> i & 1) CPU_SET(i, &set);
    pthread_t th = t ? t->t : pthread_self();
    if (pthread_getaffinity_np(th, sizeof old, &old)) return 0;
    if (pthread_setaffinity_np(th, sizeof set, &set)) return 0;
    uint64_t prev = 0;
    for (int i = 0; i < 64; i++)
        if (CPU_ISSET(i, &old)) prev |= 1ull << i;
    return prev;
}

uint32_t plat_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (uint32_t)n : 1;
}

// ---- time

uint64_t plat_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

// clock_nanosleep is already high resolution; the last PLAT_SPIN_NS are spent yielding so the deadline is met closely
// (same shape as win32.c, where the waitable timer is the coarse part and the spin is the precise one).
#define PLAT_SPIN_NS 300000u

void plat_sleep_ns(uint64_t ns) {
    uint64_t end = plat_now_ns() + ns;
    if (ns > PLAT_SPIN_NS) {
        struct timespec ts;
        uint64_t at = end - PLAT_SPIN_NS;
        ts.tv_sec = (time_t)(at / 1000000000u);
        ts.tv_nsec = (long)(at % 1000000000u);
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {
        }
    }
    while (plat_now_ns() < end) sched_yield();
}

// ---- locks and condition variables
//
// PlatMutex / PlatCond are one pointer each and must be usable when zero-initialised, so the pthread object is created
// on first use. The creation is rare (once per lock), so a global spinlock is enough.

static pthread_mutex_t init_lock = PTHREAD_MUTEX_INITIALIZER;

static void *slot_create(void **slot, size_t size, int is_cond) {
    void *p = __atomic_load_n((void **)slot, __ATOMIC_ACQUIRE);
    if (p) return p;
    pthread_mutex_lock(&init_lock);
    p = *slot;
    if (!p) {
        p = calloc(1, size);
        if (p) {
            if (is_cond) {
                pthread_condattr_t a;
                pthread_condattr_init(&a);
                pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
                pthread_cond_init(p, &a);
                pthread_condattr_destroy(&a);
            } else {
                pthread_mutexattr_t a;
                pthread_mutexattr_init(&a);
                pthread_mutexattr_settype(&a, PTHREAD_MUTEX_NORMAL);
                pthread_mutex_init(p, &a);
                pthread_mutexattr_destroy(&a);
            }
            __atomic_store_n((void **)slot, p, __ATOMIC_RELEASE);
        }
    }
    pthread_mutex_unlock(&init_lock);
    return p;
}

static pthread_mutex_t *mutex_of(PlatMutex *m) {
    return slot_create(&m->p, sizeof(pthread_mutex_t), 0);
}

static pthread_cond_t *cond_of(PlatCond *c) {
    return slot_create(&c->p, sizeof(pthread_cond_t), 1);
}

void plat_mutex_init(PlatMutex *m) { (void)mutex_of(m); }

void plat_mutex_lock(PlatMutex *m) {
    pthread_mutex_t *p = mutex_of(m);
    if (p) pthread_mutex_lock(p);
}

int plat_mutex_trylock(PlatMutex *m) {
    pthread_mutex_t *p = mutex_of(m);
    return p && pthread_mutex_trylock(p) == 0;
}

void plat_mutex_unlock(PlatMutex *m) {
    pthread_mutex_t *p = m->p;
    if (p) pthread_mutex_unlock(p);
}

void plat_cond_init(PlatCond *c) { (void)cond_of(c); }

void plat_cond_wait(PlatCond *c, PlatMutex *m) {
    pthread_cond_t *cp = cond_of(c);
    pthread_mutex_t *mp = mutex_of(m);
    if (cp && mp) pthread_cond_wait(cp, mp);
}

int plat_cond_wait_ns(PlatCond *c, PlatMutex *m, uint64_t ns) {
    pthread_cond_t *cp = cond_of(c);
    pthread_mutex_t *mp = mutex_of(m);
    if (!cp || !mp) return 1;
    uint64_t at = plat_now_ns() + ns;
    struct timespec ts = {(time_t)(at / 1000000000u), (long)(at % 1000000000u)};
    int r = pthread_cond_timedwait(cp, mp, &ts);
    return r != ETIMEDOUT;   // spurious wakeups report 1, as win32.c does
}

void plat_cond_signal(PlatCond *c) {
    pthread_cond_t *p = c->p;
    if (p) pthread_cond_signal(p);
}

void plat_cond_broadcast(PlatCond *c) {
    pthread_cond_t *p = c->p;
    if (p) pthread_cond_broadcast(p);
}

// ---- files

static int mkdir_one(const char *path) {
    return mkdir(path, 0755) == 0 || errno == EEXIST;
}

void plat_file_mkdir_p(const char *path) {
    char buf[PATH_MAX];
    size_t n = strlen(path);
    if (n >= sizeof buf) return;
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i < n; i++) {
        if (buf[i] != '/') continue;
        buf[i] = 0;
        mkdir_one(buf);
        buf[i] = '/';
    }
    mkdir_one(buf);
}

int plat_file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

uint8_t *plat_file_read(const char *path, uint64_t *size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || (uint64_t)st.st_size > (1ull << 30)) {
        close(fd);
        return NULL;
    }
    uint8_t *buf = malloc((size_t)st.st_size + 1);
    if (!buf) { close(fd); return NULL; }
    size_t got = 0;
    while (got < (size_t)st.st_size) {
        ssize_t n = read(fd, buf + got, (size_t)st.st_size - got);
        if (n <= 0) { free(buf); close(fd); return NULL; }
        got += (size_t)n;
    }
    close(fd);
    buf[got] = 0;
    if (size) *size = got;
    return buf;
}

int plat_file_write(const char *path, const void *data, size_t n) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return 0;
    const uint8_t *p = data;
    size_t done = 0;
    while (done < n) {
        ssize_t w = write(fd, p + done, n - done);
        if (w <= 0) { close(fd); return 0; }
        done += (size_t)w;
    }
    close(fd);
    return 1;
}

struct PlatAppend {
    int fd;
};

PlatAppend *plat_append_open(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return NULL;
    PlatAppend *f = malloc(sizeof *f);
    if (!f) { close(fd); return NULL; }
    f->fd = fd;
    return f;
}

int plat_append_write(PlatAppend *f, const void *data, size_t n) {
    const uint8_t *p = data;
    size_t done = 0;
    while (done < n) {
        ssize_t w = write(f->fd, p + done, n - done);   // unbuffered: the bytes are in the file when this returns
        if (w <= 0) return 0;
        done += (size_t)w;
    }
    return 1;
}

int plat_file_replace(const char *from, const char *to) {
    return rename(from, to) == 0;
}

int plat_file_write_atomic(const char *path, const void *data, size_t n) {
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof tmp, "%s.%d.%u.tmp", path, (int)getpid(), plat_thread_id()) >= (int)sizeof tmp) return 0;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) {
        unlink(tmp);
        fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) return 0;
    }
    const uint8_t *p = data;
    size_t done = 0;
    int ok = 1;
    while (done < n) {
        ssize_t w = write(fd, p + done, n - done);
        if (w <= 0) { ok = 0; break; }
        done += (size_t)w;
    }
    if (ok && fsync(fd)) ok = 0;
    if (close(fd)) ok = 0;
    if (ok && rename(tmp, path)) ok = 0;
    if (!ok) unlink(tmp);
    return ok;
}

int plat_file_flush(const char *path) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    int ok = fsync(fd) == 0;
    close(fd);
    return ok;
}

void plat_file_delete(const char *path) { unlink(path); }

int plat_file_truncate(const char *path, uint64_t size) {
    return truncate(path, (off_t)size) == 0;
}

int64_t plat_file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) || !S_ISREG(st.st_mode)) return -1;
    return (int64_t)st.st_size;
}

int plat_file_set_readonly(const char *path, int on) {
    return chmod(path, on ? 0444 : 0644) == 0;
}

int plat_file_list(const char *dir, const char *ext, int (*cb)(const char *name, void *ctx), void *ctx) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    size_t el = strlen(ext);
    struct dirent *e;
    int keep = 1;
    while (keep && (e = readdir(d))) {
        size_t nl = strlen(e->d_name);
        if (el > nl || (el && strcmp(e->d_name + nl - el, ext))) continue;
        if (!cb(e->d_name, ctx)) keep = 0;
    }
    closedir(d);
    return 1;
}

int plat_exe_path(char *out, size_t cap) {
    ssize_t n = readlink("/proc/self/exe", out, cap ? cap - 1 : 0);
    if (n <= 0 || (size_t)n >= cap) return 0;
    out[n] = 0;
    return 1;
}

size_t plat_getenv(const char *name, char *out, size_t cap) {
    const char *v = getenv(name);
    if (!v || !*v) return 0;
    size_t n = strlen(v);
    if (n >= cap) return 0;
    memcpy(out, v, n + 1);
    return n;
}

uint32_t plat_thread_id(void) {
    return (uint32_t)syscall(SYS_gettid);
}

// "en_US.UTF-8" from the environment is a POSIX locale; the menu and the ini want a BCP-47 tag ("en-US").
const char *plat_ui_language(void) {
    static char lang[32];
    if (lang[0]) return lang;
    const char *v = getenv("LC_ALL");
    if (!v || !*v) v = getenv("LC_MESSAGES");
    if (!v || !*v) v = getenv("LANG");
    if (!v || !*v || !strcmp(v, "C") || !strcmp(v, "POSIX")) return "";
    size_t n = 0;
    while (v[n] && v[n] != '.' && v[n] != '@' && n < sizeof lang - 1) {
        lang[n] = v[n] == '_' ? '-' : v[n];
        n++;
    }
    lang[n] = 0;
    return lang;
}

// System fonts for the menu's language names. fontconfig would be the general answer; the catalogue only needs CJK, and
// every distribution that ships it installs these files.
static const char *const cjk_fonts_ja[] = {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                                           "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc", NULL};
static const char *const cjk_fonts_ko[] = {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                                           "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc", NULL};
static const char *const cjk_fonts_zh[] = {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                                           "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
                                           "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc", NULL};

int plat_font_path(const char *lang, char *out, size_t cap) {
    const char *const *list = NULL;
    if (!strncmp(lang, "ja", 2)) list = cjk_fonts_ja;
    else if (!strncmp(lang, "ko", 2)) list = cjk_fonts_ko;
    else if (!strncmp(lang, "zh", 2)) list = cjk_fonts_zh;
    if (!list) return 0;
    for (int i = 0; list[i]; i++) {
        if (!plat_file_exists(list[i])) continue;
        size_t n = strlen(list[i]);
        if (n >= cap) return 0;
        memcpy(out, list[i], n + 1);
        return 1;
    }
    return 0;
}

int plat_os_version(char *out, size_t cap) {
    uint64_t n = 0;
    uint8_t *buf = plat_file_read("/etc/os-release", &n);
    if (!buf) return 0;
    const char *p = (const char *)buf;
    int ok = 0;
    while (*p) {
        if (!strncmp(p, "PRETTY_NAME=", 12)) {
            const char *v = p + 12;
            char q = (*v == '"' || *v == '\'') ? *v : 0;
            if (q) v++;
            size_t len = 0;
            while (v[len] && v[len] != '\n' && (!q || v[len] != q)) len++;
            if (len >= cap) len = cap - 1;
            memcpy(out, v, len);
            out[len] = 0;
            ok = 1;
            break;
        }
        p = strchr(p, '\n');
        if (!p) break;
        p++;
    }
    free(buf);
    return ok;
}

int plat_stderr_path(char *out, size_t cap) {
    char link[64];
    snprintf(link, sizeof link, "/proc/self/fd/%d", (int)STDERR_FILENO);
    ssize_t n = readlink(link, out, cap ? cap - 1 : 0);
    if (n <= 0 || (size_t)n >= cap) return 0;
    out[n] = 0;
    struct stat st;
    if (stat(out, &st) || !S_ISREG(st.st_mode)) return 0;
    return 1;
}

// ---- desktop

// Runs argv with stdin fed from data (when data is not NULL); returns 1 when the program was started. Used for the two
// desktop helpers, where a helper that is not installed is normal and must not be fatal.
static int run_helper(char *const argv[], const void *data, size_t n) {
    int fds[2] = {-1, -1};
    if (data && pipe(fds)) return 0;
    pid_t pid = fork();
    if (pid < 0) {
        if (fds[0] >= 0) { close(fds[0]); close(fds[1]); }
        return 0;
    }
    if (pid == 0) {
        if (data) {
            dup2(fds[0], STDIN_FILENO);
            close(fds[0]);
            close(fds[1]);
        }
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) {
            dup2(null, STDOUT_FILENO);
            dup2(null, STDERR_FILENO);
            if (null > 2) close(null);
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    if (data) {
        close(fds[0]);
        const uint8_t *p = data;
        size_t done = 0;
        while (done < n) {
            ssize_t w = write(fds[1], p + done, n - done);
            if (w <= 0) break;
            done += (size_t)w;
        }
        close(fds[1]);
    }
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

int plat_clipboard_set(const char *utf8) {
    size_t n = strlen(utf8);
    char *wl[] = {"wl-copy", NULL};
    if (run_helper(wl, utf8, n)) return 1;
    char *xc[] = {"xclip", "-selection", "clipboard", NULL};
    return run_helper(xc, utf8, n);
}

int plat_open_folder(const char *path) {
    char *argv[] = {"xdg-open", (char *)path, NULL};
    return run_helper(argv, NULL, 0);
}

// ---- dynamic libraries

// The Windows call passes a bare module name ("vulkan-1") and Windows appends ".dll". A bare name here is the stem of
// a soname, so the versioned and the development names are both tried, then the name as given.
void *plat_lib_open(const char *name) {
    if (strchr(name, '/')) return dlopen(name, RTLD_NOW | RTLD_LOCAL);
    char path[PATH_MAX];
    snprintf(path, sizeof path, "lib%s.so", name);
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        snprintf(path, sizeof path, "lib%s.so.1", name);
        h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    }
    if (!h) h = dlopen(name, RTLD_NOW | RTLD_LOCAL);
    return h;
}

void *plat_lib_sym(void *lib, const char *name) { return lib ? dlsym(lib, name) : NULL; }

// ---- machine load (perfmon.c's sampling thread)

struct PlatLoad {
    uint64_t cpu_total, cpu_idle;   // the machine, in /proc/stat jiffies
    uint64_t proc_ticks;            // this process, user + system
    uint64_t last_ns;               // when the counters were last read
    char gpu_busy[PATH_MAX];        // /sys/class/drm/cardN/device/gpu_busy_percent, "" when the driver has none
    char gpu_vram[PATH_MAX];        // .../mem_info_vram_used
    long hz;
};

// The card whose DRM node owns the Vulkan device. Vulkan reports a 16-byte deviceUUID; amdgpu exposes the same UUID in
// /sys/class/drm/cardN/device/unique_id (without dashes), so the LUID is compared as a hex prefix. A machine with one
// adapter needs no matching at all, which is the case on both targets.
static void find_gpu_sysfs(PlatLoad *l) {
    DIR *d = opendir("/sys/class/drm");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "card", 4)) continue;
        int digits = 1;
        for (const char *p = e->d_name + 4; *p; p++)
            if (*p < '0' || *p > '9') digits = 0;
        if (!digits) continue;   // cardN-DP-1 and friends are connectors, not the device
        char base[300];
        snprintf(base, sizeof base, "/sys/class/drm/%s/device", e->d_name);
        char busy[340], vram[340];
        snprintf(busy, sizeof busy, "%s/gpu_busy_percent", base);
        snprintf(vram, sizeof vram, "%s/mem_info_vram_used", base);
        if (access(busy, R_OK)) continue;
        snprintf(l->gpu_busy, sizeof l->gpu_busy, "%s", busy);
        if (!access(vram, R_OK)) snprintf(l->gpu_vram, sizeof l->gpu_vram, "%s", vram);
        break;
    }
    closedir(d);
}

PlatLoad *plat_load_open(const uint8_t *gpu_luid) {
    (void)gpu_luid;   // the DRM scan picks the first adapter with a busy counter; a single-GPU machine is the target
    PlatLoad *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    l->hz = sysconf(_SC_CLK_TCK);
    if (l->hz <= 0) l->hz = 100;
    find_gpu_sysfs(l);
    plat_cpu_load(l, NULL, NULL, NULL);   // prime the deltas, so the first sample the overlay takes is meaningful
    return l;
}

void plat_load_close(PlatLoad *l) { free(l); }

static int read_u64(const char *path, uint64_t *out) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    char buf[64];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    char *end;
    unsigned long long v = strtoull(buf, &end, 10);
    if (end == buf) return 0;
    *out = v;
    return 1;
}

// user + system jiffies of this process, from /proc/self/stat. utime and stime are fields 14 and 15 of the line, i.e.
// 12 and 13 after the comm field's closing parenthesis.
static uint64_t proc_cpu_ticks(void) {
    char buf[512];
    int fd = open("/proc/self/stat", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    char *p = strrchr(buf, ')');
    if (!p) return 0;
    p++;
    unsigned long long ut = 0, st = 0;
    for (int f = 3; f <= 15 && *p; f++) {
        while (*p == ' ') p++;
        if (f == 14) ut = strtoull(p, &p, 10);
        else if (f == 15) { st = strtoull(p, &p, 10); break; }
        else while (*p && *p != ' ') p++;
    }
    return ut + st;
}

void plat_cpu_load(PlatLoad *l, float *total_pct, float *proc_pct, float *temp_c) {
    if (total_pct) *total_pct = -1;
    if (proc_pct) *proc_pct = -1;
    if (temp_c) *temp_c = -1;
    unsigned long long user = 0, nice = 0, sys = 0, idle = 0, iow = 0, irq = 0, sirq = 0, steal = 0;
    FILE *f = fopen("/proc/stat", "r");
    if (f) {
        if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &user, &nice, &sys, &idle, &iow, &irq, &sirq,
                   &steal) >= 4) {
            uint64_t total = user + nice + sys + idle + iow + irq + sirq + steal;
            uint64_t idle_all = idle + iow;
            if (l->cpu_total && total > l->cpu_total && total_pct)
                *total_pct = 100.0f * (float)((total - l->cpu_total) - (idle_all - l->cpu_idle)) /
                             (float)(total - l->cpu_total);
            l->cpu_total = total;
            l->cpu_idle = idle_all;
        }
        fclose(f);
    }
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu <= 0) ncpu = 1;
    uint64_t now = plat_now_ns();
    uint64_t t = proc_cpu_ticks();
    if (proc_pct && l->proc_ticks && t >= l->proc_ticks && l->last_ns && now > l->last_ns) {
        double secs = (double)(now - l->last_ns) / 1e9;
        double cpu_secs = (double)(t - l->proc_ticks) / (double)l->hz;
        *proc_pct = (float)(100.0 * cpu_secs / (secs * (double)ncpu));   // of all logical processors
    }
    l->proc_ticks = t;
    l->last_ns = now;
    float hottest = -1;
    char zone[64];
    for (int i = 0; i < 32; i++) {
        snprintf(zone, sizeof zone, "/sys/class/thermal/thermal_zone%d/temp", i);
        uint64_t milli;
        if (!read_u64(zone, &milli)) continue;
        float c = (float)milli / 1000.0f;
        if (c > hottest) hottest = c;
    }
    if (hottest >= 0 && temp_c) *temp_c = hottest;
}

void plat_gpu_load(PlatLoad *l, float *load_pct, float *vram_used_mb) {
    if (load_pct) *load_pct = -1;
    if (vram_used_mb) *vram_used_mb = -1;
    uint64_t v;
    if (l->gpu_busy[0] && read_u64(l->gpu_busy, &v) && load_pct) *load_pct = (float)v;
    if (l->gpu_vram[0] && read_u64(l->gpu_vram, &v) && vram_used_mb) *vram_used_mb = (float)v / (1024.0f * 1024.0f);
}

static void meminfo_line(const char *key, float *out_mb) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;
    char buf[256];
    while (fgets(buf, sizeof buf, f)) {
        if (strncmp(buf, key, strlen(key))) continue;
        unsigned long long kb = 0;
        if (out_mb && sscanf(buf + strlen(key), ": %llu", &kb) == 1) *out_mb = (float)kb / 1024.0f;
        break;
    }
    fclose(f);
}

void plat_mem_info(float *used_mb, float *total_mb, float *proc_mb) {
    if (total_mb) *total_mb = -1;
    if (used_mb) *used_mb = -1;
    if (proc_mb) *proc_mb = -1;
    meminfo_line("MemTotal", total_mb);
    float avail = -1;
    meminfo_line("MemAvailable", &avail);
    if (used_mb && *total_mb > 0 && avail >= 0) *used_mb = *total_mb - avail;
    if (proc_mb) {
        FILE *f = fopen("/proc/self/status", "r");
        if (f) {
            char buf[256];
            while (fgets(buf, sizeof buf, f)) {
                unsigned long long kb = 0;
                if (sscanf(buf, "VmRSS: %llu kB", &kb) == 1) {
                    *proc_mb = (float)kb / 1024.0f;
                    break;
                }
            }
            fclose(f);
        }
    }
}

// ---- child processes

struct PlatChild {
    pid_t pid;
    int in, out;
};

PlatChild *plat_child_start(const char *exe, const char *arg, uint32_t *err) {
    int to_child[2] = {-1, -1}, from_child[2] = {-1, -1};
    if (pipe2(to_child, O_CLOEXEC) || pipe2(from_child, O_CLOEXEC)) {
        if (err) *err = (uint32_t)errno;
        return NULL;
    }
    pid_t pid = fork();
    if (pid < 0) {
        if (err) *err = (uint32_t)errno;
        close(to_child[0]); close(to_child[1]);
        close(from_child[0]); close(from_child[1]);
        return NULL;
    }
    if (pid == 0) {
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        int null = open("/dev/null", O_WRONLY);
        if (null >= 0) { dup2(null, STDERR_FILENO); if (null > 2) close(null); }
        close(to_child[0]); close(to_child[1]);
        close(from_child[0]); close(from_child[1]);
        if (arg) {
            char *argv[] = {(char *)exe, (char *)arg, NULL};
            execv(exe, argv);
        } else {
            char *argv[] = {(char *)exe, NULL};
            execv(exe, argv);
        }
        _exit(127);
    }
    close(to_child[0]);
    close(from_child[1]);
    PlatChild *c = calloc(1, sizeof *c);
    if (!c) {
        close(to_child[1]); close(from_child[0]);
        return NULL;
    }
    c->pid = pid;
    c->in = to_child[1];
    c->out = from_child[0];
    return c;
}

void plat_child_stop(PlatChild *c) {
    if (!c) return;
    close(c->in);
    close(c->out);
    kill(c->pid, SIGKILL);
    waitpid(c->pid, NULL, 0);
    free(c);
}

int plat_child_write(PlatChild *c, const void *data, size_t n) {
    const uint8_t *p = data;
    size_t done = 0;
    while (done < n) {
        ssize_t w = write(c->in, p + done, n - done);
        if (w <= 0) return 0;
        done += (size_t)w;
    }
    return 1;
}

long plat_child_read(PlatChild *c, void *buf, size_t cap) {
    for (;;) {
        ssize_t n = read(c->out, buf, cap);
        if (n > 0) return (long)n;
        if (n == 0) return 0;
        if (errno == EINTR) continue;
        return -1;
    }
}

// ---- memory

// Commits read-write pages inside a reservation. A range that is not mapped yet gets an anonymous mapping at its exact
// address; the caller (memory.c) has already reserved the window, so a conflicting mapping is a bug, not a fallback.
int plat_mem_commit(uint64_t addr, size_t n) {
    void *p = (void *)(uintptr_t)addr;
    if (mprotect(p, n, PROT_READ | PROT_WRITE) == 0) return 1;
    if (errno != ENOMEM && errno != EACCES) return 0;
    void *m = mmap(p, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    return m == p;
}

// Copies n bytes out of guest memory without faulting the process: an unmapped range returns 0.
int plat_mem_read(uint64_t addr, void *out, size_t n) {
    struct iovec local = {out, n}, remote = {(void *)(uintptr_t)addr, n};
    ssize_t r = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
    return r == (ssize_t)n;
}
