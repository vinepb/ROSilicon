/* Bounded OWN code/memory/context test. No game, anti-cheat or remote writes.
 * Independent workers publish a complete inactive block, flush it, then call
 * it. No worker modifies code another worker can execute. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifdef _WIN64
#define CONTEXT_PC(c) ((c)->Rip)
#else
#define CONTEXT_PC(c) ((c)->Eip)
#endif

enum mode { WARM, WX, RWX, WPM, CROSS_WPM };
struct worker {
    unsigned id, rounds, completed;
    enum mode mode;
    size_t page_size;
    unsigned char *allocation, *entry, *trap, *alias;
    volatile LONG math_value;
    unsigned traps;
    unsigned unsupported_aliases;
    int use_alias, expect_alias, keep_for_child;
    DWORD error;
};
static DWORD worker_tls;
static HANDLE start_event;

static LONG CALLBACK own_exception(EXCEPTION_POINTERS *event) {
    struct worker *worker = TlsGetValue(worker_tls);
    const uintptr_t pc = (uintptr_t)CONTEXT_PC(event->ContextRecord);
    if (worker && worker->use_alias &&
        event->ExceptionRecord->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION &&
        pc == (uintptr_t)worker->alias) {
        ++worker->unsupported_aliases;
        // Expected negative decoder control on OUR DC D8 bytes only. The
        // next call's FNINIT resets the otherwise unused test FPU state.
        CONTEXT_PC(event->ContextRecord) = (uintptr_t)(worker->alias + 2);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (worker == NULL || event->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT ||
        pc != (uintptr_t)worker->trap) {
        fprintf(stderr, "FAIL exception=0x%lx pc=%p expected=%p worker=%u\n",
                event->ExceptionRecord->ExceptionCode, (void *)pc,
                worker ? (void *)worker->trap : NULL, worker ? worker->id : 999U);
        fflush(stderr);
        ExitProcess(86);
    }
    ++worker->traps;
    CONTEXT_PC(event->ContextRecord) = (uintptr_t)(worker->trap + 1);
    return EXCEPTION_CONTINUE_EXECUTION;
}

static size_t block(unsigned char *bytes, unsigned pad, unsigned tag, size_t *trap_offset,
                    int use_alias, size_t *alias_offset) {
    size_t n = 0;
#ifndef _WIN64
    /* cdecl's first argument is on the stack; Win64 already has it in RCX. */
    const unsigned char argument[] = {0x8b, 0x4c, 0x24, 0x04}; // MOV ECX,[ESP+4]
    memcpy(bytes + n, argument, sizeof(argument)); n += sizeof(argument);
#endif
    memset(bytes + n, 0x90, pad); n += pad;
    *trap_offset = n; bytes[n++] = 0xcc;
    /* FNINIT; FLD1; FLD1; FADDP; FISTP dword [ECX/RCX]. Valid canonical
     * instructions only. Integer output must be 2 and the x87 stack empty. */
    const unsigned char math[] = {0xdb,0xe3, 0xd9,0xe8, 0xd9,0xe8,
                                 0xde,0xc1, 0xdb,0x19};
    memcpy(bytes + n, math, sizeof(math)); n += sizeof(math);
    *alias_offset = 0;
    if (use_alias) {
        bytes[n++] = 0xd9; bytes[n++] = 0xe8; // FLD1
        *alias_offset = n;
        bytes[n++] = 0xdc; bytes[n++] = 0xd8; // FCOMP ST(0) alias
    }
    bytes[n++] = 0xb8; memcpy(bytes + n, &tag, 4); n += 4;
    bytes[n++] = 0xc3;
    return n;
}

static int publish(struct worker *worker, const unsigned char *bytes, size_t n) {
    DWORD previous;
    if (worker->mode == WPM || worker->mode == CROSS_WPM) {
        SIZE_T written = 0;
        if (!WriteProcessMemory(GetCurrentProcess(), worker->entry, bytes, n, &written) ||
            written != n) return 0;
    } else {
        if (worker->mode != RWX &&
            !VirtualProtect(worker->allocation, worker->page_size * 2, PAGE_READWRITE, &previous))
            return 0;
        memcpy(worker->entry, bytes, n);
        if (worker->mode != RWX &&
            !VirtualProtect(worker->allocation, worker->page_size * 2, PAGE_EXECUTE_READ, &previous))
            return 0;
    }
    return FlushInstructionCache(GetCurrentProcess(), worker->entry, n) != 0;
}

