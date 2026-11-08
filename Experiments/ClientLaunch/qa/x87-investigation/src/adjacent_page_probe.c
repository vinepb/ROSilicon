/* OWN immutable code plus a concurrently protected UNUSED neighbor. No game,
 * remote memory, code edits during execution or protected-program handling. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN64
#define PC(c) ((c)->Rip)
#define AX(c) ((c)->Rax)
#else
#define PC(c) ((c)->Eip)
#define AX(c) ((c)->Eax)
#endif

struct fixture;
struct consumer {
    struct fixture *fixture;
    unsigned id, completed, traps, interleaved_calls;
    volatile LONG math;
    DWORD error;
};
struct fixture {
    unsigned count, rounds, cycles, mode, tail;
    size_t page, granularity;
    unsigned char *mapping, *neighbor, *entry, *trap;
    HANDLE begin, ready;
    volatile LONG active, operations;
    unsigned transitions, live_transitions;
    DWORD error;
};
static const char *modes[] = {"static-noaccess", "static-guard", "toggle-noaccess", "toggle-guard"};
static const char *tails[] = {"arpl", "fcomp"};
static DWORD tls;

static int query(const void *address, DWORD expected) {
    MEMORY_BASIC_INFORMATION info;
    return VirtualQuery(address, &info, sizeof(info)) == sizeof(info) &&
        info.State == MEM_COMMIT && info.Protect == expected;
}

static LONG CALLBACK own_exception(EXCEPTION_POINTERS *event) {
    struct consumer *consumer = TlsGetValue(tls);
    if (!consumer) return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t pc = (uintptr_t)PC(event->ContextRecord);
    const DWORD kind = event->ExceptionRecord->ExceptionCode;
    if (kind == EXCEPTION_BREAKPOINT && pc == (uintptr_t)consumer->fixture->trap &&
        (unsigned)AX(event->ContextRecord) == 0x54321U) {
        ++consumer->traps;
        PC(event->ContextRecord) = (uintptr_t)(consumer->fixture->trap + 1);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    // There is no executed instruction/data access in the changing page.
    // Never skip an illegal byte, recover an unrelated PC or patch a context.
    fprintf(stderr, "FAIL unexpected-exception=0x%lx pc=%p expected-trap=%p fault-address=%p\n",
        kind, (void *)pc, (void *)consumer->fixture->trap,
        event->ExceptionRecord->NumberParameters >= 2
            ? (void *)event->ExceptionRecord->ExceptionInformation[1] : NULL);
    fflush(stderr);
    ExitProcess(86);
}

static DWORD WINAPI consume(void *parameter) {
    struct consumer *consumer = parameter;
    struct fixture *fixture = consumer->fixture;
    if (!TlsSetValue(tls, consumer)) return 87;
    if (InterlockedIncrement(&fixture->active) == (LONG)fixture->count && !SetEvent(fixture->ready)) return 87;
    if (WaitForSingleObject(fixture->begin, 5000) != WAIT_OBJECT_0) return 88;
    typedef int (__cdecl *own_function)(volatile LONG *);
    const ULONGLONG deadline = GetTickCount64() + 15000;
    for (unsigned round = 0; round < fixture->rounds; ++round) {
        consumer->math = -1;
        const LONG before = InterlockedCompareExchange(&fixture->operations, 0, 0);
        const int got = ((own_function)(void *)fixture->entry)(&consumer->math);
        const LONG after = InterlockedCompareExchange(&fixture->operations, 0, 0);
        if (after != before) ++consumer->interleaved_calls;
        if (got != 0x54321 || consumer->math != 2 || consumer->traps != round + 1) {
            consumer->error = 1; break;
        }
        ++consumer->completed;
        if (GetTickCount64() > deadline) { consumer->error = 88; break; }
        if (!(round % 16)) SwitchToThread();
    }
    InterlockedDecrement(&fixture->active);
    return consumer->error;
}

static DWORD WINAPI protect_neighbor(void *parameter) {
    struct fixture *fixture = parameter;
    if (WaitForSingleObject(fixture->begin, 5000) != WAIT_OBJECT_0) return 88;
    const DWORD protected = fixture->mode % 2 ? PAGE_EXECUTE_READ | PAGE_GUARD : PAGE_NOACCESS;
    const ULONGLONG deadline = GetTickCount64() + 15000;
    for (unsigned cycle = 0; cycle < fixture->cycles; ++cycle) {
        if (fixture->mode >= 2) {
            DWORD previous;
            if (!VirtualProtect(fixture->neighbor, fixture->page, protected, &previous)) return 87;
            ++fixture->transitions;
            if (InterlockedCompareExchange(&fixture->active, 0, 0)) ++fixture->live_transitions;
            InterlockedIncrement(&fixture->operations);
        }
        if (!query(fixture->neighbor, protected) || !query(fixture->neighbor - 1, PAGE_EXECUTE_READ))
            return 86;
        if (fixture->mode >= 2) {
            DWORD previous;
            if (!VirtualProtect(fixture->neighbor, fixture->page, PAGE_EXECUTE_READ, &previous)) return 87;
            ++fixture->transitions;
            if (InterlockedCompareExchange(&fixture->active, 0, 0)) ++fixture->live_transitions;
            InterlockedIncrement(&fixture->operations);
        }
        if (GetTickCount64() > deadline) return 88;
        if (!(cycle % 16)) SwitchToThread();
    }
    return 0;
}

static int number(const char *text, unsigned low, unsigned high, unsigned *value) {
    char *end;
    const unsigned long got = strtoul(text, &end, 10);
    if (!*text || *end || got < low || got > high) return 0;
    *value = (unsigned)got; return 1;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    struct fixture fixture = {0};
    fixture.mode = 4; fixture.tail = 2;
    if (argc != 6 || !number(argv[3], 1, 4, &fixture.count) ||
        !number(argv[4], 1, 16384, &fixture.rounds) || !number(argv[5], 1, 16384, &fixture.cycles)) return 2;
    for (unsigned i = 0; i < 4; ++i) if (!strcmp(argv[1], modes[i])) fixture.mode = i;
    for (unsigned i = 0; i < 2; ++i) if (!strcmp(argv[2], tails[i])) fixture.tail = i;
    if (fixture.mode == 4 || fixture.tail == 2) return 2;
    SYSTEM_INFO info; GetSystemInfo(&info);
    fixture.page = info.dwPageSize; fixture.granularity = info.dwAllocationGranularity;
    if (fixture.page < 4096 || fixture.granularity < 65536 || fixture.granularity % fixture.page) return 87;
    fixture.mapping = VirtualAlloc(NULL, fixture.granularity * 2,
                                  MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!fixture.mapping || (uintptr_t)fixture.mapping % fixture.granularity) return 87;
    fixture.neighbor = fixture.mapping + fixture.granularity;
    unsigned char bytes[128]; size_t n = 0;
#ifndef _WIN64
    const unsigned char argument[] = {0x8b,0x4c,0x24,0x04};
    memcpy(bytes + n, argument, sizeof(argument)); n += sizeof(argument);
#endif
    memset(bytes + n, 0x90, 63); n += 63;
    bytes[n++] = 0xb8; const unsigned tag = 0x54321;
    memcpy(bytes + n, &tag, 4); n += 4;
    const unsigned char math[] = {0xdb,0xe3,0xd9,0xe8,0xd9,0xe8,0xde,0xc1,0xdb,0x19};
    memcpy(bytes + n, math, sizeof(math)); n += sizeof(math);
    const size_t trap_offset = n; bytes[n++] = 0xcc; bytes[n++] = 0xc3;
    fixture.entry = fixture.neighbor - n; fixture.trap = fixture.entry + trap_offset;
    memcpy(fixture.entry, bytes, n);
    // Own never-executed bytes only. Their rejected encodings may be seen by
    // speculative decode; no consumer enters them, no instruction is edited
    // during execution, and the fixture never masks their SIGILL outcome.
    const unsigned char arpl_tail[] = {0x63,0xd0,0xcc,0xc3};
    const unsigned char fcomp_tail[] = {0xdc,0xd8,0xcc,0xc3};
    for (size_t offset = 0; offset < fixture.page; offset += 4)
        memcpy(fixture.neighbor + offset, fixture.tail ? fcomp_tail : arpl_tail, 4);
    DWORD previous;
    const DWORD protected = fixture.mode % 2 ? PAGE_EXECUTE_READ | PAGE_GUARD : PAGE_NOACCESS;
    if (!VirtualProtect(fixture.neighbor - fixture.page, fixture.page, PAGE_EXECUTE_READ, &previous) ||
        !VirtualProtect(fixture.neighbor, fixture.page, protected, &previous) ||
        !FlushInstructionCache(GetCurrentProcess(), fixture.entry, n) ||
        !query(fixture.neighbor - 1, PAGE_EXECUTE_READ) || !query(fixture.neighbor, protected) ||
        !query(fixture.neighbor + fixture.page, PAGE_READWRITE)) return 87;
    printf("BEGIN case=adjacent-page mode=%s tail=%s bits=%u workers=%u rounds=%u cycles=%u page=%lu granularity=%lu\n",
           modes[fixture.mode], tails[fixture.tail], (unsigned)(sizeof(void *) * 8), fixture.count,
           fixture.rounds, fixture.cycles, info.dwPageSize, info.dwAllocationGranularity);
    tls = TlsAlloc(); if (tls == TLS_OUT_OF_INDEXES) return 87;
    void *handler = AddVectoredExceptionHandler(1, own_exception);
    fixture.begin = CreateEventA(NULL, TRUE, FALSE, NULL);
    fixture.ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!handler || !fixture.begin || !fixture.ready) return 87;
    struct consumer consumers[4] = {0}; HANDLE handles[5];
    for (unsigned i = 0; i < fixture.count; ++i) {
        consumers[i].fixture = &fixture; consumers[i].id = i;
        handles[i] = CreateThread(NULL, 0, consume, &consumers[i], 0, NULL);
        if (!handles[i]) return 87;
    }
    handles[fixture.count] = CreateThread(NULL, 0, protect_neighbor, &fixture, 0, NULL);
    if (!handles[fixture.count] || WaitForSingleObject(fixture.ready, 5000) != WAIT_OBJECT_0 ||
        !SetEvent(fixture.begin) ||
        WaitForMultipleObjects(fixture.count + 1, handles, TRUE, 20000) != WAIT_OBJECT_0) return 88;
    int failed = 0; unsigned interleaved = 0;
    for (unsigned i = 0; i <= fixture.count; ++i) {
        DWORD exit_code;
        if (!GetExitCodeThread(handles[i], &exit_code) || !CloseHandle(handles[i])) return 87;
        if (i == fixture.count) {
            fixture.error = exit_code;
            printf("OBS protector transitions=%u live-transitions=%u error=%lu\n",
                   fixture.transitions, fixture.live_transitions, fixture.error);
        } else {
            printf("OBS worker=%u completed=%u traps=%u interleaved-calls=%u math=%ld error=%lu exit=%lu\n",
                   i, consumers[i].completed, consumers[i].traps, consumers[i].interleaved_calls,
                   consumers[i].math, consumers[i].error, exit_code);
            interleaved += consumers[i].interleaved_calls;
            if (consumers[i].error || consumers[i].completed != fixture.rounds || consumers[i].traps != fixture.rounds)
                failed = 1;
        }
        if (exit_code) failed = 1;
    }
    const DWORD after = fixture.mode >= 2 ? PAGE_EXECUTE_READ : protected;
    if (!query(fixture.neighbor, after) || !query(fixture.neighbor - 1, PAGE_EXECUTE_READ)) failed = 1;
    const int unverified = fixture.mode >= 2 &&
        (!interleaved || !fixture.live_transitions || fixture.transitions != 2 * fixture.cycles);
    if (!RemoveVectoredExceptionHandler(handler) || !CloseHandle(fixture.begin) ||
        !CloseHandle(fixture.ready) || !TlsFree(tls) || !VirtualFree(fixture.mapping, 0, MEM_RELEASE)) return 87;
    printf("END case=adjacent-page mode=%s tail=%s status=%s\n", modes[fixture.mode], tails[fixture.tail],
           failed ? "FAIL" : unverified ? "UNVERIFIED" : "PASS");
    return failed ? 1 : unverified ? 89 : 0;
}
