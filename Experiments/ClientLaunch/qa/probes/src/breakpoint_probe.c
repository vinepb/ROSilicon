/* Machine-local synthetic test: no game/protection code or debugger attachment.
 * Same instruction bytes are run as PE32, PE64 and translated Mach-O x86_64.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <signal.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <libkern/OSCacheControl.h>
#endif

enum { MODE_DYNAMIC, MODE_STATIC, MODE_RAISED, MODE_STEP, MODE_RESTORE };
static volatile int mode, traps;
static uintptr_t resume_pc, observed_pc;
static unsigned long observed_code, expected_code;
static size_t page_size;
static unsigned char *restore_byte;
static int threaded, thread_go, thread_ready, thread_errors;
static unsigned char *thread_code;
static void flush(unsigned char *p, size_t size);

#ifdef _WIN32
#ifdef _WIN64
#define CTX_PC(c) ((c)->Rip)
#else
#define CTX_PC(c) ((c)->Eip)
#endif
#define CODE_BREAK EXCEPTION_BREAKPOINT
#define CODE_STEP EXCEPTION_SINGLE_STEP
#define CODE_ILLEGAL EXCEPTION_ILLEGAL_INSTRUCTION
static LONG CALLBACK handler(EXCEPTION_POINTERS *e)
{
    if (e->ExceptionRecord->ExceptionCode != expected_code) {
        fprintf(stderr, "UNEXPECTED exception=0x%lx pc=%p\n",
                e->ExceptionRecord->ExceptionCode, (void *)(uintptr_t)CTX_PC(e->ContextRecord));
        fflush(stderr);
        ExitProcess(86);
    }
    __atomic_fetch_add(&traps, 1, __ATOMIC_RELAXED);
    if (!threaded) {
        observed_code = e->ExceptionRecord->ExceptionCode;
        observed_pc = (uintptr_t)CTX_PC(e->ContextRecord);
    }
    if (mode == MODE_RESTORE) {
        *restore_byte = 0x90;
        flush(restore_byte, 1);
        CTX_PC(e->ContextRecord) = (uintptr_t)restore_byte;
    } else if (mode == MODE_DYNAMIC) CTX_PC(e->ContextRecord) = resume_pc;
    else if (mode == MODE_STATIC) CTX_PC(e->ContextRecord)++;
    e->ContextRecord->EFlags &= ~0x100u;
    return EXCEPTION_CONTINUE_EXECUTION;
}
static unsigned char *allocate(size_t size)
{
    return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}
static void release(unsigned char *p, size_t size)
{
    (void)size;
    if (!VirtualFree(p, 0, MEM_RELEASE)) exit(87);
}
static void protect(unsigned char *p, size_t size, int writable, int executable)
{
    DWORD old, prot = executable ? (writable ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ)
                                 : PAGE_READWRITE;
    if (!VirtualProtect(p, size, prot, &old)) {
        fprintf(stderr, "VirtualProtect failed: %lu\n", GetLastError());
        exit(87);
    }
}
static void flush(unsigned char *p, size_t size)
{
    if (!FlushInstructionCache(GetCurrentProcess(), p, size)) exit(87);
}
#else
#define CODE_BREAK SIGTRAP
#define CODE_STEP SIGTRAP
#define CODE_ILLEGAL SIGILL
static void handler(int sig, siginfo_t *info, void *raw)
{
    ucontext_t *c = raw;
    (void)info;
    if ((unsigned long)sig != expected_code) {
        const char *msg = sig == SIGILL ? "UNEXPECTED native SIGILL\n" : "UNEXPECTED native signal\n";
        (void)write(2, msg, 25);
        _exit(86);
    }
    __atomic_fetch_add(&traps, 1, __ATOMIC_RELAXED);
    if (!threaded) {
        observed_code = (unsigned long)sig;
        observed_pc = (uintptr_t)c->uc_mcontext->__ss.__rip;
    }
    if (mode == MODE_RESTORE) {
        *restore_byte = 0x90;
        flush(restore_byte, 1);
        c->uc_mcontext->__ss.__rip = (uintptr_t)restore_byte;
    } else if (mode == MODE_DYNAMIC) c->uc_mcontext->__ss.__rip = resume_pc;
    /* Native SIGTRAP already saves the address following static int3. */
    c->uc_mcontext->__ss.__rflags &= ~0x100u;
}
static unsigned char *allocate(size_t size)
{
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}
static void release(unsigned char *p, size_t size)
{
    if (munmap(p, size)) exit(87);
}
static void protect(unsigned char *p, size_t size, int writable, int executable)
{
    int prot = PROT_READ | (writable ? PROT_WRITE : 0) | (executable ? PROT_EXEC : 0);
    if (mprotect(p, size, prot)) { perror("mprotect"); exit(87); }
}
static void flush(unsigned char *p, size_t size)
{
    sys_icache_invalidate(p, size);
}
#endif

