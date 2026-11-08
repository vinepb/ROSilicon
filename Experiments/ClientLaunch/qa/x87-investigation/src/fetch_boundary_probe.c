/* OWN mapping/instruction-fetch recovery oracle. No game, remote memory,
 * runtime modification or protection component. Publish complete inactive
 * code, flush it, and touch only our adjacent page in the exact-address VEH. */
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

enum mode { DATA_NONE, DATA_GUARD, END_NONE, END_GUARD, FETCH_NONE, FETCH_GUARD };
static const char *names[] = {"data-noaccess", "data-guard", "end-noaccess",
                             "end-guard", "fetch-noaccess", "fetch-guard"};
struct worker {
    unsigned id, rounds, completed, faults, traps, reads, executes;
    unsigned round_faults, round_traps, tag;
    enum mode mode;
    size_t page, granularity;
    unsigned char *mapping, *boundary, *entry, *fault_site, *trap;
    volatile LONG math;
    uintptr_t last_pc, last_address;
    DWORD last_operation, error;
};
static DWORD tls_index;
static HANDLE begin_event;

static int is_guard(enum mode mode) { return mode == DATA_GUARD || mode == END_GUARD || mode == FETCH_GUARD; }
static int is_end(enum mode mode) { return mode == END_NONE || mode == END_GUARD; }
static int is_data(enum mode mode) { return mode == DATA_NONE || mode == DATA_GUARD; }

static int query(const void *address, DWORD expected) {
    MEMORY_BASIC_INFORMATION info;
    return VirtualQuery(address, &info, sizeof(info)) == sizeof(info) &&
        info.State == MEM_COMMIT && info.Protect == expected;
}

