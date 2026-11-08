/* OWN prepublished code entered from nested Windows exception callbacks.
 * No game, external memory, publication, allocator or lock in success handlers. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN64
#define PC(c) ((c)->Rip)
#define AX(c) ((c)->Rax)
#define SP(c) ((c)->Rsp)
#else
#define PC(c) ((c)->Eip)
#define AX(c) ((c)->Eax)
#define SP(c) ((c)->Esp)
#endif

enum publication { STABLE, WX, WPM };
struct frame {
    unsigned char *code, *entry, *trap;
    volatile LONG math;
    unsigned tag, publications, ready;
    volatile unsigned calls, warm_calls, traps, warm_traps, parent_checks;
};
static struct frame frames[4];
static EXCEPTION_POINTERS *active_contexts[4];
static volatile unsigned active_frame, warming, failures;
static unsigned depth, warm_children;
static HANDLE error_handle;

static void fail_number(const char *label, DWORD length, uintptr_t value) {
    static const char hex[] = "0123456789abcdef";
    char bytes[sizeof(value) * 2 + 1];
    for (unsigned i = 0; i < sizeof(value) * 2; ++i)
        bytes[i] = hex[(value >> ((sizeof(value) * 2 - i - 1) * 4)) & 15];
    bytes[sizeof(value) * 2] = '\n';
    DWORD written;
    WriteFile(error_handle, label, length, &written, NULL);
    WriteFile(error_handle, bytes, sizeof(bytes), &written, NULL);
}

static void fatal_context(EXCEPTION_POINTERS *event) {
    // Failure-only minimal own diagnostics, then terminate this process.
    // Never skip an unknown PC or turn an unknown exception into a pass.
    fail_number("FAIL exception=", sizeof("FAIL exception=") - 1, event->ExceptionRecord->ExceptionCode);
    fail_number("DETAIL pc=", sizeof("DETAIL pc=") - 1, (uintptr_t)PC(event->ContextRecord));
    fail_number("DETAIL frame=", sizeof("DETAIL frame=") - 1, active_frame);
    TerminateProcess(GetCurrentProcess(), 86);
}

static int invoke(unsigned index) {
    struct frame *frame = &frames[index];
    const unsigned previous = active_frame;
    const unsigned old_traps = warming ? frame->warm_traps : frame->traps;
    active_frame = index; frame->math = -1;
    typedef int (__cdecl *own_function)(volatile LONG *);
    const int got = ((own_function)(void *)frame->entry)(&frame->math);
    const unsigned got_traps = warming ? frame->warm_traps : frame->traps;
    if (got != (int)frame->tag || frame->math != 2 || got_traps != old_traps + 1) failures |= 2;
    if (warming) ++frame->warm_calls; else ++frame->calls;
    active_frame = previous;
    return got;
}

static LONG CALLBACK own_handler(EXCEPTION_POINTERS *event) {
    const unsigned index = active_frame;
    if (index > depth || event->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT ||
        (uintptr_t)PC(event->ContextRecord) != (uintptr_t)frames[index].trap) {
        fatal_context(event); return EXCEPTION_CONTINUE_SEARCH;
    }
    struct frame *frame = &frames[index];
    if ((unsigned)AX(event->ContextRecord) != frame->tag || frame->math != 2) failures |= 1;
    if (warming) ++frame->warm_traps; else ++frame->traps;
    const uintptr_t parent_pc = (uintptr_t)PC(event->ContextRecord);
    const uintptr_t parent_ax = (uintptr_t)AX(event->ContextRecord);
    const uintptr_t parent_sp = (uintptr_t)SP(event->ContextRecord);
    for (unsigned i = 0; i < index; ++i) {
        if (active_contexts[i] && (active_contexts[i] == event ||
            active_contexts[i]->ContextRecord == event->ContextRecord)) failures |= 8;
    }
    active_contexts[index] = event;
    if (!warming && index < depth) {
        // Child was published outside all callbacks; its first entry can be
        // cold. No success-path Windows API, allocation, publication or lock.
        invoke(index + 1);
        if ((uintptr_t)PC(event->ContextRecord) != parent_pc ||
            (uintptr_t)AX(event->ContextRecord) != parent_ax ||
            (uintptr_t)SP(event->ContextRecord) != parent_sp) failures |= 4;
        ++frame->parent_checks;
    }
    active_contexts[index] = NULL;
    PC(event->ContextRecord) = (uintptr_t)(frame->trap + 1);
    return EXCEPTION_CONTINUE_EXECUTION;
}

static size_t make_block(unsigned char *bytes, unsigned pad, unsigned tag, size_t *trap_offset) {
    size_t n = 0;
#ifndef _WIN64
    const unsigned char argument[] = {0x8b,0x4c,0x24,0x04}; // ECX = cdecl argument
    memcpy(bytes + n, argument, sizeof(argument)); n += sizeof(argument);
#endif
    memset(bytes + n, 0x90, pad); n += pad;
    // FNINIT; FLD1; FLD1; FADDP; FISTP dword [ECX/RCX]. Store before INT3:
    // no deliberately live x87 value spans an exception in this oracle.
    const unsigned char math[] = {0xdb,0xe3,0xd9,0xe8,0xd9,0xe8,0xde,0xc1,0xdb,0x19};
    memcpy(bytes + n, math, sizeof(math)); n += sizeof(math);
    bytes[n++] = 0xb8; memcpy(bytes + n, &tag, 4); n += 4;
    *trap_offset = n; bytes[n++] = 0xcc; bytes[n++] = 0xc3;
    return n;
}

static int publish(struct frame *frame, unsigned index, unsigned round,
                   enum publication publication, size_t page) {
    if (publication == STABLE && frame->ready) return 1;
    unsigned char bytes[128]; size_t trap_offset;
    frame->tag = round * 31 + index * 100000 + 5;
    const size_t n = make_block(bytes, (round * 13 + index * 7) % 96, frame->tag, &trap_offset);
    if (n > sizeof(bytes)) return 0;
    frame->entry = frame->code + 32; frame->trap = frame->entry + trap_offset;
    DWORD previous;
    if (publication == WPM) {
        if (!frame->ready && !VirtualProtect(frame->code, page, PAGE_EXECUTE_READ, &previous)) return 0;
        SIZE_T written;
        if (!WriteProcessMemory(GetCurrentProcess(), frame->entry, bytes, n, &written) || written != n) return 0;
    } else {
        if (!VirtualProtect(frame->code, page, PAGE_READWRITE, &previous)) return 0;
        memcpy(frame->entry, bytes, n);
        if (!VirtualProtect(frame->code, page, PAGE_EXECUTE_READ, &previous)) return 0;
    }
    if (!FlushInstructionCache(GetCurrentProcess(), frame->entry, n)) return 0;
    ++frame->publications; frame->ready = 1; return 1;
}

static int number(const char *text, unsigned low, unsigned high, unsigned *result) {
    char *end; const unsigned long value = strtoul(text, &end, 10);
    if (!*text || *end || value < low || value > high) return 0;
    *result = (unsigned)value; return 1;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    static const char *names[] = {"stable", "wx", "wpm"};
    unsigned publication = 3, rounds;
    if (argc != 5 || !number(argv[3], 0, 3, &depth) || !number(argv[4], 1, 2048, &rounds)) return 2;
    for (unsigned i = 0; i < 3; ++i) if (!strcmp(argv[1], names[i])) publication = i;
    if (publication == 3 || (strcmp(argv[2], "first") && strcmp(argv[2], "warm"))) return 2;
    warm_children = !strcmp(argv[2], "warm");
    SYSTEM_INFO info; GetSystemInfo(&info);
    printf("BEGIN case=nested-exception bits=%u publication=%s children=%s depth=%u rounds=%u\n",
           (unsigned)(sizeof(void *) * 8), names[publication], argv[2], depth, rounds);
    error_handle = GetStdHandle(STD_ERROR_HANDLE);
    for (unsigned i = 0; i <= depth; ++i) {
        frames[i].code = VirtualAlloc(NULL, info.dwPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!frames[i].code) return 87;
    }
    void *handler = AddVectoredExceptionHandler(1, own_handler);
    if (!handler) return 87;
    const ULONGLONG deadline = GetTickCount64() + 15000;
    unsigned completed = 0;
    for (unsigned round = 0; round < rounds; ++round) {
        // All writes/protection changes are outside exception handlers.
        for (unsigned i = 0; i <= depth; ++i)
            if (!publish(&frames[i], i, round, (enum publication)publication, info.dwPageSize)) return 87;
        if (warm_children) {
            warming = 1;
            for (unsigned i = 1; i <= depth; ++i) invoke(i);
            warming = 0;
        }
        invoke(0);
        if (failures) break;
        ++completed;
        if (GetTickCount64() > deadline) { failures |= 16; break; }
    }
    for (unsigned i = 0; i <= depth; ++i) {
        struct frame *frame = &frames[i];
        const unsigned expected_warm = warm_children && i ? completed : 0;
        printf("OBS frame=%u publications=%u calls=%u traps=%u warm-calls=%u warm-traps=%u parent-checks=%u math=%ld\n",
               i, frame->publications, frame->calls, frame->traps, frame->warm_calls,
               frame->warm_traps, frame->parent_checks, frame->math);
        if (frame->calls != completed || frame->traps != completed || frame->warm_calls != expected_warm ||
            frame->warm_traps != expected_warm || frame->parent_checks != (i < depth ? completed : 0)) failures |= 32;
        if (!VirtualFree(frame->code, 0, MEM_RELEASE)) return 87;
    }
    if (!RemoveVectoredExceptionHandler(handler)) return 87;
    printf("END case=nested-exception completed=%u failure-mask=%u status=%s\n",
           completed, failures, !failures && completed == rounds ? "PASS" : "FAIL");
    return !failures && completed == rounds ? 0 : 1;
}