typedef int (*code_fn)(void);
static __attribute__((noinline)) void static_int3(void) { __asm__ volatile("int3"); }

static int invoke(unsigned char *p)
{
    return ((code_fn)(void *)p)();
}

static int dynamic(const unsigned char *prefix, size_t length, int writable,
                   unsigned long code, size_t offset)
{
    const unsigned char tail[] = {0xb8, 42, 0, 0, 0, 0xc3}; /* mov eax,42; ret */
    size_t size = page_size * 2;
    unsigned char *p = allocate(size), *entry;
    int value;
    if (!p) { fprintf(stderr, "allocation failed\n"); return 87; }
    memset(p, 0x90, size);
    entry = p + offset;
    if (length) memcpy(entry, prefix, length);
    memcpy(entry + length, tail, sizeof(tail));
    expected_code = code;
    mode = MODE_DYNAMIC;
    resume_pc = (uintptr_t)(entry + length);
    protect(p, size, writable, 1);
    flush(p, size);
    printf("EXEC entry=%p resume=%p bytes=", (void *)entry, (void *)resume_pc);
    for (size_t i = 0; i < length; i++) printf("%02x", prefix[i]);
    puts("");
    value = invoke(entry);
    printf("OBS value=%d traps=%d code=0x%lx pc=%p\n", value, traps,
           observed_code, (void *)observed_pc);
    release(p, size);
    return value == 42 && traps == (code ? 1 : 0) ? 0 : 1;
}

static int mutate(int wx)
{
    /* Modify only this synthetic allocation, synchronously and with cache flushes. */
    unsigned char code[] = {0x90, 0xb8, 42, 0, 0, 0, 0xc3};
    unsigned char *p = allocate(page_size);
    int ok = 1;
    if (!p) return 87;
    memcpy(p, code, sizeof(code));
    protect(p, page_size, !wx, 1);
    flush(p, page_size);
    expected_code = CODE_BREAK;
    mode = MODE_DYNAMIC;
    resume_pc = (uintptr_t)(p + 1);
    if (invoke(p) != 42) ok = 0;
    for (int i = 0; i < 128 && ok; i++) {
        if (wx) protect(p, page_size, 1, 0);
        p[0] = 0xcc;
        p[2] = (unsigned char)(i + 1);
        if (wx) protect(p, page_size, 0, 1);
        flush(p, sizeof(code));
        if (invoke(p) != i + 1 || traps != i + 1) ok = 0;
        if (wx) protect(p, page_size, 1, 0);
        p[0] = 0x90;
        if (wx) protect(p, page_size, 0, 1);
        flush(p, sizeof(code));
        if (invoke(p) != i + 1 || traps != i + 1) ok = 0;
    }
    printf("OBS mutate traps=%d expected=128 pc=%p\n", traps, (void *)observed_pc);
    release(p, page_size);
    return ok && traps == 128 ? 0 : 1;
}