static DWORD WINAPI own_worker(void *parameter) {
    struct worker *worker = parameter;
    if (!TlsSetValue(worker_tls, worker)) { worker->error = 87; return 87; }
    const DWORD initial = worker->mode == RWX ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
    worker->allocation = VirtualAlloc(NULL, worker->page_size * 2,
                                     MEM_RESERVE | MEM_COMMIT, initial);
    if (worker->allocation == NULL) { worker->error = 87; return 87; }
    worker->entry = worker->allocation + (worker->mode == CROSS_WPM ? worker->page_size - 64 : 32);
    DWORD previous;
    if (worker->mode == WPM || worker->mode == CROSS_WPM) {
        if (!VirtualProtect(worker->allocation, worker->page_size, PAGE_EXECUTE_READ, &previous) ||
            !VirtualProtect(worker->allocation + worker->page_size, worker->page_size,
                worker->mode == CROSS_WPM ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ, &previous)) {
            worker->error = 87; goto done;
        }
    }
    if (WaitForSingleObject(start_event, 5000) != WAIT_OBJECT_0) {
        worker->error = 88; goto done;
    }
    const ULONGLONG deadline = GetTickCount64() + 15000;
    unsigned char bytes[160];
    typedef int (__cdecl *own_function)(volatile LONG *);
    for (unsigned round = 0; round < worker->rounds; ++round) {
        const unsigned layout_round = worker->mode == WARM ? 0 : round;
        const unsigned pad = worker->mode == WARM ? 31 : (layout_round * 13 + worker->id * 7) % 96;
        const unsigned tag = worker->id * 100000 + layout_round * 31 + 5;
        if (round == 0 || worker->mode != WARM) {
            size_t trap_offset, alias_offset;
            const size_t n = block(bytes, pad, tag, &trap_offset, worker->use_alias, &alias_offset);
            if (n > sizeof(bytes) || !publish(worker, bytes, n)) { worker->error = 87; break; }
            worker->trap = worker->entry + trap_offset;
            worker->alias = worker->use_alias ? worker->entry + alias_offset : NULL;
        }
        worker->math_value = -1;
        const int got = ((own_function)(void *)worker->entry)(&worker->math_value);
        const unsigned expected_unsupported = worker->use_alias && !worker->expect_alias ? round + 1 : 0;
        if (got != (int)tag || worker->math_value != 2 || worker->traps != round + 1 ||
            worker->unsupported_aliases != expected_unsupported) {
            fprintf(stderr, "FAIL worker=%u round=%u tag=%d expected=%u math=%ld traps=%u\n",
                    worker->id, round, got, tag, worker->math_value, worker->traps);
            worker->error = 1; break;
        }
        ++worker->completed;
        if (GetTickCount64() > deadline) { worker->error = 88; break; }
    }
done:
    if (!worker->keep_for_child) {
        if (!VirtualFree(worker->allocation, 0, MEM_RELEASE)) worker->error = 87;
        worker->allocation = NULL;
    }
    return worker->error;
}

static int number(const char *text, unsigned low, unsigned high, unsigned *result) {
    char *end;
    const unsigned long value = strtoul(text, &end, 10);
    if (!*text || *end || value < low || value > high) return 0;
    *result = (unsigned)value; return 1;
}

