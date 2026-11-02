// Checks the Linux platform layer against platform.h: the semantics the runtime relies on, not a benchmark. Runs
// without the game; `tests/platform_test.c` is to `platform/linux.c` what `--selftest` is to the game.
//
//   cc -std=gnu11 -O2 -Wall -Wextra -pthread tests/platform_test.c runtime/platform/linux.c -o platform_test
//   ./platform_test
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../runtime/platform/platform.h"

static int failures;

#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, what); failures++; } } while (0)

// ---- threads

static PlatMutex counter_lock;          // zero-initialised on purpose: the runtime has static locks
static int counter;
static volatile int ready;

static void bump(void *arg) {
    (void)arg;
    plat_mutex_lock(&counter_lock);
    counter += 1;
    plat_mutex_unlock(&counter_lock);
}

static PlatMutex wait_lock;
static PlatCond wait_cond;

static void wait_worker(void *arg) {
    (void)arg;
    plat_mutex_lock(&wait_lock);
    ready = 1;
    while (ready == 1) plat_cond_wait(&wait_cond, &wait_lock);
    plat_mutex_unlock(&wait_lock);
}

// ---- file list callback

static int listed;
static int count_cb(const char *name, void *ctx) {
    (void)name;
    (void)ctx;
    listed++;
    return 1;
}

// ---- child process

static void test_child(void) {
    PlatChild *c = plat_child_start("/bin/echo", "hello from the child", NULL);
    CHECK(c != NULL, "plat_child_start(/bin/echo)");
    if (!c) return;
    char buf[256];
    long n = plat_child_read(c, buf, sizeof buf - 1);
    CHECK(n > 0, "child produced output");
    if (n > 0) {
        buf[n] = 0;
        CHECK(strstr(buf, "hello from the child") != NULL, "child output content");
    }
    plat_child_stop(c);
}

