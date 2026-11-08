/* Own prepublished allocations reached by changing an own exception's saved PC.
 * No game, external memory or code publication inside a success callback. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN64
#define PC(c) ((c)->Rip)
#define AX(c) ((c)->Rax)
#define CX(c) ((c)->Rcx)
#define SP(c) ((c)->Rsp)
#else
#define PC(c) ((c)->Eip)
#define AX(c) ((c)->Eax)
#define CX(c) ((c)->Ecx)
#define SP(c) ((c)->Esp)
#endif

enum publication { STABLE, WX, WPM };
struct frame {
    unsigned char *code, *entry, *trap;
    unsigned tag, ready, publications;
    volatile unsigned entries, redirects, warm_calls, warm_traps;
};
static struct frame frames[4];
static volatile unsigned active_frame, warming, failures;
static volatile LONG math_result;
static uintptr_t original_sp;
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
    fail_number("FAIL exception=", sizeof("FAIL exception=") - 1, event->ExceptionRecord->ExceptionCode);
    fail_number("DETAIL pc=", sizeof("DETAIL pc=") - 1, (uintptr_t)PC(event->ContextRecord));
    fail_number("DETAIL frame=", sizeof("DETAIL frame=") - 1, active_frame);
    TerminateProcess(GetCurrentProcess(), 86);
}

static LONG CALLBACK own_handler(EXCEPTION_POINTERS *event) {
    const unsigned index = active_frame;
    if (index > depth || event->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT ||
        (uintptr_t)PC(event->ContextRecord) != (uintptr_t)frames[index].trap) {
        fatal_context(event); return EXCEPTION_CONTINUE_SEARCH;
    }
    struct frame *frame = &frames[index];
    if ((unsigned)AX(event->ContextRecord) != frame->tag || math_result != 2 ||
        (uintptr_t)CX(event->ContextRecord) != (uintptr_t)&math_result) failures |= 1;
    if (warming) {
        ++frame->warm_traps;
        PC(event->ContextRecord) = (uintptr_t)(frame->trap + 1);
    } else {
        if (!index) original_sp = (uintptr_t)SP(event->ContextRecord);
        if ((uintptr_t)SP(event->ContextRecord) != original_sp) failures |= 2;
        ++frame->entries;
        if (index < depth) {
            ++frame->redirects;
            active_frame = index + 1;
            // All allocations and byte publication predate this callback.
            // The next function inherits the original valid stack/argument.
            PC(event->ContextRecord) = (uintptr_t)frames[index + 1].entry;
        } else {
            PC(event->ContextRecord) = (uintptr_t)(frame->trap + 1);
        }
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

static size_t make_block(unsigned char *bytes, unsigned pad, unsigned tag, size_t *trap_offset) {
    size_t n = 0;
#ifndef _WIN64
    const unsigned char argument[] = {0x8b,0x4c,0x24,0x04};
    memcpy(bytes + n, argument, sizeof(argument)); n += sizeof(argument);
#endif
    memset(bytes + n, 0x90, pad); n += pad;
    // Canonical math result stored before any exception; no live x87 value
    // deliberately crosses exception delivery in this transfer oracle.
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

static int invoke(unsigned index) {
    typedef int (__cdecl *own_function)(volatile LONG *);
    active_frame = index; math_result = -1;
    return ((own_function)(void *)frames[index].entry)(&math_result);
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
    printf("BEGIN case=context-redirect bits=%u publication=%s children=%s depth=%u rounds=%u\n",
           (unsigned)(sizeof(void *) * 8), names[publication], argv[2], depth, rounds);
    error_handle = GetStdHandle(STD_ERROR_HANDLE);
    for (unsigned i = 0; i <= depth; ++i) {
        frames[i].code = VirtualAlloc(NULL, info.dwPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!frames[i].code) return 87;
    }
    void *handler = AddVectoredExceptionHandler(1, own_handler);
    if (!handler) return 87;
    const ULONGLONG deadline = GetTickCount64() + 15000;
    unsigned completed = 0, root_calls = 0;
    for (unsigned round = 0; round < rounds; ++round) {
        for (unsigned i = 0; i <= depth; ++i)
            if (!publish(&frames[i], i, round, (enum publication)publication, info.dwPageSize)) return 87;
        if (warm_children) {
            warming = 1;
            for (unsigned i = 1; i <= depth; ++i) {
                const unsigned previous = frames[i].warm_traps;
                if (invoke(i) != (int)frames[i].tag || math_result != 2 ||
                    frames[i].warm_traps != previous + 1) failures |= 4;
                ++frames[i].warm_calls;
            }
            warming = 0;
        }
        original_sp = 0;
        const int got = invoke(0); ++root_calls;
        if (got != (int)frames[depth].tag || math_result != 2) failures |= 8;
        for (unsigned i = 0; i <= depth; ++i) {
            if (frames[i].entries != round + 1 ||
                frames[i].redirects != (i < depth ? round + 1 : 0)) failures |= 16;
        }
        if (failures) break;
        ++completed;
        if (GetTickCount64() > deadline) { failures |= 32; break; }
    }
    for (unsigned i = 0; i <= depth; ++i) {
        struct frame *frame = &frames[i];
        const unsigned expected_warm = warm_children && i ? completed : 0;
        printf("OBS frame=%u publications=%u entries=%u redirects=%u warm-calls=%u warm-traps=%u\n",
               i, frame->publications, frame->entries, frame->redirects, frame->warm_calls, frame->warm_traps);
        if (frame->entries != completed || frame->redirects != (i < depth ? completed : 0) ||
            frame->warm_calls != expected_warm || frame->warm_traps != expected_warm) failures |= 64;
        if (!VirtualFree(frame->code, 0, MEM_RELEASE)) return 87;
    }
    if (!RemoveVectoredExceptionHandler(handler)) return 87;
    printf("END case=context-redirect completed=%u root-calls=%u final-math=%ld failure-mask=%u status=%s\n",
           completed, root_calls, math_result, failures, !failures && completed == rounds ? "PASS" : "FAIL");
    return !failures && completed == rounds ? 0 : 1;
}