static int step(void)
{
    /* pushf; or byte [sp+1],1 (TF); popf; nop; mov eax,42; ret.
     * Both 32-bit and 64-bit stack forms use these same bytes.
     */
    const unsigned char code[] = {0x9c, 0x80, 0x4c, 0x24, 0x01, 0x01,
                                  0x9d, 0x90, 0xb8, 42, 0, 0, 0, 0xc3};
    unsigned char *p = allocate(page_size);
    int value;
    if (!p) return 87;
    memcpy(p, code, sizeof(code));
    mode = MODE_STEP;
    expected_code = CODE_STEP;
    protect(p, page_size, 0, 1);
    flush(p, sizeof(code));
    value = invoke(p);
    printf("OBS step value=%d traps=%d code=0x%lx pc=%p\n",
           value, traps, observed_code, (void *)observed_pc);
    release(p, page_size);
    return value == 42 && traps == 1 ? 0 : 1;
}

static void thread_pause(void)
{
#ifdef _WIN32
    Sleep(1);
#else
    usleep(1000);
#endif
}

#ifdef _WIN32
static DWORD WINAPI thread_worker(void *unused)
#else
static void *thread_worker(void *unused)
#endif
{
    (void)unused;
    __atomic_fetch_add(&thread_ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&thread_go, __ATOMIC_ACQUIRE)) thread_pause();
    for (int i = 0; i < 1000; i++) {
        if (invoke(thread_code) != 42) __atomic_fetch_add(&thread_errors, 1, __ATOMIC_RELAXED);
        if (!(i % 16)) thread_pause();
    }
    return 0;
}

static int thread_test(int modify)
{
    const unsigned char code[] = {0xcc, 0xb8, 42, 0, 0, 0, 0xc3};
    unsigned char *p = allocate(page_size);
    int ok;
#ifdef _WIN32
    HANDLE threads[4];
#else
    pthread_t threads[4];
#endif
    if (!p) return 87;
    memcpy(p, code, sizeof(code));
    protect(p, page_size, 1, 1);
    flush(p, sizeof(code));
    expected_code = CODE_BREAK;
    resume_pc = (uintptr_t)(p + 1);
    mode = MODE_DYNAMIC;
    threaded = 1;
    thread_code = p;
    for (int i = 0; i < 4; i++) {
#ifdef _WIN32
        threads[i] = CreateThread(NULL, 0, thread_worker, NULL, 0, NULL);
        if (!threads[i]) return 87;
#else
        if (pthread_create(&threads[i], NULL, thread_worker, NULL)) return 87;
#endif
    }
    while (__atomic_load_n(&thread_ready, __ATOMIC_ACQUIRE) != 4) thread_pause();
    __atomic_store_n(&thread_go, 1, __ATOMIC_RELEASE);
    if (modify) for (int i = 0; i < 128; i++) {
        /* One-byte atomic replacement; both old/new streams are valid.
         * This is a crash stress test, not an x86 cache-ordering conformance claim. */
        __atomic_store_n(p, (i & 1) ? 0xcc : 0x90, __ATOMIC_RELAXED);
        flush(p, 1);
        thread_pause();
    }
    for (int i = 0; i < 4; i++) {
#ifdef _WIN32
        if (WaitForSingleObject(threads[i], 5000) != WAIT_OBJECT_0) return 88;
        CloseHandle(threads[i]);
#else
        if (pthread_join(threads[i], NULL)) return 88;
#endif
    }
    threaded = 0;
    printf("OBS threaded modify=%d traps=%d errors=%d calls=4000\n", modify, traps, thread_errors);
    ok = !thread_errors && (modify ? traps > 0 : traps == 4000);
    release(p, page_size);
    return ok ? 0 : 1;
}

static int restore(void)
{
    const unsigned char code[] = {0xcc, 0xb8, 42, 0, 0, 0, 0xc3};
    unsigned char *p = allocate(page_size);
    int ok = 1;
    if (!p) return 87;
    memcpy(p, code, sizeof(code));
    protect(p, page_size, 1, 1);
    flush(p, sizeof(code));
    restore_byte = p;
    mode = MODE_RESTORE;
    expected_code = CODE_BREAK;
    for (int i = 0; i < 256 && ok; i++) {
        p[0] = 0xcc;
        flush(p, 1);
        if (invoke(p) != 42 || traps != i + 1 || p[0] != 0x90) ok = 0;
    }
    printf("OBS restore traps=%d expected=256 pc=%p\n", traps, (void *)observed_pc);
    release(p, page_size);
    return ok && traps == 256 ? 0 : 1;
}

