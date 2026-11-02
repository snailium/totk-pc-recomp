// A small Win32 surface for the Linux build, so the runtime keeps one source tree.
//
// The runtime was written against the Windows API; `platform.h` only covers the parts that were already factored out
// (threads, time, locks, files, machine load). The rest — SRW locks, interlocked counters, VirtualAlloc/VirtualProtect,
// GetWriteWatch, SuspendThread/SetThreadContext, CreateFileW and friends — is used directly by ~60 files.
//
// On Linux those names are resolved here instead of in a forked copy of every file. This is a bring-up bridge, not a
// second platform layer: it is only compiled for Linux, it forwards to POSIX (pthreads, mmap, mprotect, open/read/write,
// signals) and it is the piece a later change removes file by file as `tools/check_platform.py` shrinks its list.
#pragma once

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

// ---- types

typedef int BOOL;
typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef int32_t LONG;
typedef uint32_t ULONG;
typedef int64_t LONGLONG;
typedef uint64_t ULONGLONG;
typedef uint64_t DWORD64;
typedef int64_t LONG64;
typedef void *HINSTANCE;
typedef uint64_t ULONG64;
typedef uintptr_t ULONG_PTR;
typedef intptr_t LONG_PTR;
typedef size_t SIZE_T;
typedef void *PVOID;
typedef void *LPVOID;
typedef const void *LPCVOID;
typedef void *HANDLE;
typedef void *HMODULE;
typedef wchar_t WCHAR;
typedef WCHAR *LPWSTR;
typedef const WCHAR *LPCWSTR;
typedef char *LPSTR;
typedef const char *LPCSTR;
typedef DWORD *LPDWORD;
typedef LONG *LPLONG;
typedef BYTE *PBYTE;
typedef BYTE *PUCHAR;
typedef unsigned int UINT;
typedef int32_t HRESULT;
typedef int32_t NTSTATUS;
typedef void (*FARPROC)(void);
#define WINAPI
#define CALLBACK
#define WINAPIV
typedef void *HWND;
typedef void *HINSTANCE;
#define HANDLE_FLAG_INHERIT 0x00000001

#define TRUE 1
#define FALSE 0
#define MAX_PATH 260
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INFINITE 0xFFFFFFFFu
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 258
#define WAIT_FAILED 0xFFFFFFFFu
#define STILL_ACTIVE 259

// ---- handles and errors

DWORD GetLastError(void);
void SetLastError(DWORD err);
#define ERROR_SUCCESS 0
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_PATH_NOT_FOUND 3
#define ERROR_ACCESS_DENIED 5
#define ERROR_INVALID_HANDLE 6
#define ERROR_NOT_ENOUGH_MEMORY 8
#define ERROR_INVALID_PARAMETER 87
#define ERROR_ALREADY_EXISTS 183
#define ERROR_MORE_DATA 234
#define ERROR_NO_MORE_FILES 18
#define ERROR_TIMEOUT 1460
#define ERROR_IO_PENDING 997

// ---- locks and condition variables (zero-initialised is valid, like the Windows ones)

typedef struct { void *p; } SRWLOCK;
typedef SRWLOCK *PSRWLOCK;
#define SRWLOCK_INIT {0}
typedef struct { void *p; } CONDITION_VARIABLE;
#define CONDITION_VARIABLE_INIT {0}
typedef CONDITION_VARIABLE *PCONDITION_VARIABLE;
typedef struct { void *p; } CRITICAL_SECTION;
typedef CRITICAL_SECTION *LPCRITICAL_SECTION;
typedef struct { pthread_mutex_t m; int done; } INIT_ONCE;
typedef INIT_ONCE *PINIT_ONCE;
#define INIT_ONCE_STATIC_INIT {PTHREAD_MUTEX_INITIALIZER, 0}
typedef uint32_t DWORD_PTR;

void InitializeSRWLock(PSRWLOCK l);
void AcquireSRWLockExclusive(PSRWLOCK l);
void ReleaseSRWLockExclusive(PSRWLOCK l);
void AcquireSRWLockShared(PSRWLOCK l);
void ReleaseSRWLockShared(PSRWLOCK l);
BOOL TryAcquireSRWLockExclusive(PSRWLOCK l);
BOOL TryAcquireSRWLockShared(PSRWLOCK l);

