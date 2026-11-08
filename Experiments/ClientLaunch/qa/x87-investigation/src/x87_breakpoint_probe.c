/* Synthetic x87 + breakpoint/cache test. Only this program's own memory is
 * changed; no game/protection code, debugger, or existing-process attachment.
 */
#define WIN32_LEAN_AND_MEAN
#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <libkern/OSCacheControl.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifdef _WIN64
#define PC(c) ((c)->Rip)
#else
#define PC(c) ((c)->Eip)
#endif
#endif

static unsigned char *entry, *trap_site;
static size_t page_size;
static volatile int traps;
static int restore_in_handler;
static uintptr_t observed_pc;
static unsigned char restore_opcode = 0x90;

static void flush(void *p, size_t size)
{
#ifdef _WIN32
    if (!FlushInstructionCache(GetCurrentProcess(), p, size)) exit(87);
#else
    sys_icache_invalidate(p, size);
#endif
}

#ifdef _WIN32
static LONG CALLBACK handler(EXCEPTION_POINTERS *e)
{
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT) {
        fprintf(stderr, "UNEXPECTED exception=0x%lx pc=%p\n",
                e->ExceptionRecord->ExceptionCode, (void *)(uintptr_t)PC(e->ContextRecord));
        ExitProcess(86);
    }
    ++traps;
    observed_pc = (uintptr_t)PC(e->ContextRecord);
    if (restore_in_handler) {
        *trap_site = restore_opcode;
        flush(trap_site, 1);
        PC(e->ContextRecord) = (uintptr_t)trap_site;
    } else {
        PC(e->ContextRecord) = (uintptr_t)(trap_site + (*trap_site == 0xcd ? 2 : 1));
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}
#else
static void handler(int sig, siginfo_t *info, void *raw)
{
    ucontext_t *context = raw;
    (void)info;
    if (sig != SIGTRAP) _exit(86);
    ++traps;
    observed_pc = context->uc_mcontext->__ss.__rip;
    if (restore_in_handler) {
        *trap_site = restore_opcode;
        flush(trap_site, 1);
        context->uc_mcontext->__ss.__rip = (uintptr_t)trap_site;
    } else {
        context->uc_mcontext->__ss.__rip = (uintptr_t)(trap_site + (*trap_site == 0xcd ? 2 : 1));
    }
}
#endif