int main(void) {
    // threads, time
    CHECK(plat_cpu_count() >= 1, "cpu count");
    uint64_t a = plat_now_ns();
    plat_sleep_ns(5 * 1000 * 1000);
    uint64_t elapsed = plat_now_ns() - a;
    CHECK(elapsed >= 5 * 1000 * 1000, "sleep does not return early");
    CHECK(elapsed < 200 * 1000 * 1000, "sleep roughly on time");

    PlatThread *t = plat_thread_new(bump, NULL, "bump");
    CHECK(t != NULL, "thread new");
    plat_thread_join(t);
    CHECK(counter == 1, "thread ran once");

    CHECK(plat_thread_affinity(NULL, 1) != 0, "affinity reads the current mask");
    plat_thread_affinity(NULL, (uint64_t)-1);

    // t->fn is still read after detach, so the struct must stay alive
    PlatThread *d = plat_thread_new(bump, NULL, "detached");
    CHECK(d != NULL, "detached thread new");
    if (d) plat_thread_detach(d);
    plat_sleep_ns(20 * 1000 * 1000);
    CHECK(counter == 2, "detached thread ran");

    // locks: static zero initialisation, trylock behaviour
    CHECK(plat_mutex_trylock(&counter_lock) == 1, "trylock on a free lock");
    CHECK(plat_mutex_trylock(&counter_lock) == 0, "trylock on a held lock");
    plat_mutex_unlock(&counter_lock);

    PlatThread *w = plat_thread_new(wait_worker, NULL, "waiter");
    CHECK(w != NULL, "cond waiter thread");
    while (!ready) plat_sleep_ns(1000 * 1000);
    plat_mutex_lock(&wait_lock);
    ready = 0;
    plat_cond_signal(&wait_cond);
    plat_mutex_unlock(&wait_lock);
    plat_thread_join(w);

    plat_mutex_lock(&wait_lock);
    uint64_t t0 = plat_now_ns();
    int woke = plat_cond_wait_ns(&wait_cond, &wait_lock, 10 * 1000 * 1000);
    uint64_t waited = plat_now_ns() - t0;
    plat_mutex_unlock(&wait_lock);
    CHECK(woke == 0, "timed wait reports the timeout");
    CHECK(waited >= 10 * 1000 * 1000, "timed wait waited");

    // files
    char dir[256];
    snprintf(dir, sizeof dir, "/tmp/totk-platform-test-%d", (int)getpid());
    char path[512], path2[512], path3[512];
    snprintf(path, sizeof path, "%s/sub/a.bin", dir);
    snprintf(path2, sizeof path2, "%s/sub/b.bin", dir);
    snprintf(path3, sizeof path3, "%s/append.log", dir);
    snprintf(path, sizeof path, "%s/sub", dir);
    plat_file_mkdir_p(path);
    snprintf(path, sizeof path, "%s/sub/a.bin", dir);
    CHECK(!plat_file_exists(dir), "a directory is not a file");
    const char payload[] = "platform test payload";
    CHECK(plat_file_write(path, payload, sizeof payload - 1), "write");
    CHECK(plat_file_exists(path), "write makes the file exist");
    CHECK(plat_file_size(path) == (int64_t)sizeof payload - 1, "file size");
    uint64_t n = 0;
    uint8_t *back = plat_file_read(path, &n);
    CHECK(back != NULL, "read");
    if (back) {
        CHECK(n == sizeof payload - 1, "read size");
        CHECK(memcmp(back, payload, n) == 0, "read content");
        CHECK(back[n] == 0, "read NUL-terminates");
        free(back);
    }
    CHECK(plat_file_truncate(path, 8), "truncate");
    CHECK(plat_file_size(path) == 8, "truncate size");
    CHECK(plat_file_write_atomic(path2, payload, sizeof payload - 1), "atomic write");
    CHECK(plat_file_size(path2) == (int64_t)sizeof payload - 1, "atomic write size");
    CHECK(plat_file_replace(path2, path), "replace");
    CHECK(plat_file_size(path) == (int64_t)sizeof payload - 1, "replace kept the new content");
    CHECK(plat_file_flush(path), "flush");
    CHECK(plat_file_set_readonly(path, 1), "set readonly");
    CHECK(!plat_file_write(path, "x", 1), "readonly file refuses writes");
    CHECK(plat_file_set_readonly(path, 0), "clear readonly");

    PlatAppend *ap = plat_append_open(path3);
    CHECK(ap != NULL, "append open");
    if (ap) {
        CHECK(plat_append_write(ap, "one\n", 4), "append one");
        CHECK(plat_append_write(ap, "two\n", 4), "append two");
        CHECK(plat_file_size(path3) == 8, "append size");
    }
    listed = 0;
    CHECK(plat_file_list(dir, ".log", count_cb, NULL), "list");
    CHECK(listed == 1, "list finds the one .log file");

    // paths, environment, identity
    char exe[512];
    CHECK(plat_exe_path(exe, sizeof exe), "exe path");
    CHECK(exe[0] == '/', "exe path is absolute");
    static char env[8192];
    CHECK(plat_getenv("PATH", env, sizeof env) > 0, "getenv PATH");
    CHECK(plat_thread_id() != 0, "thread id");
    char os[128];
    CHECK(plat_os_version(os, sizeof os) && os[0], "os version");
    const char *lang = plat_ui_language();
    CHECK(lang != NULL, "ui language never NULL");

    // desktop helpers: not installed in every environment, so only "does not crash"
    plat_clipboard_set("totk platform test");
    CHECK(plat_lib_open("z") != NULL, "dlopen libz");
    CHECK(plat_lib_open("this-library-does-not-exist") == NULL, "dlopen missing library");

    test_child();

    // machine load
    PlatLoad *load = plat_load_open(NULL);
    CHECK(load != NULL, "load open");
    if (load) {
        float total = -1, proc = -1, temp = -1;
        plat_sleep_ns(100 * 1000 * 1000);
        plat_cpu_load(load, &total, &proc, &temp);
        CHECK(total >= 0 && total <= 100, "cpu load in range");
        float gl = -1, vram = -1;
        plat_gpu_load(load, &gl, &vram);
        float used = -1, memtotal = -1, pmem = -1;
        plat_mem_info(&used, &memtotal, &pmem);
        CHECK(memtotal > 0, "mem total");
        CHECK(pmem > 0, "process memory");
        plat_load_close(load);
    }

    // memory
    uint8_t heap[64];
    memset(heap, 0x42, sizeof heap);
    uint8_t out[64];
    CHECK(plat_mem_read((uint64_t)(uintptr_t)heap, out, sizeof out) == 1, "mem read from the heap");
    CHECK(out[0] == 0x42 && out[63] == 0x42, "mem read content");
    CHECK(plat_mem_read(0x1000, out, sizeof out) == 0, "mem read of an unmapped page fails instead of faulting");

    plat_file_delete(path);
    CHECK(!plat_file_exists(path), "delete removes the file");
    plat_file_delete(path2);
    plat_file_delete(path3);
    rmdir(dir);

    printf("platform test: %s (%d failures)\n", failures ? "failed" : "passed", failures);
    return failures ? 1 : 0;
}