void InitializeConditionVariable(PCONDITION_VARIABLE c);
BOOL SleepConditionVariableSRW(PCONDITION_VARIABLE c, PSRWLOCK l, DWORD ms, ULONG flags);
void WakeConditionVariable(PCONDITION_VARIABLE c);
void WakeAllConditionVariable(PCONDITION_VARIABLE c);

void InitializeCriticalSection(LPCRITICAL_SECTION c);
void EnterCriticalSection(LPCRITICAL_SECTION c);
void LeaveCriticalSection(LPCRITICAL_SECTION c);
BOOL TryEnterCriticalSection(LPCRITICAL_SECTION c);
void DeleteCriticalSection(LPCRITICAL_SECTION c);

// ---- interlocked

LONG InterlockedIncrement(LONG volatile *p);
LONG InterlockedDecrement(LONG volatile *p);
LONG InterlockedExchange(LONG volatile *p, LONG v);
LONG InterlockedCompareExchange(LONG volatile *p, LONG v, LONG cmp);
LONG InterlockedAdd(LONG volatile *p, LONG v);
LONGLONG InterlockedIncrement64(LONGLONG volatile *p);
LONGLONG InterlockedDecrement64(LONGLONG volatile *p);
LONGLONG InterlockedExchange64(LONGLONG volatile *p, LONGLONG v);
LONGLONG InterlockedCompareExchange64(LONGLONG volatile *p, LONGLONG v, LONGLONG cmp);
LONGLONG InterlockedAdd64(LONGLONG volatile *p, LONGLONG v);
void *InterlockedExchangePointer(void *volatile *p, void *v);
void *InterlockedCompareExchangePointer(void *volatile *p, void *v, void *cmp);

// ---- threads

typedef void *(*PTHREAD_START_ROUTINE)(void *);
#define THREAD_PRIORITY_IDLE -15
#define THREAD_PRIORITY_LOWEST -2
#define THREAD_PRIORITY_BELOW_NORMAL -1
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_HIGHEST 2
#define THREAD_PRIORITY_TIME_CRITICAL 15
#define CREATE_SUSPENDED 0x00000004
#define THREAD_SUSPEND_RESUME 0x0002
#define THREAD_GET_CONTEXT 0x0008
#define THREAD_SET_CONTEXT 0x0010
#define THREAD_QUERY_INFORMATION 0x0040

HANDLE CreateThread(void *attr, SIZE_T stack, PTHREAD_START_ROUTINE fn, void *arg, DWORD flags, DWORD *id);
BOOL CloseHandle(HANDLE h);
HANDLE GetCurrentThread(void);
DWORD GetCurrentThreadId(void);
DWORD GetCurrentProcessId(void);
HANDLE GetCurrentProcess(void);
DWORD SuspendThread(HANDLE h);
DWORD ResumeThread(HANDLE h);
BOOL SetThreadPriority(HANDLE h, int prio);
int GetThreadPriority(HANDLE h);
BOOL SetThreadAffinityMask(HANDLE h, DWORD_PTR mask);
void SwitchToThread(void);
DWORD GetActiveProcessorCount(WORD group);
#define ALL_PROCESSOR_GROUPS 0xFFFF
BOOL SetThreadDescription(HANDLE h, const WCHAR *name);
HANDLE OpenThread(DWORD access, BOOL inherit, DWORD tid);
DWORD WaitForSingleObject(HANDLE h, DWORD ms);
BOOL GetExitCodeThread(HANDLE h, DWORD *code);
void GetSystemInfo(void *info);

// The x86-64 register set the runtime reads and rewrites (kernel.c changes Rip to enter kern_ap_stub).
typedef struct {
    DWORD ContextFlags;
    DWORD64 Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi, R8, R9, R10, R11, R12, R13, R14, R15;
    DWORD64 Rip;
    DWORD EFlags;
} CONTEXT;
#define CONTEXT_CONTROL 0x00100001
#define CONTEXT_INTEGER 0x00100002
BOOL GetThreadContext(HANDLE h, CONTEXT *ctx);
BOOL SetThreadContext(HANDLE h, const CONTEXT *ctx);