static unsigned char *allocate(void)
{
#ifdef _WIN32
    return VirtualAlloc(NULL, page_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    void *p = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}

static int executable(void)
{
#ifdef _WIN32
    DWORD old;
    return VirtualProtect(entry, page_size, PAGE_EXECUTE_READWRITE, &old) != 0;
#else
    return mprotect(entry, page_size, PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
#endif
}

static void release(void)
{
#ifdef _WIN32
    if (!VirtualFree(entry, 0, MEM_RELEASE)) exit(87);
#else
    if (munmap(entry, page_size)) exit(87);
#endif
}

static int invoke(void)
{
    return ((int (*)(void))(void *)entry)();
}

static int probe(const char *name)
{
    /* push ax; fninit; fld1; fld1; faddp st1,st0; fistp dword[sp]; pop ax; ret.
     * The stack slot is 4 bytes in PE32, 8 in PE64; result uses EAX either way.
     * fninit resets guest x87 state; the stores empty the x87 stack every call.
     */
    const unsigned char operations[][3] = {
        {0x50, 0, 0}, {0xdb, 0xe3, 0}, {0xd9, 0xe8, 0},
        {0xd9, 0xe8, 0}, {0xde, 0xc1, 0}, {0xdb, 0x1c, 0x24},
        {0x58, 0, 0}, {0xc3, 0, 0}
    };
    const size_t lengths[] = {1, 2, 2, 2, 2, 3, 1, 1};
    int position = -1, two_byte = 0, warm = 0, direct = 0, ok = 1, patch_existing = 0;
    size_t length = 0;
    if (!strcmp(name, "x87-control")) position = -1;
    else if (!strcmp(name, "x87-int3-before")) position = 2;
    else if (!strcmp(name, "x87-int3-between")) position = 3;
    else if (!strcmp(name, "x87-int3-after")) position = 5;
    else if (!strcmp(name, "x87-int3-after-store")) position = 6;
    else if (!strcmp(name, "x87-int03-after")) { position = 5; two_byte = 1; }
    else if (!strcmp(name, "x87-wpm-before")) { position = 2; warm = 1; }
    else if (!strcmp(name, "x87-wpm-after")) { position = 5; warm = 1; }
    else if (!strcmp(name, "x87-direct-after")) { position = 5; warm = 1; direct = 1; }
    else if (!strcmp(name, "x87-trap-restore")) { position = 5; restore_in_handler = 1; }
    else if (!strcmp(name, "x87-patch-fused-wpm") || !strcmp(name, "x87-patch-fused-direct")) {
        position = 3; warm = 1; patch_existing = 1; restore_in_handler = 1;
        direct = !strcmp(name, "x87-patch-fused-direct"); restore_opcode = 0xd9;
    }
    else return 2;
    entry = allocate();
    if (!entry) return 87;
    memset(entry, 0x90, page_size);
    for (int i = 0; i < 8; ++i) {
        if (i == position) {
            trap_site = entry + length;
            if (!patch_existing) {
                entry[length++] = warm ? 0x90 : two_byte ? 0xcd : 0xcc;
                if (two_byte) entry[length++] = 0x03;
            }
        }
        memcpy(entry + length, operations[i], lengths[i]);
        length += lengths[i];
    }
    if (!executable()) return 87;
    flush(entry, length);
    printf("EXEC entry=%p trap=%p bytes=", (void *)entry, (void *)trap_site);
    for (size_t i = 0; i < length; ++i) printf("%02x", entry[i]);
    puts("");
    if (warm) {
        int before = invoke();
        printf("WARM value=%d expected=2 traps=%d\n", before, traps);
        ok = before == 2 && traps == 0;
        const unsigned char int3 = 0xcc;
        if (direct) *trap_site = int3;
        else {
#ifdef _WIN32
            SIZE_T written;
            if (!WriteProcessMemory(GetCurrentProcess(), trap_site, &int3, 1, &written) || written != 1)
                return 87;
#else
            return 2; /* Windows API, not a native comparison. */
#endif
        }
        flush(trap_site, 1);
    }
    int value = invoke();
    int expected_traps = position < 0 ? 0 : 1;
    printf("OBS value=%d expected=2 traps=%d expected-traps=%d restored=%d pc=%p\n",
           value, traps, expected_traps, restore_in_handler ? *trap_site == restore_opcode : 0,
           (void *)observed_pc);
    ok = ok && value == 2 && traps == expected_traps;
    if (restore_in_handler) ok = ok && *trap_site == restore_opcode;
    release();
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (argc != 2) return 2;
#ifdef _WIN32
    SYSTEM_INFO info;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    GetSystemInfo(&info);
    page_size = info.dwPageSize;
    PVOID cookie = AddVectoredExceptionHandler(1, handler);
    if (!cookie) return 87;
    printf("BEGIN case=%s platform=windows bits=%u page=%zu\n", argv[1],
           (unsigned)(sizeof(void *) * 8), page_size);
#else
    struct sigaction action;
    int translated = -1;
    size_t size = sizeof(translated);
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = handler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTRAP, &action, NULL) || sigaction(SIGILL, &action, NULL)) return 87;
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    sysctlbyname("sysctl.proc_translated", &translated, &size, NULL, 0);
    printf("BEGIN case=%s platform=macos bits=64 translated=%d page=%zu\n", argv[1], translated, page_size);
    if (translated != 1) return 87;
#endif
    int result = probe(argv[1]);
#ifdef _WIN32
    RemoveVectoredExceptionHandler(cookie);
#endif
    printf("END case=%s status=%s exit=%d\n", argv[1], result ? "FAIL" : "PASS", result);
    return result;
}