#ifdef _WIN32
static int writeproc(int executable_allocation, int explicit_toggle)
{
    const unsigned char initial[] = {0x90, 0xb8, 42, 0, 0, 0, 0xc3};
    const unsigned char changed[] = {0xcc, 0xb8, 99, 0, 0, 0, 0xc3};
    DWORD protection = executable_allocation ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
    unsigned char *p = VirtualAlloc(NULL, page_size, MEM_COMMIT | MEM_RESERVE, protection);
    MEMORY_BASIC_INFORMATION info;
    SIZE_T written;
    int initial_value, changed_value;
    if (!p) return 87;
    memcpy(p, initial, sizeof(initial));
    protect(p, page_size, 1, 1);
    flush(p, sizeof(initial));
    mode = MODE_DYNAMIC;
    expected_code = CODE_BREAK;
    resume_pc = (uintptr_t)(p + 1);
    initial_value = invoke(p); /* Warm Rosetta's translation before the kernel write. */
    if (!WriteProcessMemory(GetCurrentProcess(), p, changed, sizeof(changed), &written) ||
        written != sizeof(changed)) { fprintf(stderr, "WPM failed %lu\n", GetLastError()); return 87; }
    flush(p, sizeof(changed));
    printf("MEM after-WPM first=0x%02x immediate=%u expected=0xcc,99 toggle=%d\n", p[0], p[2], explicit_toggle);
    if (explicit_toggle) {
        protect(p, page_size, 1, 0);
        protect(p, page_size, 1, 1);
        flush(p, sizeof(changed));
    }
    changed_value = invoke(p);
    if (!VirtualQuery(p, &info, sizeof(info)) || info.Protect != PAGE_EXECUTE_READWRITE) {
        fprintf(stderr, "WPM did not preserve current protection\n"); return 87;
    }
    printf("OBS writeproc allocation=0x%lx initial=%d changed=%d traps=%d expected=1 pc=%p\n",
           protection, initial_value, changed_value, traps, (void *)observed_pc);
    release(p, page_size);
    return initial_value == 42 && changed_value == 99 && traps == 1 ? 0 : 1;
}

static int writeproc_mixed(void)
{
    const unsigned char initial[] = {0x90, 0xb8, 42, 0, 0, 0, 0xc3};
    const unsigned char changed[] = {0x22, 0xcc, 0xb8, 99, 0, 0, 0, 0xc3};
    unsigned char *p = allocate(page_size * 2), *entry;
    MEMORY_BASIC_INFORMATION data_info, code_info;
    SIZE_T written;
    int before, after;
    if (!p) return 87;
    entry = p + page_size;
    p[page_size - 1] = 0x11;
    memcpy(entry, initial, sizeof(initial));
    protect(entry, page_size, 1, 1); /* First page remains plain data (RW). */
    flush(entry, sizeof(initial));
    mode = MODE_DYNAMIC;
    expected_code = CODE_BREAK;
    resume_pc = (uintptr_t)(entry + 1);
    before = invoke(entry);
    if (!WriteProcessMemory(GetCurrentProcess(), entry - 1, changed, sizeof(changed), &written) ||
        written != sizeof(changed)) return 87;
    flush(entry, sizeof(initial));
    after = invoke(entry);
    if (!VirtualQuery(p, &data_info, sizeof(data_info)) ||
        !VirtualQuery(entry, &code_info, sizeof(code_info))) return 87;
    printf("OBS mixed before=%d after=%d traps=%d data-byte=0x%02x "
           "data-protect=0x%lx code-protect=0x%lx\n", before, after, traps,
           p[page_size - 1], data_info.Protect, code_info.Protect);
    release(p, page_size * 2);
    return before == 42 && after == 99 && traps == 1 &&
           data_info.Protect == PAGE_READWRITE && code_info.Protect == PAGE_EXECUTE_READWRITE ? 0 : 1;
}