typedef struct {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
} FILETIME;

typedef struct {
    union {
        struct { DWORD LowPart; LONG HighPart; };
        LONGLONG QuadPart;
    };
} LARGE_INTEGER;
typedef LARGE_INTEGER *PLARGE_INTEGER;

typedef struct {
    DWORD dwNumberOfProcessors;
    DWORD dwPageSize;
    DWORD dwProcessorType;
    DWORD dwAllocationGranularity;
    DWORD dwActiveProcessorMask;
    WORD wProcessorArchitecture;
} SYSTEM_INFO;

// ---- time

void QueryPerformanceCounter(LARGE_INTEGER *t);
void QueryPerformanceFrequency(LARGE_INTEGER *f);
DWORD GetTickCount(void);
ULONGLONG GetTickCount64(void);
void Sleep(DWORD ms);
void timeBeginPeriod(UINT ms);
void timeEndPeriod(UINT ms);

typedef void *HANDLE_TIMER;
HANDLE CreateWaitableTimerExW(void *attr, const WCHAR *name, DWORD flags, DWORD access);
BOOL SetWaitableTimer(HANDLE t, const LARGE_INTEGER *due, LONG period, void *routine, void *arg, BOOL resume);

// ---- files

typedef struct {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
    DWORD dwReserved0, dwReserved1;
    WCHAR cFileName[MAX_PATH];
    WCHAR cAlternateFileName[14];
} WIN32_FIND_DATAW;
typedef WIN32_FIND_DATAW *LPWIN32_FIND_DATAW;

#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 0x00000001u
#define FILE_SHARE_WRITE 0x00000002u
#define FILE_SHARE_DELETE 0x00000004u
#define OPEN_EXISTING 3
#define CREATE_ALWAYS 2
#define CREATE_NEW 1
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5
#define FILE_ATTRIBUTE_NORMAL 0x80
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_READONLY 0x01
#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu
#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2
#define MOVEFILE_REPLACE_EXISTING 0x1
#define MOVEFILE_WRITE_THROUGH 0x8
#define FIND_FIRST_EX_LARGE_FETCH 0x2

HANDLE CreateFileW(const WCHAR *path, DWORD access, DWORD share, void *sa, DWORD disposition, DWORD flags, HANDLE tmpl);
HANDLE CreateFileA(const char *path, DWORD access, DWORD share, void *sa, DWORD disposition, DWORD flags, HANDLE tmpl);
BOOL ReadFile(HANDLE h, void *buf, DWORD n, DWORD *read, void *ov);
BOOL WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *written, void *ov);
BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER *size);
BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER dist, LARGE_INTEGER *out, DWORD method);
BOOL FlushFileBuffers(HANDLE h);
DWORD GetFileAttributesA(const char *path);
DWORD GetFileAttributesW(const WCHAR *path);
BOOL SetFileAttributesW(const WCHAR *path, DWORD attr);
BOOL DeleteFileW(const WCHAR *path);
BOOL DeleteFileA(const char *path);
BOOL MoveFileExW(const WCHAR *from, const WCHAR *to, DWORD flags);
BOOL CreateDirectoryW(const WCHAR *path, void *sa);
BOOL RemoveDirectoryW(const WCHAR *path);
HANDLE FindFirstFileW(const WCHAR *pattern, WIN32_FIND_DATAW *data);
BOOL FindNextFileW(HANDLE h, WIN32_FIND_DATAW *data);
BOOL FindClose(HANDLE h);
DWORD GetModuleFileNameA(HMODULE mod, char *out, DWORD cap);
DWORD GetModuleFileNameW(HMODULE mod, WCHAR *out, DWORD cap);
BOOL CreateProcessW(const WCHAR *app, WCHAR *cmd, void *pa, void *ta, BOOL inherit, DWORD flags, void *env,
                    const WCHAR *dir, void *si, void *pi);
BOOL GetExitCodeProcess(HANDLE p, DWORD *code);
void ExitProcess(UINT code);
HMODULE GetModuleHandleW(const WCHAR *name);
HMODULE LoadLibraryW(const WCHAR *name);
FARPROC GetProcAddress(HMODULE mod, const char *name);