static int own_child(unsigned selected, unsigned count, unsigned rounds, unsigned depth,
                     unsigned expected_alias) {
    static const wchar_t *names[] = {L"warm", L"wx", L"rwx", L"wpm", L"cross-wpm"};
    wchar_t executable[32768], command[33024];
    const DWORD length = GetModuleFileNameW(NULL, executable, 32768);
    if (!length || length >= 32768) return 87;
    if (swprintf(command, 33024, L"\"%ls\" %ls %u %u %u %u", executable,
                 names[selected], count, rounds, depth - 1, expected_alias) < 0) return 87;
    STARTUPINFOW startup = {0}; startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    if (!SetHandleInformation(startup.hStdOutput, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) ||
        !SetHandleInformation(startup.hStdError, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) return 87;
    PROCESS_INFORMATION child = {0};
    // Exact own executable; no shell, external target or remote memory write.
    if (!CreateProcessW(executable, command, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &child)) return 87;
    printf("OBS chain-parent=%lu child=%lu remaining-depth=%u\n",
           GetCurrentProcessId(), child.dwProcessId, depth - 1);
    const DWORD wait = WaitForSingleObject(child.hProcess, 20000);
    DWORD exit_code = 88;
    if (wait != WAIT_OBJECT_0) {
        // Only the exact child process created above; the harness also stops
        // this inventoried private prefix if a translated child remains stuck.
        TerminateProcess(child.hProcess, 88);
        WaitForSingleObject(child.hProcess, 2000);
    } else if (!GetExitCodeProcess(child.hProcess, &exit_code)) exit_code = 87;
    if (!CloseHandle(child.hThread) || !CloseHandle(child.hProcess)) return 87;
    return (int)exit_code;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    static const char *names[] = {"warm", "wx", "rwx", "wpm", "cross-wpm"};
    unsigned count, rounds, selected = 5, depth = 0, expected_alias = 0;
    if ((argc != 4 && argc != 6) || !number(argv[2], 1, 4, &count) ||
        !number(argv[3], 1, 4096, &rounds) || (argc == 6 &&
        (!number(argv[4], 0, 2, &depth) || !number(argv[5], 0, 1, &expected_alias)))) return 2;
    for (unsigned i = 0; i < 5; ++i) if (!strcmp(argv[1], names[i])) selected = i;
    if (selected == 5) return 2;
    SYSTEM_INFO system_info; GetSystemInfo(&system_info);
    printf("BEGIN case=cold-block mode=%s bits=%u threads=%u rounds=%u\n",
           names[selected], (unsigned)(sizeof(void *) * 8), count, rounds);
    printf("OBS process=%lu chain-depth=%u alias-probe=%d expect-alias=%u\n",
           GetCurrentProcessId(), depth, argc == 6, expected_alias);
    worker_tls = TlsAlloc();
    if (worker_tls == TLS_OUT_OF_INDEXES) return 87;
    void *handler = AddVectoredExceptionHandler(1, own_exception);
    start_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (handler == NULL || start_event == NULL) return 87;
    struct worker workers[4] = {0};
    HANDLE threads[4];
    for (unsigned i = 0; i < count; ++i) {
        workers[i].id = i; workers[i].rounds = rounds; workers[i].mode = (enum mode)selected;
        workers[i].page_size = system_info.dwPageSize;
        workers[i].use_alias = argc == 6; workers[i].expect_alias = (int)expected_alias;
        workers[i].keep_for_child = depth > 0;
        threads[i] = CreateThread(NULL, 0, own_worker, &workers[i], 0, NULL);
        if (threads[i] == NULL) return 87;
    }
    if (!SetEvent(start_event) || WaitForMultipleObjects(count, threads, TRUE, 20000) != WAIT_OBJECT_0)
        return 88;
    int failed = 0;
    for (unsigned i = 0; i < count; ++i) {
        DWORD exit_code;
        if (!GetExitCodeThread(threads[i], &exit_code) || !CloseHandle(threads[i])) return 87;
        printf("OBS worker=%u completed=%u traps=%u math=%ld error=%lu exit=%lu alias-sigills=%u\n",
               i, workers[i].completed, workers[i].traps, workers[i].math_value,
               workers[i].error, exit_code, workers[i].unsupported_aliases);
        if (exit_code || workers[i].error || workers[i].completed != rounds || workers[i].traps != rounds)
            failed = 1;
    }
    if (!failed && depth > 0 && own_child(selected, count, rounds, depth, expected_alias)) failed = 1;
    for (unsigned i = 0; i < count; ++i) if (workers[i].allocation) {
        if (!VirtualFree(workers[i].allocation, 0, MEM_RELEASE)) failed = 1;
        workers[i].allocation = NULL;
    }
    if (!RemoveVectoredExceptionHandler(handler) || !CloseHandle(start_event) || !TlsFree(worker_tls))
        return 87;
    printf("END case=cold-block mode=%s status=%s\n", names[selected], failed ? "FAIL" : "PASS");
    return failed;
}