struct child_exchange {
    volatile LONG state;
    uintptr_t code;
    int initial, result, traps;
    DWORD protection;
};

static int child_wait(struct child_exchange *shared, LONG state)
{
    for (int i = 0; i < 4000; i++) {
        if (InterlockedCompareExchange(&shared->state, 0, 0) == state) return 1;
        Sleep(1);
    }
    return 0;
}

static int child_entry(HANDLE mapping)
{
    const unsigned char initial[] = {0x90, 0xb8, 42, 0, 0, 0, 0xc3};
    struct child_exchange *shared = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(*shared));
    unsigned char *p = allocate(page_size);
    MEMORY_BASIC_INFORMATION info;
    if (!shared || !p) return 87;
    memcpy(p, initial, sizeof(initial));
    protect(p, page_size, 1, 1);
    flush(p, sizeof(initial));
    mode = MODE_DYNAMIC;
    expected_code = CODE_BREAK;
    resume_pc = (uintptr_t)(p + 1);
    shared->code = (uintptr_t)p;
    shared->initial = invoke(p);
    InterlockedExchange(&shared->state, 1);
    if (!child_wait(shared, 2)) return 88;
    shared->result = invoke(p);
    shared->traps = traps;
    if (!VirtualQuery(p, &info, sizeof(info))) return 87;
    shared->protection = info.Protect;
    InterlockedExchange(&shared->state, 3);
    release(p, page_size);
    UnmapViewOfFile(shared);
    CloseHandle(mapping);
    return 0;
}

static int writeproc_child(void)
{
    const unsigned char changed[] = {0xcc, 0xb8, 99, 0, 0, 0, 0xc3};
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    STARTUPINFOA startup;
    PROCESS_INFORMATION process;
    HANDLE mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
                                       0, sizeof(struct child_exchange), NULL);
    struct child_exchange *shared;
    char executable[MAX_PATH], command[MAX_PATH + 80];
    SIZE_T written;
    DWORD child_status;
    int result = 88;
    if (!mapping) return 87;
    shared = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(*shared));
    if (!shared || !GetModuleFileNameA(NULL, executable, sizeof(executable))) return 87;
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    snprintf(command, sizeof(command), "\"%s\" child:%llu", executable,
             (unsigned long long)(uintptr_t)mapping);
    if (!CreateProcessA(NULL, command, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &startup, &process)) return 87;
    /* This is a synthetic child created by this probe, never an existing process. */
    if (child_wait(shared, 1) &&
        WriteProcessMemory(process.hProcess, (void *)shared->code, changed, sizeof(changed), &written) &&
        written == sizeof(changed) && FlushInstructionCache(process.hProcess, (void *)shared->code, sizeof(changed))) {
        InterlockedExchange(&shared->state, 2);
        if (child_wait(shared, 3)) {
            printf("OBS child initial=%d result=%d traps=%d protection=0x%lx\n",
                   shared->initial, shared->result, shared->traps, shared->protection);
            result = shared->initial == 42 && shared->result == 99 && shared->traps == 1 &&
                     shared->protection == PAGE_EXECUTE_READWRITE ? 0 : 1;
        }
    }
    if (WaitForSingleObject(process.hProcess, 2000) != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 88);
        WaitForSingleObject(process.hProcess, 2000);
        result = 88;
    }
    if (!GetExitCodeProcess(process.hProcess, &child_status) || child_status) result = 88;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    UnmapViewOfFile(shared);
    CloseHandle(mapping);
    return result;
}
#endif