typedef struct {
    DWORD cb;
    LPVOID lpReserved, lpDesktop, lpTitle;
    DWORD dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
    WORD wShowWindow, cbReserved2;
    BYTE *lpReserved2;
    HANDLE hStdInput, hStdOutput, hStdError;
} STARTUPINFOW;
typedef struct {
    HANDLE hProcess, hThread;
    DWORD dwProcessId, dwThreadId;
} PROCESS_INFORMATION;

typedef struct {
    DWORD nLength;
    LPVOID lpSecurityDescriptor;
    BOOL bInheritHandle;
} SECURITY_ATTRIBUTES;

int MultiByteToWideChar(UINT cp, DWORD flags, const char *in, int in_len, WCHAR *out, int out_cap);
int WideCharToMultiByte(UINT cp, DWORD flags, const WCHAR *in, int in_len, char *out, int out_cap, const char *def,
                        BOOL *used);
#define CP_UTF8 65001
#define CP_ACP 0
BOOL SetEnvironmentVariableA(const char *name, const char *value);
DWORD GetEnvironmentVariableA(const char *name, char *out, DWORD cap);

// ---- memory

#define MEM_COMMIT 0x00001000u
#define MEM_RESERVE 0x00002000u
#define MEM_DECOMMIT 0x00004000u
#define MEM_RELEASE 0x00008000u
#define MEM_RESET 0x00080000u
#define MEM_WRITE_WATCH 0x00200000u
#define MEM_TOP_DOWN 0x00100000u
#define MEM_LARGE_PAGES 0x20000000u
#define MEM_PHYSICAL 0x00400000u
#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define PAGE_WRITECOPY 0x08
#define PAGE_EXECUTE 0x10
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_EXECUTE_WRITECOPY 0x80
#define PAGE_GUARD 0x100
#define PAGE_NOCACHE 0x200
#define WRITE_WATCH_FLAG_RESET 0x01
#define MEM_PRIVATE 0x20000
#define MEM_MAPPED 0x40000
#define MEM_IMAGE 0x1000000

typedef struct {
    PVOID BaseAddress;
    PVOID AllocationBase;
    DWORD AllocationProtect;
    SIZE_T RegionSize;
    DWORD State;
    DWORD Protect;
    DWORD Type;
} MEMORY_BASIC_INFORMATION;
typedef MEMORY_BASIC_INFORMATION *PMEMORY_BASIC_INFORMATION;

void *VirtualAlloc(void *addr, SIZE_T size, DWORD type, DWORD protect);
BOOL VirtualFree(void *addr, SIZE_T size, DWORD type);
BOOL VirtualProtect(void *addr, SIZE_T size, DWORD prot, DWORD *old);
SIZE_T VirtualQuery(const void *addr, MEMORY_BASIC_INFORMATION *mbi, SIZE_T len);
BOOL VirtualLock(void *addr, SIZE_T size);
BOOL VirtualUnlock(void *addr, SIZE_T size);
UINT GetWriteWatch(DWORD flags, void *base, SIZE_T size, void **pages, ULONG_PTR *count, DWORD *granularity);
UINT ResetWriteWatch(void *base, SIZE_T size);
BOOL FlushInstructionCache(HANDLE proc, const void *addr, SIZE_T size);

// ---- process memory (psapi.h)

typedef struct {
    DWORD cb;
    DWORD PageFaultCount;
    SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage;
} PROCESS_MEMORY_COUNTERS;
typedef struct {
    DWORD cb;
    DWORD PageFaultCount;
    SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage;
    SIZE_T PrivateUsage;
} PROCESS_MEMORY_COUNTERS_EX;
BOOL GetProcessMemoryInfo(HANDLE proc, PROCESS_MEMORY_COUNTERS *pmc, DWORD cb);
BOOL K32GetProcessMemoryInfo(HANDLE proc, PROCESS_MEMORY_COUNTERS *pmc, DWORD cb);

// ---- SHA-256 (bcrypt.h) — the runtime hashes the RomFS index at start

