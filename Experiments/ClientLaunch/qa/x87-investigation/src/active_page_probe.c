/* Own immutable, long-lived code under concurrent own-page cache/protection
 * operations. No guest breakpoint, exception handler, game or remote target. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct state { volatile LONG stop, ticks, math; };
_Static_assert(offsetof(struct state, stop) == 0, "stop offset");
_Static_assert(offsetof(struct state, ticks) == 4, "ticks offset");
_Static_assert(offsetof(struct state, math) == 8, "math offset");
struct consumer {
    unsigned char *entry;
    struct state state;
    int result;
    unsigned calls;
};
enum operation { IDLE, ACTIVE_FLUSH, NEIGHBOR_FLUSH, ACTIVE_PROTECT, NEIGHBOR_PROTECT,
                 RWX_IDLE, ACTIVE_WPM, NEIGHBOR_WPM };
static const char *names[] = {"idle", "active-flush", "neighbor-flush", "active-protect", "neighbor-protect",
                             "rwx-idle", "active-wpm", "neighbor-wpm"};
enum { TAG = 0x54321 };

static int query(const void *address, DWORD expected) {
    MEMORY_BASIC_INFORMATION info;
    return VirtualQuery(address, &info, sizeof(info)) == sizeof(info) &&
        info.State == MEM_COMMIT && info.Protect == expected;
}

static size_t make_code(unsigned char *bytes, int x87) {
    size_t n = 0;
#ifndef _WIN64
    const unsigned char argument[] = {0x8b, 0x4c, 0x24, 0x04};
    memcpy(bytes + n, argument, sizeof(argument)); n += sizeof(argument);
#endif
    const size_t loop = n;
    memset(bytes + n, 0x90, 32); n += 32;
    if (x87) {
        // FNINIT; FLD1; FLD1; FADDP; FISTP dword [ECX/RCX+8].
        const unsigned char math[] = {0xdb,0xe3,0xd9,0xe8,0xd9,0xe8,0xde,0xc1,0xdb,0x59,0x08};
        memcpy(bytes + n, math, sizeof(math)); n += sizeof(math);
    } else {
        // MOV EAX,1; ADD EAX,1; MOV [ECX/RCX+8],EAX.
        const unsigned char math[] = {0xb8,1,0,0,0,0x83,0xc0,1,0x89,0x41,8};
        memcpy(bytes + n, math, sizeof(math)); n += sizeof(math);
    }
    // LOCK INC dword [ECX/RCX+4]; CMP dword [ECX/RCX],0; JE loop.
    const unsigned char progress[] = {0xf0,0xff,0x41,4,0x83,0x39,0};
    memcpy(bytes + n, progress, sizeof(progress)); n += sizeof(progress);
    bytes[n++] = 0x0f; bytes[n++] = 0x84;
    const int32_t displacement = (int32_t)loop - (int32_t)(n + 4);
    memcpy(bytes + n, &displacement, 4); n += 4;
    bytes[n++] = 0xb8; const uint32_t tag = TAG;
    memcpy(bytes + n, &tag, 4); n += 4;
    bytes[n++] = 0xc3;
    return n;
}

static DWORD WINAPI consume(void *parameter) {
    struct consumer *consumer = parameter;
    typedef int (__cdecl *own_function)(struct state *);
    consumer->result = ((own_function)(void *)consumer->entry)(&consumer->state);
    ++consumer->calls;
    return consumer->result == TAG && consumer->state.math == 2 ? 0 : 86;
}

static int number(const char *text, unsigned *result) {
    char *end; const unsigned long got = strtoul(text, &end, 10);
    if (!*text || *end || got < 1 || got > 4096) return 0;
    *result = (unsigned)got; return 1;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    unsigned mode = 8, rounds;
    if (argc != 4 || !number(argv[3], &rounds) ||
        (strcmp(argv[2], "integer") && strcmp(argv[2], "x87"))) return 2;
    for (unsigned i = 0; i < 8; ++i) if (!strcmp(argv[1], names[i])) mode = i;
    if (mode == 8) return 2;
    const int x87 = !strcmp(argv[2], "x87");
    const int writes = mode == ACTIVE_WPM || mode == NEIGHBOR_WPM;
    const int protected = mode == ACTIVE_PROTECT || mode == NEIGHBOR_PROTECT;
    const DWORD base_protection = mode >= RWX_IDLE ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    SYSTEM_INFO info; GetSystemInfo(&info);
    const size_t page = info.dwPageSize, granularity = info.dwAllocationGranularity;
    if (page < 4096 || granularity < 65536 || granularity % page) return 87;
    printf("BEGIN case=active-page bits=%u mode=%s kind=%s rounds=%u page=%lu granularity=%lu code-protect=0x%lx\n",
           (unsigned)(sizeof(void *) * 8), names[mode], argv[2], rounds,
           info.dwPageSize, info.dwAllocationGranularity, base_protection);
    unsigned char *mapping = VirtualAlloc(NULL, granularity * 2,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    unsigned char *snapshot = malloc(granularity * 2);
    if (!mapping || !snapshot) return 87;
    unsigned char *neighbor = mapping + granularity;
    unsigned char bytes[128]; const size_t code_size = make_code(bytes, x87);
    if (code_size > sizeof(bytes) || (uintptr_t)mapping % granularity) return 87;
    struct consumer consumer = {0};
    consumer.entry = mapping + 32; consumer.state.math = -1;
    memcpy(consumer.entry, bytes, code_size);
    memcpy(snapshot, mapping, granularity * 2);
    DWORD previous;
    if (!VirtualProtect(mapping, page, base_protection, &previous) ||
        !VirtualProtect(neighbor, page, base_protection, &previous) ||
        !FlushInstructionCache(GetCurrentProcess(), mapping, page) ||
        !FlushInstructionCache(GetCurrentProcess(), neighbor, page)) return 87;
    if (!query(mapping, base_protection) || !query(neighbor, base_protection)) return 86;
    HANDLE worker = CreateThread(NULL, 0, consume, &consumer, 0, NULL);
    if (!worker) return 87;
    unsigned failures = 0, completed = 0, transitions = 0, flushes = 0, api_writes = 0, progress_rounds = 0;
    DWORD api_error = 0;
    const ULONGLONG deadline = GetTickCount64() + 15000;
    while (!InterlockedCompareExchange(&consumer.state.ticks, 0, 0)) {
        if (WaitForSingleObject(worker, 0) != WAIT_TIMEOUT) { failures |= 1; break; }
        if (GetTickCount64() >= deadline) { failures |= 2; break; }
        Sleep(1);
    }
    const LONG began = InterlockedCompareExchange(&consumer.state.ticks, 0, 0);
    unsigned char *target = mode == NEIGHBOR_FLUSH || mode == NEIGHBOR_PROTECT || mode == NEIGHBOR_WPM ? neighbor : mapping;
    for (unsigned round = 0; !failures && round < rounds; ++round) {
        const LONG before = InterlockedCompareExchange(&consumer.state.ticks, 0, 0);
        if (mode == IDLE || mode == RWX_IDLE) {
            SwitchToThread();
        } else if (mode == ACTIVE_FLUSH || mode == NEIGHBOR_FLUSH) {
            if (!FlushInstructionCache(GetCurrentProcess(), target, page)) {
                api_error = GetLastError(); failures |= 4; break;
            }
            ++flushes;
        } else if (writes) {
            // Copy only identical bytes from the immutable snapshot, through
            // our current-process handle. The Wine Apple path explicitly
            // invalidates executable translations after this API write.
            SIZE_T written = 0;
            const size_t offset = (size_t)(target - mapping) + 32;
            if (!WriteProcessMemory(GetCurrentProcess(), target + 32, snapshot + offset,
                                    code_size, &written) || written != code_size) {
                api_error = GetLastError(); failures |= 4; break;
            }
            ++api_writes;
            if (!FlushInstructionCache(GetCurrentProcess(), target, page)) {
                api_error = GetLastError(); failures |= 4; break;
            }
            ++flushes;
        } else {
            if (!VirtualProtect(target, page, PAGE_EXECUTE_READWRITE, &previous)) {
                api_error = GetLastError(); failures |= 4; break;
            }
            ++transitions;
            if (previous != PAGE_EXECUTE_READ || !query(target, PAGE_EXECUTE_READWRITE)) { failures |= 8; break; }
            if (!FlushInstructionCache(GetCurrentProcess(), target, page)) {
                api_error = GetLastError(); failures |= 4; break;
            }
            ++flushes;
            if (!VirtualProtect(target, page, PAGE_EXECUTE_READ, &previous)) {
                api_error = GetLastError(); failures |= 4; break;
            }
            ++transitions;
            if (previous != PAGE_EXECUTE_READWRITE) { failures |= 8; break; }
            if (!FlushInstructionCache(GetCurrentProcess(), target, page)) {
                api_error = GetLastError(); failures |= 4; break;
            }
            ++flushes;
        }
        const LONG after = InterlockedCompareExchange(&consumer.state.ticks, 0, 0);
        if (after != before) ++progress_rounds;
        if (memcmp(mapping, snapshot, page) || !query(mapping, base_protection) ||
            !query(neighbor, base_protection)) { failures |= 8; break; }
        if (WaitForSingleObject(worker, 0) != WAIT_TIMEOUT) { failures |= 1; break; }
        ++completed;
        if (GetTickCount64() >= deadline) { failures |= 2; break; }
        if (!(round % 16)) SwitchToThread();
    }
    const LONG ended = InterlockedCompareExchange(&consumer.state.ticks, 0, 0);
    InterlockedExchange(&consumer.state.stop, 1);
    const int joined = WaitForSingleObject(worker, 3000) == WAIT_OBJECT_0;
    if (!joined) {
        printf("END case=active-page completed=%u joined=0 failure-mask=%u status=FAIL\n", completed, failures | 16);
        return 88; // Process exit stops its own worker; never free live code.
    }
    DWORD worker_exit;
    if (!GetExitCodeThread(worker, &worker_exit) || !CloseHandle(worker)) return 87;
    const int unchanged = !memcmp(mapping, snapshot, granularity * 2);
    const int protections = query(mapping, base_protection) && query(neighbor, base_protection);
    const unsigned expected_transitions = protected ? 2 * rounds : 0;
    const unsigned expected_flushes = mode == IDLE || mode == RWX_IDLE ? 0 : protected ? 2 * rounds : rounds;
    if (worker_exit || consumer.calls != 1 || consumer.result != TAG || consumer.state.math != 2 ||
        !unchanged || !protections || completed != rounds || transitions != expected_transitions ||
        flushes != expected_flushes || api_writes != (writes ? rounds : 0)) failures |= 32;
    const int unverified = !began || ended == began || !progress_rounds;
    printf("OBS writer completed=%u transitions=%u flushes=%u api-writes=%u progress-rounds=%u ticks-before=%lu ticks-after=%lu bytes-unchanged=%u protections-ok=%u api-error=%lu\n",
        completed, transitions, flushes, api_writes, progress_rounds, (DWORD)began, (DWORD)ended,
        unchanged, protections, api_error);
    printf("OBS consumer calls=%u ticks=%lu math=%ld result=%d exit=%lu joined=%u\n",
        consumer.calls, (DWORD)consumer.state.ticks, consumer.state.math, consumer.result, worker_exit, joined);
    free(snapshot);
    if (!VirtualFree(mapping, 0, MEM_RELEASE)) return 87;
    printf("END case=active-page completed=%u joined=%u failure-mask=%u status=%s\n",
        completed, joined, failures, failures ? "FAIL" : unverified ? "UNVERIFIED" : "PASS");
    return failures ? 1 : unverified ? 89 : 0;
}