int main(int argc, char **argv)
{
    const unsigned char int3[] = {0xcc}, int03[] = {0xcd, 0x03};
    const unsigned char rep_int3[] = {0xf3, 0xcc}, icebp[] = {0xf1}, ud2[] = {0x0f, 0x0b};
    int result = 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (argc != 2) { fprintf(stderr, "Usage: probe CASE\n"); return 2; }
#ifdef _WIN32
    SYSTEM_INFO system;
    PVOID cookie;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    GetSystemInfo(&system);
    page_size = system.dwPageSize;
    cookie = AddVectoredExceptionHandler(1, handler);
    if (!cookie) return 87;
    printf("BEGIN case=%s platform=windows bits=%u page=%zu\n", argv[1],
           (unsigned)(sizeof(void *) * 8), page_size);
    if (!strncmp(argv[1], "child:", 6))
        return child_entry((HANDLE)(uintptr_t)strtoull(argv[1] + 6, NULL, 10));
#else
    struct sigaction action;
    int translated = -1;
    size_t len = sizeof(translated);
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = handler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTRAP, &action, NULL) || sigaction(SIGILL, &action, NULL)) return 87;
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    sysctlbyname("sysctl.proc_translated", &translated, &len, NULL, 0);
    printf("BEGIN case=%s platform=macos bits=%u translated=%d page=%zu\n", argv[1],
           (unsigned)(sizeof(void *) * 8), translated, page_size);
    if (translated != 1) { fprintf(stderr, "Expected stock Rosetta\n"); return 87; }
#endif
    if (!strcmp(argv[1], "control")) result = dynamic(NULL, 0, 0, 0, 0);
    else if (!strcmp(argv[1], "static-int3")) {
        mode = MODE_STATIC;
        expected_code = CODE_BREAK;
        static_int3();
        printf("OBS static traps=%d pc=%p\n", traps, (void *)observed_pc);
        result = traps == 1 ? 0 : 1;
    } else if (!strcmp(argv[1], "raised")) {
        mode = MODE_RAISED;
        expected_code = CODE_BREAK;
#ifdef _WIN32
        RaiseException(EXCEPTION_BREAKPOINT, 0, 0, NULL);
#else
        raise(SIGTRAP);
#endif
        printf("OBS raised traps=%d pc=%p\n", traps, (void *)observed_pc);
        result = traps == 1 ? 0 : 1;
    } else if (!strcmp(argv[1], "int3-rx")) result = dynamic(int3, 1, 0, CODE_BREAK, 0);
    else if (!strcmp(argv[1], "int3-rwx")) result = dynamic(int3, 1, 1, CODE_BREAK, 0);
    else if (!strcmp(argv[1], "int03-rx")) result = dynamic(int03, 2, 0, CODE_BREAK, 0);
    else if (!strcmp(argv[1], "int03-rwx")) result = dynamic(int03, 2, 1, CODE_BREAK, 0);
    else if (!strcmp(argv[1], "rep-int3")) result = dynamic(rep_int3, 2, 0, CODE_BREAK, 0);
    else if (!strcmp(argv[1], "icebp")) result = dynamic(icebp, 1, 0, CODE_STEP, 0);
    else if (!strcmp(argv[1], "ud2")) result = dynamic(ud2, 2, 0, CODE_ILLEGAL, 0);
    else if (!strcmp(argv[1], "boundary-int03")) result = dynamic(int03, 2, 0, CODE_BREAK, page_size - 1);
    else if (!strcmp(argv[1], "single-step")) result = step();
    else if (!strcmp(argv[1], "mutate-rwx")) result = mutate(0);
    else if (!strcmp(argv[1], "mutate-wx")) result = mutate(1);
    else if (!strcmp(argv[1], "trap-restore")) result = restore();
    else if (!strcmp(argv[1], "thread-int3")) result = thread_test(0);
    else if (!strcmp(argv[1], "thread-mutate")) result = thread_test(1);
#ifdef _WIN32
    else if (!strcmp(argv[1], "writeproc-rwalloc")) result = writeproc(0, 0);
    else if (!strcmp(argv[1], "writeproc-execalloc")) result = writeproc(1, 0);
    else if (!strcmp(argv[1], "writeproc-explicit-toggle")) result = writeproc(0, 1);
    else if (!strcmp(argv[1], "writeproc-mixed")) result = writeproc_mixed();
    else if (!strcmp(argv[1], "writeproc-child")) result = writeproc_child();
#endif
#ifdef _WIN32
    RemoveVectoredExceptionHandler(cookie);
#endif
    printf("END case=%s status=%s exit=%d\n", argv[1], result ? "FAIL" : "PASS", result);
    return result;
}