typedef void *BCRYPT_ALG_HANDLE;
typedef void *BCRYPT_HASH_HANDLE;
#define BCRYPT_SHA256_ALGORITHM "SHA256"
NTSTATUS BCryptOpenAlgorithmProvider(BCRYPT_ALG_HANDLE *alg, const char *id, const char *impl, DWORD flags);
NTSTATUS BCryptCloseAlgorithmProvider(BCRYPT_ALG_HANDLE alg, DWORD flags);
NTSTATUS BCryptCreateHash(BCRYPT_ALG_HANDLE alg, BCRYPT_HASH_HANDLE *hash, PBYTE obj, DWORD obj_len, PBYTE secret,
                          DWORD secret_len, DWORD flags);
NTSTATUS BCryptHashData(BCRYPT_HASH_HANDLE hash, PBYTE data, DWORD n, DWORD flags);
NTSTATUS BCryptFinishHash(BCRYPT_HASH_HANDLE hash, PBYTE out, DWORD n, DWORD flags);
NTSTATUS BCryptDestroyHash(BCRYPT_HASH_HANDLE hash);
NTSTATUS BCryptGenRandom(BCRYPT_ALG_HANDLE alg, PBYTE buf, DWORD n, DWORD flags);

// ---- toolhelp (tlhelp32.h)

typedef struct {
    DWORD dwSize;
    DWORD cntUsage;
    DWORD th32ThreadID;
    DWORD th32OwnerProcessID;
    LONG tpBasePri;
    LONG tpDeltaPri;
    DWORD dwFlags;
} THREADENTRY32;
typedef THREADENTRY32 *LPTHREADENTRY32;
#define TH32CS_SNAPTHREAD 0x00000004
HANDLE CreateToolhelp32Snapshot(DWORD flags, DWORD pid);
BOOL Thread32First(HANDLE snap, THREADENTRY32 *te);
BOOL Thread32Next(HANDLE snap, THREADENTRY32 *te);

// ---- MSVC CRT helpers

char *_strdup(const char *s);
void *_aligned_malloc(size_t size, size_t align);
void *_aligned_realloc(void *p, size_t size, size_t align);
void _aligned_free(void *p);
BOOL SetHandleInformation(HANDLE h, DWORD mask, DWORD flags);
BOOL GetHandleInformation(HANDLE h, DWORD *flags);
BOOL InitOnceBeginInitialize(PINIT_ONCE once, DWORD flags, BOOL *pending, void **ctx);
BOOL InitOnceComplete(PINIT_ONCE once, DWORD flags, void *ctx);
int GetUserDefaultLocaleName(WCHAR *out, int cap);
typedef struct { DWORD dwFileAttributes; WCHAR cFileName[MAX_PATH]; } WIN32_FIND_DATAA;

// ---- wide-string helpers the MSVC CRT provides

int _wcsicmp(const WCHAR *a, const WCHAR *b);
int _stricmp(const char *a, const char *b);
int _strnicmp(const char *a, const char *b, size_t n);
int _snwprintf(WCHAR *out, size_t cap, const WCHAR *fmt, ...);
int _vsnwprintf(WCHAR *out, size_t cap, const WCHAR *fmt, va_list ap);
int swprintf_s(WCHAR *out, size_t cap, const WCHAR *fmt, ...);

// pipes
BOOL InitOnceExecuteOnce(PINIT_ONCE once, BOOL (*fn)(PINIT_ONCE, void *, void **), void *param, void **ctx);
BOOL CreatePipe(HANDLE *read_end, HANDLE *write_end, void *sa, DWORD size);
int _putenv_s(const char *name, const char *value);
#define LOCALE_NAME_MAX_LENGTH 85

// ---- remaining odds and ends the runtime uses

typedef struct { DWORD dwOemId; } MEMORYSTATUSEX;
#define GMEM_MOVEABLE 0x0002
#define GHND 0x0042
void *GlobalAlloc(UINT flags, SIZE_T bytes);
void *GlobalFree(void *ptr);
BOOL Beep(DWORD freq, DWORD ms);
DWORD GetFullPathNameW(const WCHAR *name, DWORD cap, WCHAR *out, WCHAR **file);