static LONG CALLBACK own_exception(EXCEPTION_POINTERS *event) {
    struct worker *worker = TlsGetValue(tls_index);
    const uintptr_t pc = (uintptr_t)PC(event->ContextRecord);
    const DWORD kind = event->ExceptionRecord->ExceptionCode;
    if (!worker || !worker->entry) return EXCEPTION_CONTINUE_SEARCH;
    if (kind == EXCEPTION_BREAKPOINT && pc == (uintptr_t)worker->trap) {
        if (++worker->round_traps != 1 || (unsigned)AX(event->ContextRecord) != worker->tag)
            ExitProcess(86);
        ++worker->traps;
        PC(event->ContextRecord) = (uintptr_t)(worker->trap + 1);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    const DWORD expected = is_guard(worker->mode) ? STATUS_GUARD_PAGE_VIOLATION : EXCEPTION_ACCESS_VIOLATION;
    if (!is_end(worker->mode) && kind == expected && pc == (uintptr_t)worker->fault_site &&
        event->ExceptionRecord->NumberParameters >= 2) {
        const uintptr_t address = event->ExceptionRecord->ExceptionInformation[1];
        const uintptr_t operation = event->ExceptionRecord->ExceptionInformation[0];
        // Record native execute=8 versus a decoder read=0. Neither may be a
        // write; the data-read control requires read=0. Do not invent Windows
        // conformance from this explicitly bounded Wine compatibility oracle.
        const uintptr_t wanted = (uintptr_t)worker->boundary + (is_data(worker->mode) ? 37 : 0);
        worker->last_pc = pc; worker->last_address = address;
        worker->last_operation = (DWORD)operation;
        if (++worker->round_faults != 1 || address != wanted ||
            (operation != 0 && operation != 8) || (is_data(worker->mode) && operation != 0)) {
            worker->error = 86; return EXCEPTION_CONTINUE_SEARCH;
        }
        if (is_guard(worker->mode) && !query(worker->boundary, PAGE_EXECUTE_READ)) {
            worker->error = 86; return EXCEPTION_CONTINUE_SEARCH;
        }
        DWORD previous;
        if (!VirtualProtect(worker->boundary, worker->page, PAGE_EXECUTE_READ, &previous))
            ExitProcess(87);
        ++worker->faults;
        if (operation == 8) ++worker->executes; else ++worker->reads;
        // Retry the same OWN instruction, unchanged. No code patch or fake
        // return context is used to make the test continue.
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    fprintf(stderr, "FAIL unexpected-exception=0x%lx pc=%p expected-pc=%p address=%p worker=%u\n",
            kind, (void *)pc, (void *)worker->fault_site,
            event->ExceptionRecord->NumberParameters >= 2
                ? (void *)event->ExceptionRecord->ExceptionInformation[1] : NULL, worker->id);
    fflush(stderr);
    ExitProcess(86);
}

static size_t make_block(unsigned char *bytes, unsigned pad, unsigned tag,
                         enum mode mode, uintptr_t data_address,
                         size_t *fault_offset, size_t *trap_offset) {
    size_t n = 0;
#ifndef _WIN64
    const unsigned char argument[] = {0x8b, 0x4c, 0x24, 0x04}; // ECX = cdecl first argument
    memcpy(bytes + n, argument, sizeof(argument)); n += sizeof(argument);
#endif
    memset(bytes + n, 0x90, pad); n += pad;
    *fault_offset = n;
    if (is_data(mode)) {
        bytes[n++] = 0xa0; // MOV AL, [absolute OWN adjacent data address]
        memcpy(bytes + n, &data_address, sizeof(data_address)); n += sizeof(data_address);
    }
    if (!is_data(mode)) *fault_offset = n;
    bytes[n++] = 0xb8; memcpy(bytes + n, &tag, sizeof(tag)); n += sizeof(tag);
    const unsigned char math[] = {0xdb,0xe3, 0xd9,0xe8, 0xd9,0xe8, 0xde,0xc1, 0xdb,0x19};
    memcpy(bytes + n, math, sizeof(math)); n += sizeof(math);
    *trap_offset = n; bytes[n++] = 0xcc; bytes[n++] = 0xc3;
    return n;
}

static int publish(struct worker *worker, const unsigned char *bytes, size_t length) {
    DWORD previous;
    if (!VirtualProtect(worker->mapping, worker->granularity * 2, PAGE_READWRITE, &previous)) return 0;
    memcpy(worker->entry, bytes, length);
    if (is_data(worker->mode)) worker->boundary[37] = 0x5a;
    if (!VirtualProtect(worker->boundary - worker->page, worker->page, PAGE_EXECUTE_READ, &previous))
        return 0;
    const DWORD right_protection = is_guard(worker->mode)
        ? PAGE_EXECUTE_READ | PAGE_GUARD : PAGE_NOACCESS;
    if (!VirtualProtect(worker->boundary, worker->page, right_protection, &previous)) return 0;
    if (!query(worker->boundary - 1, PAGE_EXECUTE_READ) ||
        !query(worker->boundary, right_protection) ||
        !query(worker->boundary + worker->page, PAGE_READWRITE)) return 0;
    return FlushInstructionCache(GetCurrentProcess(), worker->entry, length) != 0;
}

static DWORD WINAPI run_worker(void *argument) {
    struct worker *worker = argument;
    if (!TlsSetValue(tls_index, worker)) return 87;
    worker->mapping = VirtualAlloc(NULL, worker->granularity * 2,
                                  MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!worker->mapping || (uintptr_t)worker->mapping % worker->granularity) return 87;
    worker->boundary = worker->mapping + worker->granularity;
    if (WaitForSingleObject(begin_event, 5000) != WAIT_OBJECT_0) { worker->error = 88; goto done; }
    const ULONGLONG deadline = GetTickCount64() + 15000;
    typedef int (__cdecl *own_function)(volatile LONG *);
    for (unsigned round = 0; round < worker->rounds; ++round) {
        unsigned char bytes[128];
        size_t fault_offset, trap_offset;
        worker->tag = worker->id * 100000 + round * 31 + 5;
        const size_t length = make_block(bytes, (round * 13 + worker->id * 7) % 48,
            worker->tag, worker->mode, (uintptr_t)worker->boundary + 37, &fault_offset, &trap_offset);
        if (length > sizeof(bytes) || length >= worker->page) { worker->error = 87; break; }
        // Complete function ends exactly before the protected page; fetch mode
        // puts MOV's opcode in the last byte and its immediate across the edge.
        worker->entry = worker->boundary - (is_end(worker->mode) ? length :
            is_data(worker->mode) ? length + 64 : fault_offset + 1);
        worker->fault_site = worker->entry + fault_offset;
        worker->trap = worker->entry + trap_offset;
        worker->round_faults = 0; worker->round_traps = 0; worker->math = -1;
        if (!publish(worker, bytes, length)) { worker->error = 87; break; }
        const int got = ((own_function)(void *)worker->entry)(&worker->math);
        const DWORD after = is_end(worker->mode) ? (is_guard(worker->mode)
            ? PAGE_EXECUTE_READ | PAGE_GUARD : PAGE_NOACCESS) : PAGE_EXECUTE_READ;
        if (got != (int)worker->tag || worker->math != 2 || worker->round_traps != 1 ||
            worker->round_faults != (unsigned)!is_end(worker->mode) || !query(worker->boundary, after)) {
            fprintf(stderr, "FAIL worker=%u round=%u got=%d expected=%u math=%ld faults=%u traps=%u\n",
                    worker->id, round, got, worker->tag, worker->math,
                    worker->round_faults, worker->round_traps);
            worker->error = 1; break;
        }
        ++worker->completed;
        if (GetTickCount64() > deadline) { worker->error = 88; break; }
    }
done:
    if (!VirtualFree(worker->mapping, 0, MEM_RELEASE)) worker->error = 87;
    worker->mapping = NULL; worker->entry = NULL;
    return worker->error;
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
    unsigned threads, rounds, mode = 6;
    if (argc != 4 || !number(argv[2], 1, 4, &threads) || !number(argv[3], 1, 4096, &rounds)) return 2;
    for (unsigned i = 0; i < 6; ++i) if (!strcmp(argv[1], names[i])) mode = i;
    if (mode == 6) return 2;
    SYSTEM_INFO info; GetSystemInfo(&info);
    if (info.dwPageSize < 4096 || info.dwAllocationGranularity < 65536 ||
        info.dwAllocationGranularity % info.dwPageSize) return 87;
    printf("BEGIN case=fetch-boundary mode=%s bits=%u workers=%u rounds=%u page=%lu granularity=%lu\n",
           names[mode], (unsigned)(sizeof(void *) * 8), threads, rounds,
           info.dwPageSize, info.dwAllocationGranularity);
    tls_index = TlsAlloc();
    if (tls_index == TLS_OUT_OF_INDEXES) return 87;
    void *handler = AddVectoredExceptionHandler(1, own_exception);
    begin_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!handler || !begin_event) return 87;
    struct worker workers[4] = {0}; HANDLE handles[4];
    for (unsigned i = 0; i < threads; ++i) {
        workers[i].id = i; workers[i].mode = (enum mode)mode; workers[i].rounds = rounds;
        workers[i].page = info.dwPageSize; workers[i].granularity = info.dwAllocationGranularity;
        handles[i] = CreateThread(NULL, 0, run_worker, &workers[i], 0, NULL);
        if (!handles[i]) return 87;
    }
    if (!SetEvent(begin_event) || WaitForMultipleObjects(threads, handles, TRUE, 20000) != WAIT_OBJECT_0) return 88;
    int failed = 0;
    for (unsigned i = 0; i < threads; ++i) {
        DWORD exit_code;
        if (!GetExitCodeThread(handles[i], &exit_code) || !CloseHandle(handles[i])) return 87;
        printf("OBS worker=%u completed=%u faults=%u traps=%u reads=%u executes=%u pc=%p address=%p operation=%lu error=%lu exit=%lu\n",
               i, workers[i].completed, workers[i].faults, workers[i].traps, workers[i].reads,
               workers[i].executes, (void *)workers[i].last_pc, (void *)workers[i].last_address,
               workers[i].last_operation, workers[i].error, exit_code);
        if (exit_code || workers[i].error || workers[i].completed != rounds || workers[i].traps != rounds ||
            workers[i].faults != (is_end((enum mode)mode) ? 0 : rounds)) failed = 1;
    }
    if (!RemoveVectoredExceptionHandler(handler) || !CloseHandle(begin_event) || !TlsFree(tls_index)) return 87;
    printf("END case=fetch-boundary mode=%s status=%s\n", names[mode], failed ? "FAIL" : "PASS");
    return failed;
}
