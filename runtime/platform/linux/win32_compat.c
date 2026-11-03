// Implementations for win32_compat.h: POSIX underneath, Windows semantics on top.
//
// Two pieces carry real weight and are worth reading before the rest:
//   * write watch — Windows tracks written pages in the kernel; here watched pages are mprotect(RD) and a SIGSEGV
//     handler records the page, makes it writable again and returns. GetWriteWatch(RESET) reports and re-protects.
//   * thread context — SuspendThread/GetThreadContext/SetThreadContext take a signal (SIGUSR2) on the target thread,
//     copy its ucontext into the CONTEXT, keep the thread parked while the caller inspects it, and apply the edited
//     register set back into the ucontext before returning. That is what kernel.c's preemption needs.
#define _GNU_SOURCE
#include "win32_compat.h"

static void watch_install(void);
static void altstack_install(void);
void kern_dump_threads(const char *why);   // kernel.c: SIGQUIT prints every guest thread
uint64_t kern_now_ns(void);                 // host.h: the runtime's clock
void waitstat_add(const char *name, uint64_t ns);   // waitstat.c: TOTK_WAIT_LOG accounting
static void quit_handler(int sig) {
    (void)sig;
    kern_dump_threads("SIGQUIT");
    // The interrupted thread's own last calls (TOTK_LASTCALLS=1): the loop that the dump cannot see, since the lifted
    // code has no frame pointers.
    extern void rt_lastcalls_dump(int n);
    rt_lastcalls_dump(16);
}

static void install_quit_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = quit_handler;
    sa.sa_flags = SA_RESTART | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGQUIT, &sa, NULL);
}

__attribute__((constructor)) static void compat_early(void) { watch_install(); altstack_install(); install_quit_handler(); }

void kern_guest_crash_report(const char *why, uint64_t addr);   // kernel.c: logs the guest stack of a fatal fault
void rt_logf(const char *fmt, ...);   // host.h: the runtime log

#include <dirent.h>
#include <dlfcn.h>
#include <sched.h>
#include <math.h>
#include <stdarg.h>
#include <sys/ioctl.h>

#if defined(__linux__)
#include <linux/fs.h>          // PAGEMAP_SCAN, PAGE_IS_WRITTEN
#include <linux/userfaultfd.h> // UFFD_FEATURE_WP_ASYNC, UFFDIO_*
#endif
#include <strings.h>
#include <sys/statvfs.h>
#include <poll.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

// ---- errors

static __thread DWORD tls_last_error;

DWORD GetLastError(void) { return tls_last_error; }
void SetLastError(DWORD err) { tls_last_error = err; }

// ---- lazily created pthread objects behind zero-initialised storage

static pthread_mutex_t g_slot_lock = PTHREAD_MUTEX_INITIALIZER;

// kind: 0 a mutex for an SRW lock (not recursive, as on Windows), 1 a condition variable, 2 a mutex for a critical
// section (recursive: a Windows CRITICAL_SECTION may be entered again by the thread that holds it, and the runtime does
// that -- vk_staging enters cmd_lock while an upload batch already holds it).
static void *slot(void **p, size_t size, int kind) {
    void *v = __atomic_load_n(p, __ATOMIC_ACQUIRE);
    if (v) return v;
    pthread_mutex_lock(&g_slot_lock);
    v = *p;
    if (!v) {
        v = calloc(1, size);
        if (v) {
            if (kind == 1) {
                pthread_condattr_t a;
                pthread_condattr_init(&a);
                pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
                pthread_cond_init(v, &a);
                pthread_condattr_destroy(&a);
            } else {
                pthread_mutexattr_t a;
                pthread_mutexattr_init(&a);
                pthread_mutexattr_settype(&a, kind == 2 ? PTHREAD_MUTEX_RECURSIVE : PTHREAD_MUTEX_NORMAL);
                pthread_mutex_init(v, &a);
                pthread_mutexattr_destroy(&a);
            }
            __atomic_store_n(p, v, __ATOMIC_RELEASE);
        }
    }
    pthread_mutex_unlock(&g_slot_lock);
    return v;
}

#define MUTEX(l) ((pthread_mutex_t *)slot(&(l)->p, sizeof(pthread_mutex_t), 0))
#define CSMUTEX(l) ((pthread_mutex_t *)slot(&(l)->p, sizeof(pthread_mutex_t), 2))
#define COND(c) ((pthread_cond_t *)slot(&(c)->p, sizeof(pthread_cond_t), 1))

void InitializeSRWLock(PSRWLOCK l) { (void)MUTEX(l); }
void AcquireSRWLockExclusive(PSRWLOCK l) { pthread_mutex_lock(MUTEX(l)); }
void ReleaseSRWLockExclusive(PSRWLOCK l) { pthread_mutex_unlock((pthread_mutex_t *)l->p); }
void AcquireSRWLockShared(PSRWLOCK l) { pthread_mutex_lock(MUTEX(l)); }
void ReleaseSRWLockShared(PSRWLOCK l) { pthread_mutex_unlock((pthread_mutex_t *)l->p); }
BOOL TryAcquireSRWLockExclusive(PSRWLOCK l) { return pthread_mutex_trylock(MUTEX(l)) == 0; }
BOOL TryAcquireSRWLockShared(PSRWLOCK l) { return TryAcquireSRWLockExclusive(l); }

void InitializeConditionVariable(PCONDITION_VARIABLE c) { (void)COND(c); }
BOOL SleepConditionVariableSRW(PCONDITION_VARIABLE c, PSRWLOCK l, DWORD ms, ULONG flags) {
    (void)flags;
    pthread_cond_t *cp = COND(c);
    pthread_mutex_t *mp = MUTEX(l);
    uint64_t w0 = kern_now_ns();
    int r;
    if (ms == INFINITE) {
        r = pthread_cond_wait(cp, mp);
        waitstat_add("cond wait (infinite)", kern_now_ns() - w0);
        return r == 0;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    r = pthread_cond_timedwait(cp, mp, &ts);
    waitstat_add("cond wait (timed)", kern_now_ns() - w0);
    return r == 0;
}

void WakeConditionVariable(PCONDITION_VARIABLE c) { pthread_cond_signal(COND(c)); }
void WakeAllConditionVariable(PCONDITION_VARIABLE c) { pthread_cond_broadcast(COND(c)); }

void InitializeCriticalSection(LPCRITICAL_SECTION c) { (void)CSMUTEX(c); }
void EnterCriticalSection(LPCRITICAL_SECTION c) { pthread_mutex_lock(CSMUTEX(c)); }
void LeaveCriticalSection(LPCRITICAL_SECTION c) { pthread_mutex_unlock((pthread_mutex_t *)c->p); }
BOOL TryEnterCriticalSection(LPCRITICAL_SECTION c) { return pthread_mutex_trylock(CSMUTEX(c)) == 0; }
void DeleteCriticalSection(LPCRITICAL_SECTION c) { (void)c; }

// ---- interlocked

LONG InterlockedIncrement(LONG volatile *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
LONG InterlockedDecrement(LONG volatile *p) { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
LONG InterlockedExchange(LONG volatile *p, LONG v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
LONG InterlockedCompareExchange(LONG volatile *p, LONG v, LONG cmp) {
    __atomic_compare_exchange_n(p, &cmp, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return cmp;
}
LONG InterlockedAdd(LONG volatile *p, LONG v) { return __atomic_add_fetch(p, v, __ATOMIC_SEQ_CST); }
LONGLONG InterlockedIncrement64(LONGLONG volatile *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
LONGLONG InterlockedDecrement64(LONGLONG volatile *p) { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
LONGLONG InterlockedExchange64(LONGLONG volatile *p, LONGLONG v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
LONGLONG InterlockedCompareExchange64(LONGLONG volatile *p, LONGLONG v, LONGLONG cmp) {
    __atomic_compare_exchange_n(p, &cmp, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return cmp;
}
LONGLONG InterlockedAdd64(LONGLONG volatile *p, LONGLONG v) { return __atomic_add_fetch(p, v, __ATOMIC_SEQ_CST); }
void *InterlockedExchangePointer(void *volatile *p, void *v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
void *InterlockedCompareExchangePointer(void *volatile *p, void *v, void *cmp) {
    __atomic_compare_exchange_n(p, &cmp, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return cmp;
}

// ---- thread handles, including suspension and context transfer

enum { K_THREAD = 0x54485244, K_FILE = 0x46494c45, K_FIND = 0x464e4421, K_TIMER = 0x54494d52, K_SNAP = 0x534e4150,
       K_PROCESS = 0x50524f43, K_EVENT = 0x45564e54, K_MAP = 0x4d415020 };

typedef struct {
    int kind;
    int fd;
    uint64_t size;
} MapHandle;

typedef struct {
    int kind;
    pthread_mutex_t m;
    pthread_cond_t c;
    int signaled;
    int manual;
} EventHandle;

typedef struct ThreadHandle {
    int kind;
    pthread_t thread;
    pid_t tid;
    int used;
    int detached;
    int suspended;          // 0 = not parked, 1 = parked and published, 2 = released
    CONTEXT ctx;
    int ctx_valid;
    int ctx_modified;
    int exit_code;
} ThreadHandle;

#define MAX_THREAD_HANDLES 512
static ThreadHandle *g_handles[MAX_THREAD_HANDLES];
static pthread_mutex_t g_handle_lock = PTHREAD_MUTEX_INITIALIZER;
static __thread ThreadHandle *tls_handle;

// SI_USER-style machinery for suspend/context
static void suspend_handler(int sig, siginfo_t *si, void *uc_) {
    (void)sig;
    (void)si;
    ucontext_t *uc = uc_;
    ThreadHandle *h = tls_handle;
    if (!h) return;
    greg_t *g = uc->uc_mcontext.gregs;
    h->ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    h->ctx.Rax = (DWORD64)g[REG_RAX];
    h->ctx.Rcx = (DWORD64)g[REG_RCX];
    h->ctx.Rdx = (DWORD64)g[REG_RDX];
    h->ctx.Rbx = (DWORD64)g[REG_RBX];
    h->ctx.Rsp = (DWORD64)g[REG_RSP];
    h->ctx.Rbp = (DWORD64)g[REG_RBP];
    h->ctx.Rsi = (DWORD64)g[REG_RSI];
    h->ctx.Rdi = (DWORD64)g[REG_RDI];
    h->ctx.R8 = (DWORD64)g[REG_R8];
    h->ctx.R9 = (DWORD64)g[REG_R9];
    h->ctx.R10 = (DWORD64)g[REG_R10];
    h->ctx.R11 = (DWORD64)g[REG_R11];
    h->ctx.R12 = (DWORD64)g[REG_R12];
    h->ctx.R13 = (DWORD64)g[REG_R13];
    h->ctx.R14 = (DWORD64)g[REG_R14];
    h->ctx.R15 = (DWORD64)g[REG_R15];
    h->ctx.Rip = (DWORD64)g[REG_RIP];
    h->ctx.EFlags = (DWORD)g[REG_EFL];
    h->ctx_valid = 1;
    h->ctx_modified = 0;
    __atomic_store_n(&h->suspended, 1, __ATOMIC_RELEASE);
    while (__atomic_load_n(&h->suspended, __ATOMIC_ACQUIRE) == 1) syscall(SYS_sched_yield);
    if (h->ctx_modified) {
        g[REG_RAX] = (greg_t)h->ctx.Rax;
        g[REG_RCX] = (greg_t)h->ctx.Rcx;
        g[REG_RDX] = (greg_t)h->ctx.Rdx;
        g[REG_RBX] = (greg_t)h->ctx.Rbx;
        g[REG_RSP] = (greg_t)h->ctx.Rsp;
        g[REG_RBP] = (greg_t)h->ctx.Rbp;
        g[REG_RSI] = (greg_t)h->ctx.Rsi;
        g[REG_RDI] = (greg_t)h->ctx.Rdi;
        g[REG_R8] = (greg_t)h->ctx.R8;
        g[REG_R9] = (greg_t)h->ctx.R9;
        g[REG_R10] = (greg_t)h->ctx.R10;
        g[REG_R11] = (greg_t)h->ctx.R11;
        g[REG_R12] = (greg_t)h->ctx.R12;
        g[REG_R13] = (greg_t)h->ctx.R13;
        g[REG_R14] = (greg_t)h->ctx.R14;
        g[REG_R15] = (greg_t)h->ctx.R15;
        g[REG_RIP] = (greg_t)h->ctx.Rip;
        g[REG_EFL] = (greg_t)h->ctx.EFlags;
    }
    h->ctx_valid = 0;
    __atomic_store_n(&h->suspended, 0, __ATOMIC_RELEASE);
}

// A signal frame is written below the interrupted stack pointer, which on SysV is the red zone: without a separate
// stack, every SuspendThread would destroy up to 128 bytes of live data in the suspended function. Both handlers run on
// an alternate stack, and every thread the runtime creates gets one.
static void altstack_install(void) {
    static const size_t SZ = 64 * 1024;
    void *m = mmap(NULL, SZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (m == MAP_FAILED) return;
    stack_t ss;
    memset(&ss, 0, sizeof ss);
    ss.ss_sp = m;
    ss.ss_size = SZ;
    sigaltstack(&ss, NULL);
}

static void install_suspend_handler(void) {
    static int done;
    if (done) return;
    done = 1;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = suspend_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR2, &sa, NULL);
}

typedef struct {
    ThreadHandle *h;
    PTHREAD_START_ROUTINE fn;
    void *arg;
} ThreadStart;

static void *thread_body(void *p) {
    ThreadStart *s = p;
    tls_handle = s->h;
    s->h->tid = (pid_t)syscall(SYS_gettid);
    s->h->exit_code = (int)s->fn(s->arg);
    free(s);
    return NULL;
}

static ThreadHandle *handle_for_tid(pid_t tid) {
    ThreadHandle *found = NULL;
    pthread_mutex_lock(&g_handle_lock);
    for (int i = 0; i < MAX_THREAD_HANDLES; i++)
        if (g_handles[i] && g_handles[i]->tid == tid && g_handles[i]->used) { found = g_handles[i]; break; }
    pthread_mutex_unlock(&g_handle_lock);
    return found;
}

HANDLE CreateThread(void *attr, SIZE_T stack, PTHREAD_START_ROUTINE fn, void *arg, DWORD flags, DWORD *id) {
    (void)attr;
    (void)flags;
    install_suspend_handler();
    ThreadHandle *h = calloc(1, sizeof *h);
    ThreadStart *s = calloc(1, sizeof *s);
    if (!h || !s) { free(h); free(s); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    h->kind = K_THREAD;
    h->used = 1;
    s->h = h;
    s->fn = fn;
    s->arg = arg;
    pthread_attr_t a;
    pthread_attr_init(&a);
    if (stack) pthread_attr_setstacksize(&a, stack);
    int r = pthread_create(&h->thread, &a, thread_body, s);
    pthread_attr_destroy(&a);
    if (r) { free(h); free(s); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    for (int i = 0; i < 100000 && !h->tid; i++) sched_yield();   // thread_body publishes it
    pthread_mutex_lock(&g_handle_lock);
    for (int i = 0; i < MAX_THREAD_HANDLES; i++)
        if (!g_handles[i]) { g_handles[i] = h; break; }
    pthread_mutex_unlock(&g_handle_lock);
    if (id) *id = (DWORD)h->tid;
    return h;
}


HANDLE GetCurrentThread(void) {
    if (!tls_handle) {
        ThreadHandle *h = calloc(1, sizeof *h);
        h->kind = K_THREAD;
        h->used = 1;
        h->tid = (pid_t)syscall(SYS_gettid);
        h->thread = pthread_self();
        tls_handle = h;
    }
    return tls_handle;
}

DWORD GetCurrentThreadId(void) { return (DWORD)syscall(SYS_gettid); }
DWORD GetCurrentProcessId(void) { return (DWORD)getpid(); }
HANDLE GetCurrentProcess(void) { return (HANDLE)(intptr_t)-2; }

DWORD SuspendThread(HANDLE h) {
    ThreadHandle *t = h;
    if (!t) { SetLastError(ERROR_INVALID_HANDLE); return (DWORD)-1; }
    if (t == tls_handle) return 0;   // suspending ourselves is a no-op, as Windows documents it is a deadlock
    __atomic_store_n(&t->suspended, 0, __ATOMIC_RELEASE);
    t->ctx_valid = 0;
    if (syscall(SYS_tgkill, getpid(), t->tid, SIGUSR2)) { SetLastError(ESRCH); return (DWORD)-1; }
    for (int i = 0; i < 100000000 && __atomic_load_n(&t->suspended, __ATOMIC_ACQUIRE) == 0; i++) syscall(SYS_sched_yield);
    if (__atomic_load_n(&t->suspended, __ATOMIC_ACQUIRE) != 1) { SetLastError(ERROR_TIMEOUT); return (DWORD)-1; }
    return 1;
}

DWORD ResumeThread(HANDLE h) {
    ThreadHandle *t = h;
    if (!t) return (DWORD)-1;
    int was = __atomic_load_n(&t->suspended, __ATOMIC_ACQUIRE);
    __atomic_store_n(&t->suspended, 2, __ATOMIC_RELEASE);
    return was == 1 ? 1 : 0;
}

BOOL GetThreadContext(HANDLE h, CONTEXT *ctx) {
    ThreadHandle *t = h;
    if (!t || !t->ctx_valid) return FALSE;
    *ctx = t->ctx;
    return TRUE;
}

BOOL SetThreadContext(HANDLE h, const CONTEXT *ctx) {
    ThreadHandle *t = h;
    if (!t || !t->ctx_valid) return FALSE;
    t->ctx = *ctx;
    t->ctx_modified = 1;
    return TRUE;
}

HANDLE OpenThread(DWORD access, BOOL inherit, DWORD tid) {
    (void)access;
    (void)inherit;
    ThreadHandle *h = handle_for_tid((pid_t)tid);
    if (h) return h;
    // The calling thread always has a handle: the preemption code opens its own thread by id.
    if (tid == GetCurrentThreadId()) return GetCurrentThread();
    return NULL;
}

BOOL SetThreadPriority(HANDLE h, int prio) {
    (void)h;
    (void)prio;   // the kernel models priorities itself; host priorities would only add noise
    return TRUE;
}
int GetThreadPriority(HANDLE h) { (void)h; return THREAD_PRIORITY_NORMAL; }

BOOL SetThreadAffinityMask(HANDLE h, DWORD_PTR mask) {
    ThreadHandle *t = h;
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int i = 0; i < 64; i++)
        if (mask >> i & 1) CPU_SET(i, &set);
    pthread_t th = t ? t->thread : pthread_self();
    return pthread_setaffinity_np(th, sizeof set, &set) == 0;
}

void SwitchToThread(void) { sched_yield(); }
DWORD GetActiveProcessorCount(WORD group) { (void)group; return (DWORD)sysconf(_SC_NPROCESSORS_ONLN); }

BOOL SetThreadDescription(HANDLE h, const WCHAR *name) {
    (void)h;
    if (!name) return FALSE;
    char buf[64];
    size_t i = 0;
    for (; name[i] && i < sizeof buf - 1; i++) buf[i] = (char)name[i];
    buf[i] = 0;
    pthread_setname_np(pthread_self(), buf);
    return TRUE;
}


BOOL GetExitCodeThread(HANDLE h, DWORD *code) {
    ThreadHandle *t = h;
    if (!t || !code) return FALSE;
    *code = (DWORD)t->exit_code;
    return TRUE;
}

void GetSystemInfo(void *info) {
    SYSTEM_INFO *si = info;
    memset(si, 0, sizeof *si);
    si->dwNumberOfProcessors = (DWORD)sysconf(_SC_NPROCESSORS_ONLN);
    si->dwPageSize = (DWORD)sysconf(_SC_PAGESIZE);
    si->dwAllocationGranularity = 65536;
    si->wProcessorArchitecture = 9;   // PROCESSOR_ARCHITECTURE_AMD64
}

// ---- time

static uint64_t mono_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

void QueryPerformanceCounter(LARGE_INTEGER *t) { t->QuadPart = (LONGLONG)mono_ns(); }
void QueryPerformanceFrequency(LARGE_INTEGER *f) { f->QuadPart = 1000000000ll; }
DWORD GetTickCount(void) { return (DWORD)(mono_ns() / 1000000u); }
ULONGLONG GetTickCount64(void) { return mono_ns() / 1000000u; }
void timeBeginPeriod(UINT ms) { (void)ms; }
void timeEndPeriod(UINT ms) { (void)ms; }

void Sleep(DWORD ms) {
    if (!ms) { sched_yield(); return; }
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
    }
}

HANDLE CreateWaitableTimerExW(void *attr, const WCHAR *name, DWORD flags, DWORD access) {
    (void)attr; (void)name; (void)flags; (void)access;
    struct Timer { int kind; pthread_mutex_t m; pthread_cond_t c; uint64_t due; int armed; };
    struct Timer *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->kind = K_TIMER;
    pthread_mutex_init(&t->m, NULL);
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(&t->c, &a);
    pthread_condattr_destroy(&a);
    return t;
}

BOOL SetWaitableTimer(HANDLE h, const LARGE_INTEGER *due, LONG period, void *routine, void *arg, BOOL resume) {
    (void)period; (void)routine; (void)arg; (void)resume;
    struct Timer { int kind; pthread_mutex_t m; pthread_cond_t c; uint64_t due; int armed; };
    struct Timer *t = h;
    if (!t || t->kind != K_TIMER) return FALSE;
    pthread_mutex_lock(&t->m);
    // A negative LARGE_INTEGER is a relative time in 100 ns units (Windows); the audio renderer paces its 5 ms buffers
    // with exactly this, and treating the value as nanoseconds made every timer fire 100 times too early.
    t->due = mono_ns() + (uint64_t)(-due->QuadPart) * 100u;
    t->armed = 1;
    pthread_cond_broadcast(&t->c);
    pthread_mutex_unlock(&t->m);
    return TRUE;
}

// ---- files

typedef struct {
    int kind;
    int fd;
} FileHandle;

static int utf8_from_wide(const WCHAR *w, char *out, size_t cap) {
    size_t o = 0;
    for (; *w; w++) {
        uint32_t c = (uint32_t)*w;
        char buf[4];
        int n;
        if (c == '\\') { buf[0] = '/'; n = 1; }   // the runtime builds some paths with Windows separators
        else if (c < 0x80) { buf[0] = (char)c; n = 1; }
        else if (c < 0x800) { buf[0] = (char)(0xC0 | (c >> 6)); buf[1] = (char)(0x80 | (c & 0x3F)); n = 2; }
        else if (c < 0x10000) {
            buf[0] = (char)(0xE0 | (c >> 12)); buf[1] = (char)(0x80 | ((c >> 6) & 0x3F));
            buf[2] = (char)(0x80 | (c & 0x3F)); n = 3;
        } else {
            buf[0] = (char)(0xF0 | (c >> 18)); buf[1] = (char)(0x80 | ((c >> 12) & 0x3F));
            buf[2] = (char)(0x80 | ((c >> 6) & 0x3F)); buf[3] = (char)(0x80 | (c & 0x3F)); n = 4;
        }
        if (o + (size_t)n + 1 > cap) break;
        memcpy(out + o, buf, (size_t)n);
        o += (size_t)n;
    }
    out[o] = 0;
    return (int)o;
}

// Forward slashes become backslashes on the way back: the runtime compares paths with the separator it uses.
static size_t wide_from_utf8(const char *s, WCHAR *out, size_t cap) {
    size_t o = 0;
    while (*s && o + 1 < cap) {
        unsigned char c = (unsigned char)*s;
        uint32_t cp;
        int n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
        else { cp = c & 0x07; n = 4; }
        for (int i = 1; i < n && s[i]; i++) cp = (cp << 6) | ((unsigned char)s[i] & 0x3F);
        s += n;
        out[o++] = (WCHAR)cp;
    }
    if (cap) out[o] = 0;
    return o;
}

static int open_flags(DWORD access, DWORD disposition) {
    int f = 0;
    if ((access & GENERIC_READ) && (access & GENERIC_WRITE)) f = O_RDWR;
    else if (access & GENERIC_WRITE) f = O_WRONLY;
    else f = O_RDONLY;
    switch (disposition) {
    case CREATE_ALWAYS: return f | O_CREAT | O_TRUNC;
    case CREATE_NEW: return f | O_CREAT | O_EXCL;
    case OPEN_ALWAYS: return f | O_CREAT;
    case TRUNCATE_EXISTING: return f | O_TRUNC;
    default: return f;
    }
}

HANDLE CreateFileA(const char *path, DWORD access, DWORD share, void *sa, DWORD disposition, DWORD flags, HANDLE tmpl) {
    (void)share; (void)sa; (void)flags; (void)tmpl;
    int fd = open(path, open_flags(access, disposition) | O_CLOEXEC, 0644);
    if (fd < 0) { SetLastError(errno == ENOENT ? ERROR_FILE_NOT_FOUND : errno); return INVALID_HANDLE_VALUE; }
    FileHandle *h = calloc(1, sizeof *h);
    if (!h) { close(fd); return INVALID_HANDLE_VALUE; }
    h->kind = K_FILE;
    h->fd = fd;
    return h;
}

HANDLE CreateFileW(const WCHAR *path, DWORD access, DWORD share, void *sa, DWORD disposition, DWORD flags, HANDLE tmpl) {
    char p[4096];
    utf8_from_wide(path, p, sizeof p);
    return CreateFileA(p, access, share, sa, disposition, flags, tmpl);
}

// Positional I/O: the runtime reads RomFS and save files through OVERLAPPED offsets on synchronous handles, so the
// offset in the structure is the whole point of the call.
BOOL ReadFile(HANDLE h, void *buf, DWORD n, DWORD *got, void *ovp) {
    OVERLAPPED *ov = ovp;
    FileHandle *f = h;
    if (!f || f->kind != K_FILE) return FALSE;
    ssize_t r;
    if (ov) {
        off_t off = (off_t)(((uint64_t)ov->OffsetHigh << 32) | ov->Offset);
        r = pread(f->fd, buf, n, off);
    } else {
        r = read(f->fd, buf, n);
    }
    if (r < 0) { SetLastError(errno); return FALSE; }
    if (got) *got = (DWORD)r;
    // A positional read past the end reports ERROR_HANDLE_EOF (romfs.c and fs_host.c read_at test for it); a plain
    // sequential read at the end succeeds with 0 bytes, as Windows does — copy_file_w treats FALSE as a failure.
    if (r == 0 && n > 0 && ov) {
        SetLastError(ERROR_HANDLE_EOF);
        return FALSE;
    }
    return TRUE;
}

BOOL WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *written, void *ovp) {
    OVERLAPPED *ov = ovp;
    FileHandle *f = h;
    if (!f || f->kind != K_FILE) return FALSE;
    off_t off = ov ? (off_t)(((uint64_t)ov->OffsetHigh << 32) | ov->Offset) : -1;
    const uint8_t *p = buf;
    DWORD done = 0;
    while (done < n) {
        ssize_t w = off >= 0 ? pwrite(f->fd, p + done, n - done, off + done) : write(f->fd, p + done, n - done);
        if (w <= 0) { SetLastError(errno); return FALSE; }
        done += (DWORD)w;
    }
    if (written) *written = done;
    return TRUE;
}

BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER *size) {
    FileHandle *f = h;
    struct stat st;
    if (!f || f->kind != K_FILE || fstat(f->fd, &st)) return FALSE;
    size->QuadPart = st.st_size;
    return TRUE;
}

BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER dist, LARGE_INTEGER *out, DWORD method) {
    FileHandle *f = h;
    if (!f || f->kind != K_FILE) return FALSE;
    int w = method == FILE_BEGIN ? SEEK_SET : method == FILE_CURRENT ? SEEK_CUR : SEEK_END;
    off_t o = lseek(f->fd, (off_t)dist.QuadPart, w);
    if (o < 0) return FALSE;
    if (out) out->QuadPart = o;
    return TRUE;
}

BOOL FlushFileBuffers(HANDLE h) {
    FileHandle *f = h;
    return f && f->kind == K_FILE && fsync(f->fd) == 0;
}

// errno to the Win32 codes the runtime tests for after a failed lookup.
static void set_error_from_errno(int e) {
    SetLastError(e == ENOENT ? ERROR_FILE_NOT_FOUND : e == ENOTDIR ? ERROR_PATH_NOT_FOUND :
                 e == EACCES ? ERROR_ACCESS_DENIED : e);
}

static DWORD stat_attributes(const char *path) {
    struct stat st;
    if (stat(path, &st)) { set_error_from_errno(errno); return 0xFFFFFFFFu; }
    DWORD a = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    if (!(st.st_mode & S_IWUSR)) a |= FILE_ATTRIBUTE_READONLY;
    return a;
}

DWORD GetFileAttributesA(const char *path) { return stat_attributes(path); }

DWORD GetFileAttributesW(const WCHAR *path) {
    char p[4096];
    utf8_from_wide(path, p, sizeof p);
    return stat_attributes(p);
}

BOOL SetFileAttributesW(const WCHAR *path, DWORD attr) {
    char p[4096];
    utf8_from_wide(path, p, sizeof p);
    return chmod(p, (attr & FILE_ATTRIBUTE_READONLY) ? 0444 : 0644) == 0;
}

BOOL DeleteFileA(const char *path) { return unlink(path) == 0; }

BOOL DeleteFileW(const WCHAR *path) {
    char p[4096];
    utf8_from_wide(path, p, sizeof p);
    return DeleteFileA(p);
}

BOOL MoveFileExW(const WCHAR *from, const WCHAR *to, DWORD flags) {
    (void)flags;
    char a[4096], b[4096];
    utf8_from_wide(from, a, sizeof a);
    utf8_from_wide(to, b, sizeof b);
    return rename(a, b) == 0;
}

BOOL CreateDirectoryW(const WCHAR *path, void *sa) {
    char p[4096];
    utf8_from_wide(path, p, sizeof p);
    return CreateDirectoryA(p, sa);
}

BOOL RemoveDirectoryW(const WCHAR *path) {
    char p[4096];
    utf8_from_wide(path, p, sizeof p);
    return rmdir(p) == 0;
}

// FindFirstFileW/FindNextFileW: the runtime lists directories (romfs files, shader cache) by pattern "dir/*.ext".
typedef struct {
    int kind;
    DIR *dir;
    char pattern[512];
} FindHandle;

static int match_pattern(const char *name, const char *pat) {
    // only the two forms the runtime uses: "*" and "*.<ext>"
    if (!strcmp(pat, "*")) return 1;
    if (pat[0] == '*' && pat[1] == '.') {
        size_t n = strlen(name), e = strlen(pat + 1);
        return n > e && !strcmp(name + n - e, pat + 1);
    }
    return !strcmp(name, pat);
}

HANDLE FindFirstFileW(const WCHAR *pattern, WIN32_FIND_DATAW *data) {
    char p[4096];
    utf8_from_wide(pattern, p, sizeof p);
    char *slash = strrchr(p, '/');
    char dir_path[4096];
    const char *pat = p;
    if (slash) {
        size_t n = (size_t)(slash - p);
        if (n >= sizeof dir_path) return INVALID_HANDLE_VALUE;
        memcpy(dir_path, p, n);
        dir_path[n] = 0;
        pat = slash + 1;
    } else {
        snprintf(dir_path, sizeof dir_path, ".");
    }
    DIR *d = opendir(dir_path[0] ? dir_path : "/");
    if (!d) { SetLastError(ERROR_PATH_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    FindHandle *h = calloc(1, sizeof *h);
    if (!h) { closedir(d); return INVALID_HANDLE_VALUE; }
    h->kind = K_FIND;
    h->dir = d;
    snprintf(h->pattern, sizeof h->pattern, "%s", pat);
    if (!FindNextFileW(h, data)) { FindClose(h); SetLastError(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    return h;
}

BOOL FindNextFileW(HANDLE h, WIN32_FIND_DATAW *data) {
    FindHandle *f = h;
    if (!f || f->kind != K_FIND) return FALSE;
    struct dirent *e;
    while ((e = readdir(f->dir))) {
        if (!match_pattern(e->d_name, f->pattern)) continue;
        memset(data, 0, sizeof *data);
        wide_from_utf8(e->d_name, data->cFileName, MAX_PATH);
        data->dwFileAttributes = e->d_type == DT_DIR ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        return TRUE;
    }
    return FALSE;
}

BOOL FindClose(HANDLE h) {
    FindHandle *f = h;
    if (!f || f->kind != K_FIND) return FALSE;
    closedir(f->dir);
    free(f);
    return TRUE;
}

DWORD GetModuleFileNameA(HMODULE mod, char *out, DWORD cap) {
    (void)mod;
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n <= 0) return 0;
    out[n] = 0;
    return (DWORD)n;
}

DWORD GetModuleFileNameW(HMODULE mod, WCHAR *out, DWORD cap) {
    char p[4096];
    DWORD n = GetModuleFileNameA(mod, p, sizeof p);
    if (!n) return 0;
    wide_from_utf8(p, out, cap);
    return (DWORD)wcslen(out);
}

// ---- process memory

static BOOL proc_mem(HANDLE proc, PROCESS_MEMORY_COUNTERS *pmc, DWORD cb) {
    (void)proc;
    (void)cb;
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return FALSE;
    char buf[256];
    unsigned long long rss = 0, hwm = 0;
    while (fgets(buf, sizeof buf, f)) {
        unsigned long long kb;
        if (sscanf(buf, "VmRSS: %llu kB", &kb) == 1) rss = kb;
        else if (sscanf(buf, "VmHWM: %llu kB", &kb) == 1) hwm = kb;
    }
    fclose(f);
    pmc->PeakWorkingSetSize = (SIZE_T)hwm * 1024;
    pmc->WorkingSetSize = (SIZE_T)rss * 1024;
    return TRUE;
}

BOOL GetProcessMemoryInfo(HANDLE proc, PROCESS_MEMORY_COUNTERS *pmc, DWORD cb) { return proc_mem(proc, pmc, cb); }
BOOL K32GetProcessMemoryInfo(HANDLE proc, PROCESS_MEMORY_COUNTERS *pmc, DWORD cb) { return proc_mem(proc, pmc, cb); }

// ---- string helpers

int _wcsicmp(const WCHAR *a, const WCHAR *b) {
    for (;; a++, b++) {
        WCHAR x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return (int)x - (int)y;
        if (!x) return 0;
    }
}
int _stricmp(const char *a, const char *b) { return strcasecmp(a, b); }
int _strnicmp(const char *a, const char *b, size_t n) { return strncasecmp(a, b, n); }


#define WATCH_MAX 64
typedef struct Watch {
    uintptr_t base;
    size_t size;
    uint8_t *dirty;   // one byte per 4 KiB page (trap flavour only)
    int kernel_tracked; // 1: userfaultfd tracks writes, so a fault here is a mapping fault, not a watch write
    int sd_tracked;     // 1: soft-dirty bits track writes (fallback; the bits are process-wide)
} Watch;
static Watch *watch_for(uintptr_t addr);

// ---- write watch through userfaultfd
//
// The kernel's own dirty-bit tracking, the same mechanism Wine uses: a range is write-protected asynchronously
// (UFFD_FEATURE_WP_ASYNC) so writes never fault, and PAGEMAP_SCAN reads the pages marked written; scanning with
// PM_SCAN_WP_MATCHING re-arms them. No faults, no traps, and the guest stack and GPU-imported pools stay writable.
static int g_uffd = -1, g_pagemap = -1, g_uffd_watch;
static int g_force_trap;

// One event per first write of a page since its write protection: record the page in its watch and let the write go
// through by clearing the page's write-protect bit.
static void *uffd_thread(void *arg) {
    (void)arg;
    struct pollfd pfd = {.fd = g_uffd, .events = POLLIN};
    for (;;) {
        if (poll(&pfd, 1, -1) <= 0) continue;
        struct uffd_msg msg;
        if (read(g_uffd, &msg, sizeof msg) != (ssize_t)sizeof msg) continue;
        if (msg.event != UFFD_EVENT_PAGEFAULT) continue;
        uintptr_t addr = (uintptr_t)msg.arg.pagefault.address & ~(uintptr_t)4095;
        Watch *w = watch_for(addr);
        if (w) {
            size_t page = (addr - w->base) / 4096;
            if (page < w->size / 4096) __atomic_store_n(&w->dirty[page], 1, __ATOMIC_RELAXED);
        }
        struct uffdio_writeprotect wp = {.range = {addr, 4096}, .mode = 0};
        ioctl(g_uffd, UFFDIO_WRITEPROTECT, &wp);
    }
    return NULL;
}

// PM_SCAN_WP_MATCHING re-protects the pages but leaves PAGE_IS_WRITTEN set, so without this every page would be reported
// as written for ever and every texture re-uploaded. clear_refs(4) resets the soft-dirty bits; it walks the page tables,
// so it runs once per submission (mem_write_watch_epoch), not once per query.
static int g_clear_refs = -1;

// Off by default: clear_refs walks the whole address space, and on the BC-250 it took 483 ms per submission (two a
// frame, so nearly a second of CPU per frame) while the textures it was meant to settle are handled by TOTK_WATCH=self.
// TOTK_WATCH_EPOCH=1 turns it back on for measurement.
void mem_write_watch_epoch(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *v = getenv("TOTK_WATCH_EPOCH");
        enabled = v && *v == '1';
    }
    if (!enabled || !g_uffd_watch || g_clear_refs < 0) return;
    if (pwrite(g_clear_refs, "4", 1, 0) != 1) g_clear_refs = -1;
}

static int uffd_init(void) {
    if (g_uffd_watch) return 1;
    const char *mode = getenv("TOTK_WRITE_WATCH");
    if (mode && !strcmp(mode, "softdirty")) return 0;   // force the fallback, for testing it
    if (mode && !strcmp(mode, "trap")) { g_force_trap = 1; return 0; }   // force the mprotect flavour
    if (g_uffd < 0) {
        g_uffd = (int)syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
        if (g_uffd < 0) {
            rt_logf("watch: userfaultfd unavailable (seccomp?); write watch reports untracked pages\n");
            return 0;
        }
        // WP_ASYNC: writes never fault, which GPU-imported buffers need -- amdgpu pins those pages for DMA with
        // FOLL_WRITE, and a write-protected page makes it reject the submission (VK_ERROR_DEVICE_LOST). The dirty bits
        // therefore come from PAGEMAP_SCAN, and the epoch reset below clears them.
        struct uffdio_api api = {.api = UFFD_API, .features = UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_UNPOPULATED};
        if (ioctl(g_uffd, UFFDIO_API, &api)) { close(g_uffd); g_uffd = -1; return 0; }
        g_pagemap = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
        if (g_pagemap < 0) { close(g_uffd); g_uffd = -1; return 0; }
    }
    g_uffd_watch = 1;
    if (g_clear_refs < 0) g_clear_refs = open("/proc/self/clear_refs", O_WRONLY | O_CLOEXEC);
    rt_logf("watch: userfaultfd write watch active (WP_ASYNC, kernel dirty bits)\n");
    return 1;
}

static int uffd_watch(uintptr_t base, size_t n) {
    struct uffdio_register reg = {.range = {base, n}, .mode = UFFDIO_REGISTER_MODE_WP};
    if (ioctl(g_uffd, UFFDIO_REGISTER, &reg)) return 0;
    struct uffdio_writeprotect wp = {.range = {base, n}, .mode = UFFDIO_WRITEPROTECT_MODE_WP};
    if (ioctl(g_uffd, UFFDIO_WRITEPROTECT, &wp)) { ioctl(g_uffd, UFFDIO_UNREGISTER, &reg.range); return 0; }
    return 1;
}

static void uffd_unwatch(uintptr_t base, size_t n) {
    if (g_uffd_watch) {
        struct uffdio_range r = {base, n};
        ioctl(g_uffd, UFFDIO_UNREGISTER, &r);
    }
}

// Pages of [base, base+size) written since the last reset, as 4 KiB addresses; (size_t)-1 when the scan fails.
static size_t uffd_written(uintptr_t base, size_t size, uint64_t *pages, size_t max, int reset) {
    struct page_region rgns[256];
    uintptr_t addr = base, end = base + size;
    size_t n = 0;
    while (addr < end) {
        struct pm_scan_arg arg = {0};
        arg.size = sizeof arg;
        arg.start = addr;
        arg.end = end;
        arg.vec = (uintptr_t)rgns;
        arg.vec_len = sizeof rgns / sizeof *rgns;
        arg.category_mask = PAGE_IS_WRITTEN;
        arg.return_mask = PAGE_IS_WRITTEN;
        arg.max_pages = (n < max ? max - n : 0);
        if (reset) arg.flags |= PM_SCAN_WP_MATCHING;
        int r = ioctl(g_pagemap, PAGEMAP_SCAN, &arg);
        if (r < 0) return (size_t)-1;
        if (!r) break;
        for (int i = 0; i < r; i++)
            for (uintptr_t a = rgns[i].start; a < rgns[i].end && n < max; a += 4096) pages[n++] = a;
        if ((size_t)r < sizeof rgns / sizeof *rgns) break;
        if ((uintptr_t)arg.walk_end <= addr) break;
        addr = (uintptr_t)arg.walk_end;
    }
    return n;
}

// ---- memory: reservations, protection, and the write watch
//
// The watch is the mprotect trap flavour: watched pages are read-only, a write faults, the SIGSEGV handler records the
// page, makes it writable and returns. GetWriteWatch(RESET) reports the recorded pages and protects them again. That is
// the slow of the three implementations platform/linux.c is meant to offer (userfaultfd and soft-dirty come later);
// it is correct, which is what the first bring-up needs.


static Watch g_watch[WATCH_MAX];
static pthread_mutex_t g_watch_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_watch_installed;
static int g_watch_traps;   // 1 when pages are mprotect(RD) so writes fault (the trap flavour)

static Watch *watch_for(uintptr_t addr) {
    for (int i = 0; i < WATCH_MAX; i++)
        if (g_watch[i].dirty && addr >= g_watch[i].base && addr < g_watch[i].base + g_watch[i].size) return &g_watch[i];
    return NULL;
}

// Vectored exception handlers, in registration order. memory.c installs one to thaw a frozen 2 MiB region when the
// game writes to it; when it continues execution the write is real, so the page is still recorded as written below.
#define VEH_MAX 8
static struct { PVECTORED_EXCEPTION_HANDLER fn; } g_veh[VEH_MAX];
static int g_veh_count;

void *AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER fn) {
    (void)first;
    if (g_veh_count >= VEH_MAX) return NULL;
    g_veh[g_veh_count].fn = fn;
    return &g_veh[g_veh_count++];
}

ULONG RemoveVectoredExceptionHandler(void *handle) {
    (void)handle;
    return 1;
}

// The exception records the vectored handlers see: an access violation with [0] = 1 (write), [1] = the address.
static LONG veh_dispatch(siginfo_t *si, ucontext_t *uc) {
    if (!g_veh_count) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT ctx;
    memset(&ctx, 0, sizeof ctx);
    greg_t *g = uc->uc_mcontext.gregs;
    ctx.Rip = (DWORD64)g[REG_RIP];
    ctx.Rsp = (DWORD64)g[REG_RSP];
    EXCEPTION_RECORD er;
    memset(&er, 0, sizeof er);
    er.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    er.ExceptionAddress = si->si_addr;
    er.NumberParameters = 2;
    er.ExceptionInformation[0] = si->si_code == SEGV_ACCERR ? 1 : 0;
    er.ExceptionInformation[1] = (ULONG_PTR)si->si_addr;
    EXCEPTION_POINTERS ep = {&er, &ctx};
    for (int i = 0; i < g_veh_count; i++) {
        LONG r = g_veh[i].fn(&ep);
        if (r == EXCEPTION_CONTINUE_EXECUTION) return r;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void watch_fault(int sig, siginfo_t *si, void *uc_) {
    (void)sig;
    ucontext_t *uc = uc_;
    Watch *w = watch_for((uintptr_t)si->si_addr);
    if (!w) {
        // not a watched page: give the runtime's handlers a chance, then let the fault kill the process
        if (veh_dispatch(si, uc) == EXCEPTION_CONTINUE_EXECUTION) return;
        kern_guest_crash_report("unhandled access violation", (uint64_t)(uintptr_t)si->si_addr);
        signal(SIGSEGV, SIG_DFL);
        return;
    }
    if (w->kernel_tracked) {
        // userfaultfd records writes itself, so a fault inside a kernel-tracked range is a mapping fault (the alias
        // region is reserved PROT_NONE and mapped on demand): the runtime's handlers must see it.
        if (veh_dispatch(si, uc) == EXCEPTION_CONTINUE_EXECUTION) return;
        kern_guest_crash_report("unhandled mapping fault", (uint64_t)(uintptr_t)si->si_addr);
        signal(SIGSEGV, SIG_DFL);
        return;
    }
    // Trap flavour: record the write and make the page writable until the next GetWriteWatch(RESET) protects it again.
    // The vectored handlers are deliberately not consulted here: the runtime's crash reporter is one of them and would
    // exit the process on what is a normal write-watch fault.
    size_t page = ((uintptr_t)si->si_addr - w->base) / 4096;
    if (page < w->size / 4096) __atomic_store_n(&w->dirty[page], 1, __ATOMIC_RELAXED);
    mprotect((void *)(w->base + page * 4096), 4096, PROT_READ | PROT_WRITE);
}

static void watch_install(void) {
    if (g_watch_installed) return;
    g_watch_installed = 1;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = watch_fault;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_RESTART | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
}

static int prot_of(DWORD p) {
    switch (p & 0xff) {
    case PAGE_NOACCESS: return PROT_NONE;
    case PAGE_READONLY: return PROT_READ;
    case PAGE_READWRITE: return PROT_READ | PROT_WRITE;
    case PAGE_EXECUTE: return PROT_EXEC;
    case PAGE_EXECUTE_READ: return PROT_EXEC | PROT_READ;
    case PAGE_EXECUTE_READWRITE: return PROT_EXEC | PROT_READ | PROT_WRITE;
    default: return PROT_READ | PROT_WRITE;
    }
}

static size_t page_up(size_t n) {
    size_t p = (size_t)sysconf(_SC_PAGESIZE);
    return (n + p - 1) & ~(p - 1);
}

// A watched range starts read-only: the first write faults, the handler records the page and makes it writable until
// the next GetWriteWatch(RESET) protects it again.
static void watch_after_commit(void *addr, size_t n) {
    if (!watch_for((uintptr_t)addr)) return;
    mprotect(addr, n, PROT_READ);
}

// ---- a portable log10f
//
// Building on a glibc newer than the target ties the executable to that glibc: the newest images export log10f at a
// re-versioned symbol (GLIBC_2.43), which a Bazzite (2.39-2.41) or SteamOS (2.38) machine does not have. Defining it
// here keeps the import at the oldest version the double log10f carries. One correctly rounded-ish step through
// double precision is far more than the game needs from a log10f call in geometry and animation code.
float log10f(float x) { return (float)log10((double)x); }

// ---- write watch through soft dirty bits (fallback when userfaultfd is not available)
//
// /proc/self/clear_refs (4) clears the soft-dirty bit of every page; /proc/self/pagemap reports bit 55 for pages
// written since then. The bits are process-wide, so a scan must read every tracked range before the bits are cleared
// again, and a range too large to scan cheaply stays untracked (its callers then treat every page as written).
static int g_sd_clear = -1, g_sd_pagemap = -1, g_sd_on;
// The soft-dirty flavour is the only one whose reset works (clear_refs clears the bit; PM_SCAN_WP_MATCHING does not), so
// it has to cover the guest's own reservations: a 6 GiB app pool is one range, and the alias region is another.
static const size_t SD_MAX_RANGE = 12ull << 30;  // per range
static const size_t SD_MAX_TOTAL = 40ull << 30;  // all ranges together
static size_t g_sd_total;

static int sd_init(void) {
    if (g_sd_on) return 1;
    if (g_sd_clear < 0) {
        g_sd_clear = open("/proc/self/clear_refs", O_WRONLY | O_CLOEXEC);
        g_sd_pagemap = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
        if (g_sd_clear < 0 || g_sd_pagemap < 0) return 0;
    }
    g_sd_on = 1;
    rt_logf("watch: soft-dirty write watch active (clear_refs + pagemap; ranges over %zu MiB stay untracked)\n",
            SD_MAX_RANGE >> 20);
    return 1;
}

// Pages of [base, base+size) written since the last global clear, as 4 KiB addresses.
static size_t sd_written(uintptr_t base, size_t size, uint64_t *pages, size_t max) {
    enum { CHUNK = 128 * 1024 };   // pages per pread: 1 MiB of pagemap entries
    uint64_t buf[CHUNK];
    size_t n = 0;
    for (size_t done = 0; done < size / 4096 && n < max;) {
        size_t want = size / 4096 - done;
        if (want > CHUNK) want = CHUNK;
        ssize_t got = pread(g_sd_pagemap, buf, want * sizeof buf[0],
                            (off_t)((base / 4096 + done) * sizeof buf[0]));
        if (got <= 0) break;
        size_t entries = (size_t)got / sizeof buf[0];
        for (size_t i = 0; i < entries && n < max; i++)
            if (buf[i] >> 55 & 1) pages[n++] = base + (done + i) * 4096;
        done += entries;
        if (entries < want) break;
    }
    return n;
}

// Reads every soft-dirty range (the bits are about to be cleared) and answers for the requested one.
static size_t sd_collect(uintptr_t base, size_t size, uint64_t *pages, size_t max, int reset) {
    pthread_mutex_lock(&g_watch_lock);
    size_t n = 0;
    int asked = 0;
    uint64_t tmp;
    for (int i = 0; i < WATCH_MAX; i++) {
        Watch *w = &g_watch[i];
        if (!w->dirty || !w->sd_tracked) continue;
        int mine = (uintptr_t)base >= w->base && (uintptr_t)base + size <= w->base + w->size;
        asked |= mine;
        size_t k = mine ? sd_written((uintptr_t)base, size, pages, max)
                        : sd_written(w->base, w->size, &tmp, 1);
        if (mine) n = k;
    }
    if (reset && g_sd_on) {
        const char clear[] = "4";
        if (pwrite(g_sd_clear, clear, 1, 0) != 1) g_sd_on = 0;
    }
    pthread_mutex_unlock(&g_watch_lock);
    return asked ? n : (size_t)-1;
}

static void watch_register(void *base, size_t n) {
    int kernel = uffd_init() && uffd_watch((uintptr_t)base, n);
    int sd = 0;
    if (!kernel && !g_force_trap && n <= SD_MAX_RANGE && g_sd_total + n <= SD_MAX_TOTAL && sd_init()) {
        sd = 1;
        g_sd_total += n;
    }
    if (!kernel) watch_install();
    // The range is recorded either way: the fault path needs to know whether it owns the range (trap flavour) or
    // whether the kernel does (then a fault there is a mapping fault, and the runtime's handlers must see it).
    pthread_mutex_lock(&g_watch_lock);
    for (int i = 0; i < WATCH_MAX; i++) {
        if (g_watch[i].dirty) continue;
        uint8_t *bits = calloc(n / 4096 + 1, 1);
        if (!bits) break;
        g_watch[i].base = (uintptr_t)base;
        g_watch[i].size = n;
        g_watch[i].dirty = bits;
        g_watch[i].kernel_tracked = kernel;
        g_watch[i].sd_tracked = sd;
        break;
    }
    pthread_mutex_unlock(&g_watch_lock);
}

void *VirtualAlloc(void *addr, SIZE_T size, DWORD type, DWORD prot) {
    size_t n = page_up(size ? size : 1);
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    int p = PROT_NONE;
    if (type & MEM_COMMIT) p = prot_of(prot);
    // Committing inside a reservation: the pages are already mapped PROT_NONE and anonymous, so mprotect is the
    // whole operation (Windows' MEM_COMMIT has exactly that meaning for a reserved range).
    if (addr && (type & MEM_COMMIT) && !(type & MEM_RESERVE) && mprotect(addr, n, p) == 0) return addr;
    if (addr) flags |= MAP_FIXED_NOREPLACE;
    void *m = mmap(addr, n, p, flags, -1, 0);
    if (m == MAP_FAILED) { SetLastError(errno == EEXIST ? ERROR_ALREADY_EXISTS : errno); return NULL; }
    if ((type & MEM_WRITE_WATCH) && !getenv("TOTK_NO_WRITE_WATCH")) watch_register(m, n);   // reservations too: the pages are committed inside it later
    return m;
}

BOOL VirtualFree(void *addr, SIZE_T size, DWORD type) {
    if (type & MEM_RELEASE) {
        uffd_unwatch((uintptr_t)addr, page_up(size));
        pthread_mutex_lock(&g_watch_lock);
        for (int i = 0; i < WATCH_MAX; i++) {
            if (!g_watch[i].dirty || g_watch[i].base != (uintptr_t)addr) continue;
            free(g_watch[i].dirty);
            g_watch[i].dirty = NULL;
            g_watch[i].size = 0;
        }
        pthread_mutex_unlock(&g_watch_lock);
        return munmap(addr, page_up(size)) == 0;
    }
    if (type & MEM_DECOMMIT) {
        // Decommit means the content is gone: a later commit must read zeros, as Windows guarantees. MADV_DONTNEED
        // discards the anonymous pages, so the next touch faults in fresh zero pages.
        size_t n = page_up(size);
        madvise(addr, n, MADV_DONTNEED);
        return mprotect(addr, n, PROT_NONE) == 0;
    }
    return FALSE;
}

BOOL VirtualProtect(void *addr, SIZE_T size, DWORD prot, DWORD *old) {
    if (old) *old = PAGE_READWRITE;
    return mprotect(addr, page_up(size), prot_of(prot)) == 0;
}

BOOL VirtualLock(void *addr, SIZE_T size) { (void)addr; (void)size; return TRUE; }
BOOL VirtualUnlock(void *addr, SIZE_T size) { (void)addr; (void)size; return TRUE; }

SIZE_T VirtualQuery(const void *addr, MEMORY_BASIC_INFORMATION *mbi, SIZE_T len) {
    (void)len;
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char buf[512];
    uintptr_t want = (uintptr_t)addr;
    SIZE_T got = 0;
    while (fgets(buf, sizeof buf, f)) {
        unsigned long long lo, hi;
        char perms[8];
        if (sscanf(buf, "%llx-%llx %7s", &lo, &hi, perms) != 3) continue;
        if (want < lo || want >= hi) continue;
        memset(mbi, 0, sizeof *mbi);
        mbi->BaseAddress = (PVOID)(uintptr_t)lo;
        mbi->AllocationBase = (PVOID)(uintptr_t)lo;
        mbi->RegionSize = (SIZE_T)(hi - lo);
        // A PROT_NONE anonymous mapping is how a reservation looks here, and MEM_RESERVE is what VirtualQuery must
        // report for it: the runtime checks its fixed windows that way.
        int accessed = perms[0] == 'r' || perms[1] == 'w' || perms[2] == 'x';
        mbi->Protect = perms[1] == 'w' ? PAGE_READWRITE : perms[0] == 'r' ? PAGE_READONLY
                       : perms[2] == 'x' ? PAGE_EXECUTE : PAGE_NOACCESS;
        mbi->AllocationProtect = mbi->Protect;
        mbi->State = accessed ? MEM_COMMIT : MEM_RESERVE;
        mbi->Type = MEM_PRIVATE;
        got = sizeof *mbi;
        break;
    }
    fclose(f);
    return got;
}

UINT GetWriteWatch(DWORD flags, void *base, SIZE_T size, void **pages, ULONG_PTR *count, DWORD *granularity) {
    if (!count || !granularity || !pages) return 1;
    if (!g_uffd_watch && g_sd_on) {
        size_t n = sd_collect((uintptr_t)base, size, (uint64_t *)(void *)pages, *count,
                              (flags & WRITE_WATCH_FLAG_RESET) != 0);
        if (n == (size_t)-1) return 1;
        *count = n;
        *granularity = 4096;
        return 0;
    }
    if (!g_uffd_watch && g_sd_on) {
        size_t n = sd_collect((uintptr_t)base, size, (uint64_t *)(void *)pages, *count,
                              (flags & WRITE_WATCH_FLAG_RESET) != 0);
        if (n == (size_t)-1) return 1;
        *count = n;
        *granularity = 4096;
        return 0;
    }
    if (g_uffd_watch) {
        if (!watch_for((uintptr_t)base)) return 1;
        size_t n = uffd_written((uintptr_t)base, size, (uint64_t *)(void *)pages, *count,
                                (flags & WRITE_WATCH_FLAG_RESET) != 0);
        if (n == (size_t)-1) return 1;
        *count = n;
        *granularity = 4096;
        return 0;
    }
    // Without a dirty-bit source the honest answer is "not tracked": callers then treat every page as written, which
    // is slow but never stale. See docs/linux.md, M4.
    if (!g_watch_traps) return 1;
    Watch *w = watch_for((uintptr_t)base);
    if (!w || !count || !granularity || !pages) return 1;
    size_t first = ((uintptr_t)base - w->base) / 4096;
    size_t npages = size / 4096;
    if (first + npages > w->size / 4096) npages = w->size / 4096 - first;
    ULONG_PTR max = *count, n = 0;
    for (size_t i = 0; i < npages && n < max; i++) {
        if (!__atomic_load_n(&w->dirty[first + i], __ATOMIC_RELAXED)) continue;
        pages[n++] = (void *)(w->base + (first + i) * 4096);
        if (flags & WRITE_WATCH_FLAG_RESET) {
            __atomic_store_n(&w->dirty[first + i], 0, __ATOMIC_RELAXED);
            mprotect((void *)(w->base + (first + i) * 4096), 4096, PROT_READ);
        }
    }
    *count = n;
    *granularity = 4096;
    return 0;
}

UINT ResetWriteWatch(void *base, SIZE_T size) {
    Watch *w = watch_for((uintptr_t)base);
    if (!w) return 1;
    size_t first = ((uintptr_t)base - w->base) / 4096;
    size_t npages = size / 4096;
    for (size_t i = 0; i < npages && first + i < w->size / 4096; i++) {
        __atomic_store_n(&w->dirty[first + i], 0, __ATOMIC_RELAXED);
        mprotect((void *)(w->base + (first + i) * 4096), 4096, PROT_READ);
    }
    return 0;
}

BOOL FlushInstructionCache(HANDLE proc, const void *addr, SIZE_T size) {
    (void)proc;
    __builtin___clear_cache((char *)(uintptr_t)addr, (char *)(uintptr_t)addr + size);
    return TRUE;
}

// ---- environment and code-page conversion

BOOL SetEnvironmentVariableA(const char *name, const char *value) {
    if (!value) { unsetenv(name); return TRUE; }
    return setenv(name, value, 1) == 0;
}

DWORD GetEnvironmentVariableA(const char *name, char *out, DWORD cap) {
    const char *v = getenv(name);
    if (!v) return 0;
    size_t n = strlen(v);
    if (n >= cap) return (DWORD)n + 1;
    memcpy(out, v, n + 1);
    return (DWORD)n;
}

int MultiByteToWideChar(UINT cp, DWORD flags, const char *in, int in_len, WCHAR *out, int out_cap) {
    (void)cp; (void)flags;
    if (in_len < 0) in_len = (int)strlen(in) + 1;
    if (!out) return in_len;
    size_t n = wide_from_utf8(in, out, (size_t)out_cap);
    return (int)n;
}

int WideCharToMultiByte(UINT cp, DWORD flags, const WCHAR *in, int in_len, char *out, int out_cap, const char *def,
                        BOOL *used) {
    (void)cp; (void)flags; (void)def;
    if (used) *used = FALSE;
    if (in_len < 0) in_len = (int)wcslen(in) + 1;
    if (!out) return in_len * 4;
    int n = utf8_from_wide(in, out, (size_t)out_cap);
    return n;
}

// ---- modules and processes

HMODULE GetModuleHandleW(const WCHAR *name) {
    if (!name) {
        // The executable's own load address: the kernel uses it to tell lifted code from host code in a thread's RIP.
        Dl_info info;
        if (dladdr((void *)(uintptr_t)&GetModuleHandleW, &info) && info.dli_fbase) return (HMODULE)info.dli_fbase;
        return (HMODULE)(uintptr_t)0x400000;
    }
    return NULL;
}

HMODULE LoadLibraryW(const WCHAR *name) {
    char p[4096];
    utf8_from_wide(name, p, sizeof p);
    return dlopen(p, RTLD_NOW | RTLD_LOCAL);
}

FARPROC GetProcAddress(HMODULE mod, const char *name) { return (FARPROC)dlsym(mod, name); }

typedef struct {
    int kind;
    pid_t pid;
    int status;
} ProcessHandle;

static int split_args(char *line, char **argv, int max) {
    int n = 0;
    char *p = line;
    while (*p && n < max - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (*p == '"') {
            p++;
            argv[n++] = p;
            while (*p && *p != '"') p++;
        } else {
            argv[n++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
        }
        if (*p) *p++ = 0;
    }
    argv[n] = NULL;
    return n;
}

BOOL CreateProcessW(const WCHAR *app, WCHAR *cmd, void *pa, void *ta, BOOL inherit, DWORD flags, void *env,
                    const WCHAR *dir, void *si, void *pi) {
    (void)pa; (void)ta; (void)inherit; (void)flags; (void)env; (void)si;
    char line[8192];
    if (cmd) utf8_from_wide(cmd, line, sizeof line);
    else if (app) { utf8_from_wide(app, line, sizeof line); }
    else return FALSE;
    char *argv[64];
    if (!split_args(line, argv, 64) || !argv[0]) return FALSE;
    pid_t pid = fork();
    if (pid < 0) return FALSE;
    if (pid == 0) {
        if (dir) {
            char d[4096];
            utf8_from_wide(dir, d, sizeof d);
            if (chdir(d)) _exit(127);
        }
        if (!inherit) {
            int null = open("/dev/null", O_RDWR);
            if (null >= 0) { dup2(null, 0); dup2(null, 1); dup2(null, 2); }
        }
        execv(argv[0], argv);
        _exit(127);
    }
    ProcessHandle *h = calloc(1, sizeof *h);
    if (!h) { kill(pid, SIGKILL); return FALSE; }
    h->kind = K_PROCESS;
    h->pid = pid;
    PROCESS_INFORMATION *p = pi;
    p->hProcess = (HANDLE)h;
    p->hThread = NULL;
    p->dwProcessId = (DWORD)pid;
    p->dwThreadId = 0;
    return TRUE;
}

BOOL GetExitCodeProcess(HANDLE h, DWORD *code) {
    ProcessHandle *p = h;
    if (!p || !code) return FALSE;
    int st = 0;
    pid_t r = waitpid(p->pid, &st, WNOHANG);
    if (r == 0) { *code = STILL_ACTIVE; return TRUE; }
    *code = WIFEXITED(st) ? (DWORD)WEXITSTATUS(st) : 1;
    return TRUE;
}

void ExitProcess(UINT code) { exit((int)code); }

// ---- toolhelp: the process's own threads

typedef struct {
    int kind;
    DIR *dir;
} SnapHandle;

HANDLE CreateToolhelp32Snapshot(DWORD flags, DWORD pid) {
    (void)flags;
    (void)pid;
    SnapHandle *s = calloc(1, sizeof *s);
    if (!s) return INVALID_HANDLE_VALUE;
    s->kind = K_SNAP;
    s->dir = opendir("/proc/self/task");
    if (!s->dir) { free(s); return INVALID_HANDLE_VALUE; }
    return s;
}

BOOL Thread32First(HANDLE snap, THREADENTRY32 *te) {
    SnapHandle *s = snap;
    if (!s || s->kind != K_SNAP) return FALSE;
    rewinddir(s->dir);
    return Thread32Next(snap, te);
}

BOOL Thread32Next(HANDLE snap, THREADENTRY32 *te) {
    SnapHandle *s = snap;
    struct dirent *e;
    if (!s || s->kind != K_SNAP) return FALSE;
    while ((e = readdir(s->dir))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        memset(te, 0, sizeof *te);
        te->dwSize = sizeof *te;
        te->th32ThreadID = (DWORD)strtoul(e->d_name, NULL, 10);
        te->th32OwnerProcessID = (DWORD)getpid();
        return TRUE;
    }
    return FALSE;
}

// ---- handles

BOOL CloseHandle(HANDLE h) {
    if (!h) return FALSE;
    int kind = *(int *)h;
    switch (kind) {
    case K_FILE: {
        FileHandle *f = h;
        close(f->fd);
        free(f);
        return TRUE;
    }
    case K_FIND: return FindClose(h);
    case K_SNAP: {
        SnapHandle *s = h;
        if (s->dir) closedir(s->dir);
        free(s);
        return TRUE;
    }
    case K_TIMER: {
        struct Timer { int kind; pthread_mutex_t m; pthread_cond_t c; uint64_t due; int armed; };
        struct Timer *t = h;
        pthread_mutex_destroy(&t->m);
        pthread_cond_destroy(&t->c);
        free(t);
        return TRUE;
    }
    case K_EVENT: {
        EventHandle *e = h;
        pthread_mutex_destroy(&e->m);
        pthread_cond_destroy(&e->c);
        free(e);
        return TRUE;
    }
    case K_MAP: {
        MapHandle *m = h;
        close(m->fd);
        free(m);
        return TRUE;
    }
    case K_THREAD: return TRUE;   // see the note above CreateThread
    default: return FALSE;
    }
}

static DWORD wait_for_single(HANDLE h, int kind, DWORD ms);

DWORD WaitForSingleObject(HANDLE h, DWORD ms) {
    if (!h) return WAIT_FAILED;
    int kind = *(int *)h;
    uint64_t w0 = kern_now_ns();
    DWORD wr = wait_for_single(h, kind, ms);
    waitstat_add(kind == K_THREAD ? "wait thread" : kind == K_PROCESS ? "wait process" : kind == K_EVENT ? "wait event"
                 : kind == K_TIMER ? "wait timer" : "wait object",
                 kern_now_ns() - w0);
    return wr;
}

static DWORD wait_for_single(HANDLE h, int kind, DWORD ms) {
    if (kind == K_THREAD) {
        ThreadHandle *t = h;
        if (ms == INFINITE) { pthread_join(t->thread, NULL); return WAIT_OBJECT_0; }
        // ms == 0 is a poll and has to probe once: a counted loop `for (waited = 0; waited < ms)` never runs for it and
        // reported every finished thread as still running, which hung WaitForMultipleObjects(..., 0) polling.
        uint64_t deadline = mono_ns() + (uint64_t)ms * 1000000ull;
        for (;;) {
            if (syscall(SYS_tgkill, getpid(), t->tid, 0) != 0) return WAIT_OBJECT_0;
            if (mono_ns() >= deadline) return WAIT_TIMEOUT;
            Sleep(1);
        }
    }
    if (kind == K_PROCESS) {
        ProcessHandle *p = h;
        if (ms == INFINITE) { waitpid(p->pid, NULL, 0); return WAIT_OBJECT_0; }
        uint64_t deadline = mono_ns() + (uint64_t)ms * 1000000ull;
        for (;;) {
            int st;
            if (waitpid(p->pid, &st, WNOHANG) == p->pid) return WAIT_OBJECT_0;
            if (mono_ns() >= deadline) return WAIT_TIMEOUT;
            Sleep(1);
        }
    }
    if (kind == K_EVENT) {
        EventHandle *e = h;
        pthread_mutex_lock(&e->m);
        if (ms == INFINITE) {
            while (!e->signaled) pthread_cond_wait(&e->c, &e->m);
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            ts.tv_sec += ms / 1000;
            ts.tv_nsec += (long)(ms % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            while (!e->signaled) {
                if (pthread_cond_timedwait(&e->c, &e->m, &ts) == ETIMEDOUT) {
                    pthread_mutex_unlock(&e->m);
                    return WAIT_TIMEOUT;
                }
            }
        }
        if (!e->manual) e->signaled = 0;
        pthread_mutex_unlock(&e->m);
        return WAIT_OBJECT_0;
    }
    if (kind == K_TIMER) {
        struct Timer { int kind; pthread_mutex_t m; pthread_cond_t c; uint64_t due; int armed; };
        struct Timer *t = h;
        pthread_mutex_lock(&t->m);
        while (t->armed) {
            uint64_t now = mono_ns();
            if (now >= t->due) { t->armed = 0; break; }
            struct timespec ts = {(time_t)(t->due / 1000000000ull), (long)(t->due % 1000000000ull)};
            pthread_cond_timedwait(&t->c, &t->m, &ts);
        }
        pthread_mutex_unlock(&t->m);
        return WAIT_OBJECT_0;
    }
    return WAIT_FAILED;
}

// ---- printf-like wide formatting (the runtime's menu and log helpers)

int _vsnwprintf(WCHAR *out, size_t cap, const WCHAR *fmt, va_list ap) {
    char f[512], buf[4096];
    size_t i = 0;
    for (; fmt[i] && i < sizeof f - 1; i++) f[i] = (char)fmt[i];
    f[i] = 0;
    int n = vsnprintf(buf, sizeof buf, f, ap);
    wide_from_utf8(buf, out, cap);
    return n;
}

int _snwprintf(WCHAR *out, size_t cap, const WCHAR *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnwprintf(out, cap, fmt, ap);
    va_end(ap);
    return n;
}

int swprintf_s(WCHAR *out, size_t cap, const WCHAR *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnwprintf(out, cap, fmt, ap);
    va_end(ap);
    return n;
}

void *GlobalAlloc(UINT flags, SIZE_T bytes) { (void)flags; return malloc(bytes); }
void *GlobalFree(void *ptr) { free(ptr); return NULL; }
BOOL Beep(DWORD freq, DWORD ms) { (void)freq; (void)ms; return FALSE; }

DWORD GetFullPathNameW(const WCHAR *name, DWORD cap, WCHAR *out, WCHAR **file) {
    char in[4096], abs[4096];
    utf8_from_wide(name, in, sizeof in);
    if (in[0] == '/') snprintf(abs, sizeof abs, "%s", in);
    else {
        char cwd[2048];
        if (!getcwd(cwd, sizeof cwd)) return 0;
        snprintf(abs, sizeof abs, "%s/%s", cwd, in);
    }
    wide_from_utf8(abs, out, cap);
    if (file) *file = wcsrchr(out, '/');
    return (DWORD)wcslen(out);
}

// ---- SHA-256 (bcrypt.h). The runtime hashes the RomFS index at start; the RomFS checks are otherwise independent.

typedef struct {
    DWORD state[8];
    uint64_t bytes;
    BYTE block[64];
    size_t used;
} Sha256;

static const DWORD sha_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(Sha256 *s, const BYTE *p) {
    DWORD w[64];
    for (int i = 0; i < 16; i++) w[i] = (DWORD)p[i * 4] << 24 | (DWORD)p[i * 4 + 1] << 16 | (DWORD)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        DWORD a = w[i - 15], b = w[i - 2];
        w[i] = w[i - 16] + w[i - 7] + (ROR(a, 7) ^ ROR(a, 18) ^ (a >> 3)) + (ROR(b, 17) ^ ROR(b, 19) ^ (b >> 10));
    }
    DWORD a = s->state[0], b = s->state[1], c = s->state[2], d = s->state[3];
    DWORD e = s->state[4], f = s->state[5], g = s->state[6], h = s->state[7];
    for (int i = 0; i < 64; i++) {
        DWORD t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + sha_k[i] + w[i];
        DWORD t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->state[0] += a; s->state[1] += b; s->state[2] += c; s->state[3] += d;
    s->state[4] += e; s->state[5] += f; s->state[6] += g; s->state[7] += h;
}

static void sha_init(Sha256 *s) {
    static const DWORD iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->state, iv, sizeof iv);
    s->bytes = 0;
    s->used = 0;
}

static void sha_update(Sha256 *s, const BYTE *p, size_t n) {
    s->bytes += n;
    while (n) {
        size_t take = 64 - s->used;
        if (take > n) take = n;
        memcpy(s->block + s->used, p, take);
        s->used += take;
        p += take;
        n -= take;
        if (s->used == 64) { sha_block(s, s->block); s->used = 0; }
    }
}

static void sha_final(Sha256 *s, BYTE out[32]) {
    uint64_t bits = s->bytes * 8;
    BYTE pad = 0x80;
    sha_update(s, &pad, 1);
    BYTE zero = 0;
    while (s->used != 56) sha_update(s, &zero, 1);
    BYTE len[8];
    for (int i = 0; i < 8; i++) len[i] = (BYTE)(bits >> (56 - i * 8));
    sha_update(s, len, 8);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (BYTE)(s->state[i] >> 24);
        out[i * 4 + 1] = (BYTE)(s->state[i] >> 16);
        out[i * 4 + 2] = (BYTE)(s->state[i] >> 8);
        out[i * 4 + 3] = (BYTE)s->state[i];
    }
}

NTSTATUS BCryptOpenAlgorithmProvider(BCRYPT_ALG_HANDLE *alg, const char *id, const char *impl, DWORD flags) {
    (void)id; (void)impl; (void)flags;
    if (alg) *alg = calloc(1, 1);
    return alg && *alg ? 0 : (NTSTATUS)0xC0000017;
}
NTSTATUS BCryptCloseAlgorithmProvider(BCRYPT_ALG_HANDLE alg, DWORD flags) { (void)flags; free(alg); return 0; }
NTSTATUS BCryptCreateHash(BCRYPT_ALG_HANDLE alg, BCRYPT_HASH_HANDLE *hash, PBYTE obj, DWORD obj_len, PBYTE secret,
                          DWORD secret_len, DWORD flags) {
    (void)alg; (void)obj; (void)obj_len; (void)secret; (void)secret_len; (void)flags;
    Sha256 *s = calloc(1, sizeof *s);
    if (!s) return (NTSTATUS)0xC0000017;
    sha_init(s);
    *hash = s;
    return 0;
}
NTSTATUS BCryptHashData(BCRYPT_HASH_HANDLE hash, PBYTE data, DWORD n, DWORD flags) {
    (void)flags;
    sha_update(hash, data, n);
    return 0;
}
NTSTATUS BCryptFinishHash(BCRYPT_HASH_HANDLE hash, PBYTE out, DWORD n, DWORD flags) {
    (void)flags;
    BYTE digest[32];
    sha_final(hash, digest);
    memcpy(out, digest, n < 32 ? n : 32);
    return 0;
}
NTSTATUS BCryptDestroyHash(BCRYPT_HASH_HANDLE hash) { free(hash); return 0; }

// ---- one-time initialisation, pipes, environment

BOOL InitOnceExecuteOnce(PINIT_ONCE once, BOOL (*fn)(PINIT_ONCE, void *, void **), void *param, void **ctx) {
    pthread_mutex_lock(&once->m);
    if (!once->done) {
        once->done = fn(once, param, ctx) ? 1 : 2;
    }
    int ok = once->done == 1;
    pthread_mutex_unlock(&once->m);
    return ok;
}

BOOL CreatePipe(HANDLE *read_end, HANDLE *write_end, void *sa, DWORD size) {
    (void)sa;
    (void)size;
    struct Pipe { int kind; int fd; };
    int fds[2];
    if (pipe(fds)) return FALSE;
    struct Pipe *r = calloc(1, sizeof *r), *w = calloc(1, sizeof *w);
    if (!r || !w) { close(fds[0]); close(fds[1]); free(r); free(w); return FALSE; }
    r->kind = K_FILE;
    r->fd = fds[0];
    w->kind = K_FILE;
    w->fd = fds[1];
    *read_end = r;
    *write_end = w;
    return TRUE;
}

int _putenv_s(const char *name, const char *value) { return setenv(name, value, 1); }

// ---- MSVC CRT helpers

char *_strdup(const char *s) { return strdup(s); }
void *_aligned_malloc(size_t size, size_t align) {
    void *p = NULL;
    if (posix_memalign(&p, align < sizeof(void *) ? sizeof(void *) : align, size)) return NULL;
    return p;
}
void *_aligned_realloc(void *p, size_t size, size_t align) { (void)align; return realloc(p, size); }
void _aligned_free(void *p) { free(p); }
BOOL SetHandleInformation(HANDLE h, DWORD mask, DWORD flags) { (void)h; (void)mask; (void)flags; return TRUE; }
BOOL GetHandleInformation(HANDLE h, DWORD *flags) { (void)h; if (flags) *flags = 0; return TRUE; }

BOOL InitOnceBeginInitialize(PINIT_ONCE once, DWORD flags, BOOL *pending, void **ctx) {
    (void)flags;
    pthread_mutex_lock(&once->m);
    if (once->done) {
        if (pending) *pending = FALSE;
        if (ctx) *ctx = NULL;
    } else if (pending) {
        *pending = TRUE;
    }
    pthread_mutex_unlock(&once->m);
    return TRUE;
}

BOOL InitOnceComplete(PINIT_ONCE once, DWORD flags, void *ctx) {
    (void)flags;
    (void)ctx;
    pthread_mutex_lock(&once->m);
    once->done = 1;
    pthread_mutex_unlock(&once->m);
    return TRUE;
}

int GetUserDefaultLocaleName(WCHAR *out, int cap) {
    const char *lang = getenv("LC_ALL");
    if (!lang || !*lang) lang = getenv("LANG");
    if (!lang || !*lang) lang = "en_US";
    return (int)wide_from_utf8(lang, out, (size_t)cap);
}

NTSTATUS BCryptGenRandom(BCRYPT_ALG_HANDLE alg, PBYTE buf, DWORD n, DWORD flags) {
    (void)alg;
    (void)flags;
    static int fd = -1;
    if (fd < 0) fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return (NTSTATUS)0xC00000BB;
    size_t done = 0;
    while (done < n) {
        ssize_t r = read(fd, buf + done, n - done);
        if (r <= 0) return (NTSTATUS)0xC00000BB;
        done += (size_t)r;
    }
    return 0;
}

// ---- events, directories, temporary paths, narrow find, module handles

HANDLE CreateEventW(void *attr, BOOL manual_reset, BOOL initial, const WCHAR *name) {
    (void)attr;
    (void)name;
    EventHandle *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->kind = K_EVENT;
    e->manual = manual_reset;
    e->signaled = initial != 0;
    pthread_mutex_init(&e->m, NULL);
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(&e->c, &a);
    pthread_condattr_destroy(&a);
    return e;
}

BOOL SetEvent(HANDLE h) {
    EventHandle *e = h;
    if (!e || e->kind != K_EVENT) return FALSE;
    pthread_mutex_lock(&e->m);
    e->signaled = 1;
    if (e->manual) pthread_cond_broadcast(&e->c);
    else pthread_cond_signal(&e->c);
    pthread_mutex_unlock(&e->m);
    return TRUE;
}

BOOL ResetEvent(HANDLE h) {
    EventHandle *e = h;
    if (!e || e->kind != K_EVENT) return FALSE;
    pthread_mutex_lock(&e->m);
    e->signaled = 0;
    pthread_mutex_unlock(&e->m);
    return TRUE;
}

BOOL CreateDirectoryA(const char *path, void *sa) {
    (void)sa;
    if (mkdir(path, 0755) == 0) return TRUE;
    // Windows reports an existing directory as a failure with ERROR_ALREADY_EXISTS, and the runtime checks that.
    switch (errno) {
    case EEXIST: SetLastError(ERROR_ALREADY_EXISTS); break;
    case ENOENT: SetLastError(ERROR_PATH_NOT_FOUND); break;
    case EACCES: SetLastError(ERROR_ACCESS_DENIED); break;
    default: SetLastError((DWORD)errno); break;
    }
    return FALSE;
}

DWORD GetFullPathNameA(const char *name, DWORD cap, char *out, char **file) {
    char abs[4096];
    if (name[0] == '/') snprintf(abs, sizeof abs, "%s", name);
    else {
        char cwd[2048];
        if (!getcwd(cwd, sizeof cwd)) return 0;
        snprintf(abs, sizeof abs, "%s/%s", cwd, name);
    }
    snprintf(out, cap, "%s", abs);
    if (file) *file = strrchr(out, '/');
    return (DWORD)strlen(out);
}

DWORD GetTempPathA(DWORD cap, char *out) {
    const char *t = getenv("TMPDIR");
    if (!t || !*t) t = "/tmp";
    snprintf(out, cap, "%s/", t);
    return (DWORD)strlen(out);
}

HANDLE FindFirstFileA(const char *pattern, WIN32_FIND_DATAA *data) {
    WCHAR w[4096];
    WIN32_FIND_DATAW wd;
    wide_from_utf8(pattern, w, sizeof w / sizeof *w);
    HANDLE h = FindFirstFileW(w, &wd);
    if (h == INVALID_HANDLE_VALUE) return h;
    memset(data, 0, sizeof *data);
    data->dwFileAttributes = wd.dwFileAttributes;
    for (int i = 0; i < MAX_PATH && wd.cFileName[i]; i++) data->cFileName[i] = (WCHAR)wd.cFileName[i];
    return h;
}

HANDLE FindFirstFileExA(const char *pattern, int level, WIN32_FIND_DATAA *data, int search, void *filter, DWORD flags) {
    (void)level;
    (void)search;
    (void)filter;
    (void)flags;
    return FindFirstFileA(pattern, data);
}

BOOL GetModuleHandleExW(DWORD flags, const WCHAR *name, HMODULE *out) {
    (void)flags;
    (void)name;
    if (out) *out = (HMODULE)(uintptr_t)0x400000;
    return TRUE;
}

int _commit(int fd) { return fsync(fd); }
LONGLONG InterlockedOr64(LONGLONG volatile *p, LONGLONG v) { return __atomic_or_fetch(p, v, __ATOMIC_SEQ_CST); }
LONGLONG InterlockedAnd64(LONGLONG volatile *p, LONGLONG v) { return __atomic_and_fetch(p, v, __ATOMIC_SEQ_CST); }
LONG InterlockedExchangeAdd(LONG volatile *p, LONG v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }

// ---- the rest of the surface the runtime touches

int _fileno(FILE *f) { return fileno(f); }
int _fseeki64(FILE *f, int64_t off, int whence) { return fseeko(f, (off_t)off, whence); }
void _tzset(void) { tzset(); }

char *strtok_s(char *s, const char *delim, char **ctx) { return strtok_r(s, delim, ctx); }

WCHAR *_wcsdup(const WCHAR *s) {
    size_t n = wcslen(s) + 1;
    WCHAR *d = malloc(n * sizeof *d);
    if (d) memcpy(d, s, n * sizeof *d);
    return d;
}

HANDLE GetStdHandle(DWORD which) {
    static FileHandle out = {K_FILE, 1}, err = {K_FILE, 2}, in = {K_FILE, 0};
    if (which == STD_OUTPUT_HANDLE) return &out;
    if (which == STD_ERROR_HANDLE) return &err;
    return &in;
}

BOOL GetProcessTimes(HANDLE proc, FILETIME *create, FILETIME *exit, FILETIME *kernel, FILETIME *user) {
    (void)proc;
    (void)create;
    (void)exit;
    FILE *f = fopen("/proc/self/stat", "r");
    if (!f) return FALSE;
    char buf[1024];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    char *p = strrchr(buf, ')');
    if (!p) return FALSE;
    p++;
    unsigned long long ut = 0, st = 0;
    for (int field = 3; field <= 15 && *p; field++) {
        while (*p == ' ') p++;
        if (field == 14) ut = strtoull(p, &p, 10);
        else if (field == 15) { st = strtoull(p, &p, 10); break; }
        else while (*p && *p != ' ') p++;
    }
    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) hz = 100;
    uint64_t us = (uint64_t)ut * 1000000 / (uint64_t)hz, ss = (uint64_t)st * 1000000 / (uint64_t)hz;
    if (kernel) { kernel->dwLowDateTime = (DWORD)us; kernel->dwHighDateTime = (DWORD)(us >> 32); }
    if (user) { user->dwLowDateTime = (DWORD)ss; user->dwHighDateTime = (DWORD)(ss >> 32); }
    return TRUE;
}

DWORD GetTempFileNameA(const char *dir, const char *prefix, UINT unique, char *out) {
    for (UINT i = 0; i < 1000; i++) {
        snprintf(out, MAX_PATH, "%s/%s%04x%04x.tmp", dir, prefix ? prefix : "tmp", unique & 0xffff, i);
        int fd = open(out, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
        if (fd >= 0) { close(fd); return unique + i; }
    }
    return 0;
}

static void to_systemtime(time_t t, SYSTEMTIME *st) {
    struct tm tm;
    localtime_r(&t, &tm);
    st->wYear = (WORD)(tm.tm_year + 1900);
    st->wMonth = (WORD)(tm.tm_mon + 1);
    st->wDayOfWeek = (WORD)tm.tm_wday;
    st->wDay = (WORD)tm.tm_mday;
    st->wHour = (WORD)tm.tm_hour;
    st->wMinute = (WORD)tm.tm_min;
    st->wSecond = (WORD)tm.tm_sec;
    st->wMilliseconds = 0;
}

void GetLocalTime(SYSTEMTIME *st) { to_systemtime(time(NULL), st); }

void GetSystemTime(SYSTEMTIME *st) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    st->wYear = (WORD)(tm.tm_year + 1900);
    st->wMonth = (WORD)(tm.tm_mon + 1);
    st->wDayOfWeek = (WORD)tm.tm_wday;
    st->wDay = (WORD)tm.tm_mday;
    st->wHour = (WORD)tm.tm_hour;
    st->wMinute = (WORD)tm.tm_min;
    st->wSecond = (WORD)tm.tm_sec;
    st->wMilliseconds = (WORD)(ts.tv_nsec / 1000000);
}

HANDLE CreateEventA(void *attr, BOOL manual_reset, BOOL initial, const char *name) {
    (void)name;
    return CreateEventW(attr, manual_reset, initial, NULL);
}

BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA *data) {
    WIN32_FIND_DATAW wd;
    if (!FindNextFileW(h, &wd)) return FALSE;
    memset(data, 0, sizeof *data);
    data->dwFileAttributes = wd.dwFileAttributes;
    for (int i = 0; i < MAX_PATH && wd.cFileName[i]; i++) data->cFileName[i] = (WCHAR)wd.cFileName[i];
    return TRUE;
}

int64_t _ftelli64(FILE *f) { return (int64_t)ftello(f); }
BOOL RemoveDirectoryA(const char *path) { return rmdir(path) == 0; }
HWND GetForegroundWindow(void) { return NULL; }

// The command line as one UTF-32 string, built once from /proc/self/cmdline (NUL separated).
WCHAR *GetCommandLineW(void) {
    static WCHAR *line;
    if (line) return line;
    FILE *f = fopen("/proc/self/cmdline", "rb");
    char buf[8192];
    size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    if (f) fclose(f);
    for (size_t i = 0; i < n; i++)
        if (!buf[i]) buf[i] = ' ';
    buf[n] = 0;
    WCHAR w[8192];
    wide_from_utf8(buf, w, 8192);
    line = _wcsdup(w);
    return line;
}

void GetSystemTimeAsFileTime(FILETIME *ft) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t t = ((uint64_t)ts.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)ts.tv_nsec / 100;
    ft->dwLowDateTime = (DWORD)t;
    ft->dwHighDateTime = (DWORD)(t >> 32);
}

int localtime_s(struct tm *out, const time_t *t) {
    if (!out || !t) return 22;
    return localtime_r(t, out) ? 0 : 22;
}

short GetAsyncKeyState(int vk) { (void)vk; return 0; }
void YieldProcessor(void) { __builtin_ia32_pause(); }
int _get_timezone(long *seconds) { if (seconds) *seconds = 0; return 0; }

// Waits for the first (or all) of the handles; the runtime uses it for the shader translator's job list.
DWORD WaitForMultipleObjects(DWORD count, const HANDLE *handles, BOOL wait_all, DWORD ms) {
    uint64_t deadline = ms == INFINITE ? 0 : mono_ns() + (uint64_t)ms * 1000000;
    for (;;) {
        DWORD ready = 0;
        for (DWORD i = 0; i < count; i++) {
            if (WaitForSingleObject(handles[i], 0) == WAIT_OBJECT_0) ready++;
            else if (wait_all) break;
            else return WAIT_OBJECT_0 + i;
        }
        if (wait_all && ready == count) return WAIT_OBJECT_0;
        if (deadline && mono_ns() >= deadline) return WAIT_TIMEOUT;
        Sleep(1);
    }
}

BOOL CreateHardLinkW(const WCHAR *to, const WCHAR *from, void *sa) {
    (void)sa;
    char a[4096], b[4096];
    utf8_from_wide(to, a, sizeof a);
    utf8_from_wide(from, b, sizeof b);
    return link(b, a) == 0;
}

BOOL GetThreadTimes(HANDLE thread, FILETIME *create, FILETIME *exit, FILETIME *kernel, FILETIME *user) {
    (void)thread;
    (void)create;
    (void)exit;
    (void)kernel;
    if (!user) return TRUE;
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts)) return FALSE;
    uint64_t us = (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000;
    user->dwLowDateTime = (DWORD)us;
    user->dwHighDateTime = (DWORD)(us >> 32);
    return TRUE;
}

BOOL GetModuleInformation(HANDLE proc, HMODULE mod, MODULEINFO *info, DWORD cb) {
    (void)proc;
    (void)cb;
    if (!info) return FALSE;
    memset(info, 0, sizeof *info);
    info->lpBaseOfDll = (LPVOID)(uintptr_t)(mod ? (uintptr_t)mod : 0x400000);
    info->SizeOfImage = 0x10000000;   // the guest modules live above this; the check only bounds a diagnostic
    return TRUE;
}

// ---- fibres
//
// One ucontext per fibre. The thread's own context is a fibre too, so every switch is a plain swapcontext and the
// caller's C frames survive untouched. MXCSR is not part of a ucontext, which is why kernel.c syncs the FP control
// word itself around a switch.

typedef struct FiberCtx {
    ucontext_t ctx;
    void *stack;
    size_t stack_size;
    void (*fn)(void *);
    void *arg;
    struct FiberCtx *return_to;
    int done;
} FiberCtx;

static __thread FiberCtx *tls_fiber;

static void fiber_entry(void) {
    FiberCtx *f = tls_fiber;
    f->fn(f->arg);
    f->done = 1;
    tls_fiber = f->return_to;
    setcontext(&f->return_to->ctx);
}

static FiberCtx *fiber_new(void (*fn)(void *), void *arg, size_t reserve) {
    FiberCtx *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    if (reserve < 0x10000) reserve = 0x10000;
    void *stack = mmap(NULL, reserve, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (stack == MAP_FAILED) { free(f); return NULL; }
    f->stack = stack;
    f->stack_size = reserve;
    f->fn = fn;
    f->arg = arg;
    getcontext(&f->ctx);
    f->ctx.uc_stack.ss_sp = stack;
    f->ctx.uc_stack.ss_size = reserve;
    f->ctx.uc_link = NULL;
    makecontext(&f->ctx, fiber_entry, 0);
    return f;
}

void *ConvertThreadToFiber(void *param) { return ConvertThreadToFiberEx(param, 0); }

void *ConvertThreadToFiberEx(void *param, DWORD flags) {
    (void)param;
    (void)flags;
    if (tls_fiber) return tls_fiber;
    FiberCtx *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    tls_fiber = f;
    return f;
}

BOOL ConvertFiberToThread(void) {
    if (tls_fiber && tls_fiber->stack) return FALSE;   // only the thread's own context converts
    free(tls_fiber);
    tls_fiber = NULL;
    return TRUE;
}

void *CreateFiber(SIZE_T size, void (*fn)(void *), void *arg) {
    return CreateFiberEx(size, size, 0, fn, arg);
}

void *CreateFiberEx(SIZE_T commit, SIZE_T reserve, DWORD flags, void (*fn)(void *), void *arg) {
    (void)commit;
    (void)flags;
    return fiber_new(fn, arg, (size_t)reserve);
}

void SwitchToFiber(void *fiber) {
    FiberCtx *to = fiber;
    FiberCtx *from = tls_fiber;
    if (!to || to == from) return;
    to->return_to = from;
    tls_fiber = to;
    swapcontext(&from->ctx, &to->ctx);
}

void DeleteFiber(void *fiber) {
    FiberCtx *f = fiber;
    if (!f) return;
    if (f->stack) munmap(f->stack, f->stack_size);
    free(f);
}

void *GetCurrentFiber(void) { return tls_fiber; }
BOOL IsThreadAFiber(void) { return tls_fiber != NULL; }

BOOL SetFileInformationByHandle(HANDLE h, FILE_INFO_BY_HANDLE_CLASS cls, void *info, DWORD size) {
    (void)size;
    FileHandle *f = h;
    if (!f || f->kind != K_FILE) return FALSE;
    if (cls == FileEndOfFileInfo) {
        FILE_END_OF_FILE_INFO *eof = info;
        return ftruncate(f->fd, (off_t)eof->EndOfFile.QuadPart) == 0;
    }
    if (cls == FileDispositionInfo) {
        int del = *(int *)info;
        return del ? 1 : 1;   // deletion by handle is not used by the runtime; report success and let CloseHandle keep it
    }
    return FALSE;
}

BOOL GetFileInformationByHandle(HANDLE h, BY_HANDLE_FILE_INFORMATION *info) {
    FileHandle *f = h;
    struct stat st;
    if (!f || f->kind != K_FILE || fstat(f->fd, &st)) return FALSE;
    memset(info, 0, sizeof *info);
    info->dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    info->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32);
    info->nFileSizeLow = (DWORD)st.st_size;
    info->nNumberOfLinks = (DWORD)st.st_nlink;
    info->nFileIndexHigh = (DWORD)((uint64_t)st.st_ino >> 32);
    info->nFileIndexLow = (DWORD)st.st_ino;
    info->dwVolumeSerialNumber = (DWORD)st.st_dev;
    return TRUE;
}

DWORD GetTempPathW(DWORD cap, WCHAR *out) {
    char p[4096];
    DWORD n = GetTempPathA(sizeof p, p);
    wide_from_utf8(p, out, cap);
    return (DWORD)wcslen(out);
}

DWORD GetCurrentDirectoryA(DWORD cap, char *out) {
    if (!getcwd(out, cap)) return 0;
    return (DWORD)strlen(out);
}

DWORD GetCurrentDirectoryW(DWORD cap, WCHAR *out) {
    char p[4096];
    if (!GetCurrentDirectoryA(sizeof p, p)) return 0;
    wide_from_utf8(p, out, cap);
    return (DWORD)wcslen(out);
}

BOOL SetCurrentDirectoryW(const WCHAR *path) {
    char p[4096];
    utf8_from_wide(path, p, sizeof p);
    return chdir(p) == 0;
}

BOOL GetDiskFreeSpaceExW(const WCHAR *path, uint64_t *avail, uint64_t *total, uint64_t *free_bytes) {
    char p[4096];
    utf8_from_wide(path, p, sizeof p);
    struct statvfs vfs;
    if (statvfs(p, &vfs)) return FALSE;
    uint64_t unit = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
    if (total) *total = (uint64_t)vfs.f_blocks * unit;
    if (free_bytes) *free_bytes = (uint64_t)vfs.f_bfree * unit;
    if (avail) *avail = (uint64_t)vfs.f_bavail * unit;
    return TRUE;
}

DWORD GetFileType(HANDLE h) {
    FileHandle *f = h;
    if (!f || f->kind != K_FILE) return FILE_TYPE_UNKNOWN;
    struct stat st;
    if (fstat(f->fd, &st)) return FILE_TYPE_UNKNOWN;
    if (S_ISREG(st.st_mode)) return FILE_TYPE_DISK;
    if (S_ISCHR(st.st_mode)) return FILE_TYPE_CHAR;
    if (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode)) return FILE_TYPE_PIPE;
    return FILE_TYPE_UNKNOWN;
}

FILE *_wfopen(const WCHAR *path, const WCHAR *mode) {
    char p[4096], m[16];
    utf8_from_wide(path, p, sizeof p);
    utf8_from_wide(mode, m, sizeof m);
    return fopen(p, m);
}

// The Linux file layer is synchronous, so an OVERLAPPED operation has already completed when it returns.
BOOL GetOverlappedResult(HANDLE h, OVERLAPPED *ov, DWORD *got, BOOL wait) {
    (void)h;
    (void)ov;
    (void)wait;
    if (got) *got = 0;
    return TRUE;
}

// ---- file mappings (the RomFS image is mapped, not read)

static struct { void *base; size_t size; } g_views[64];
static pthread_mutex_t g_view_lock = PTHREAD_MUTEX_INITIALIZER;

HANDLE CreateFileMappingA(HANDLE file, void *sa, DWORD protect, DWORD high, DWORD low, const char *name) {
    (void)sa;
    (void)protect;
    (void)name;
    FileHandle *f = file;
    if (!f || f->kind != K_FILE) return NULL;
    struct stat st;
    if (fstat(f->fd, &st)) return NULL;
    MapHandle *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->kind = K_MAP;
    m->fd = dup(f->fd);
    m->size = ((uint64_t)high << 32) | low;
    if (!m->size) m->size = (uint64_t)st.st_size;
    return m;
}

HANDLE CreateFileMappingW(HANDLE file, void *sa, DWORD protect, DWORD high, DWORD low, const WCHAR *name) {
    (void)name;
    return CreateFileMappingA(file, sa, protect, high, low, NULL);
}

void *MapViewOfFile(HANDLE map, DWORD access, DWORD high, DWORD low, SIZE_T bytes) {
    (void)access;
    MapHandle *m = map;
    if (!m || m->kind != K_MAP) return NULL;
    uint64_t off = ((uint64_t)high << 32) | low;
    size_t len = bytes ? bytes : (size_t)(m->size - off);
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, m->fd, (off_t)off);
    if (p == MAP_FAILED) return NULL;
    pthread_mutex_lock(&g_view_lock);
    for (int i = 0; i < 64; i++) {
        if (g_views[i].base) continue;
        g_views[i].base = p;
        g_views[i].size = len;
        break;
    }
    pthread_mutex_unlock(&g_view_lock);
    return p;
}

BOOL UnmapViewOfFile(const void *base) {
    pthread_mutex_lock(&g_view_lock);
    for (int i = 0; i < 64; i++) {
        if (g_views[i].base != base) continue;
        munmap(g_views[i].base, g_views[i].size);
        g_views[i].base = NULL;
        g_views[i].size = 0;
        pthread_mutex_unlock(&g_view_lock);
        return TRUE;
    }
    pthread_mutex_unlock(&g_view_lock);
    return FALSE;
}
