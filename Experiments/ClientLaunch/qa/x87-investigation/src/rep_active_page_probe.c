/* Owned cross-modification stress, NOT an x86 cache-ordering conformance test.
 * Only our allocation is written, through our current-process handle. Both
 * versions are valid equal-length streams. No guest trap or fault handler. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct state {
    volatile LONG stop, ticks, math, seen, errors;
    void *source, *destination;
    DWORD words;
};
_Static_assert(sizeof(void *) == 4, "This separate REP fixture is PE32 only");
_Static_assert(offsetof(struct state, seen) == 12, "seen offset");
_Static_assert(offsetof(struct state, errors) == 16, "errors offset");
_Static_assert(offsetof(struct state, source) == 20, "source offset");
_Static_assert(offsetof(struct state, destination) == 24, "destination offset");
_Static_assert(offsetof(struct state, words) == 28, "words offset");
struct consumer { unsigned char *entry; struct state state; int result; unsigned calls; };
enum { TAG = 0x54321 };
enum operation { IDLE, SAME_BYTE, CHANGED_BYTE, NEIGHBOR_CHANGED, MODE_COUNT };
static const char *names[] = {"idle", "same-byte", "changed-byte", "neighbor-changed"};

static int query(const void *address) {
    MEMORY_BASIC_INFORMATION info;
    return VirtualQuery(address, &info, sizeof(info)) == sizeof(info) &&
        info.State == MEM_COMMIT && info.Protect == PAGE_EXECUTE_READWRITE;
}

static void emit(unsigned char *bytes, size_t *n, const void *data, size_t size) {
    memcpy(bytes + *n, data, size); *n += size;
}

static size_t make_code(unsigned char *bytes, int x87, size_t *version_offset) {
    size_t n = 0;
#ifndef _WIN64
    const unsigned char argument[] = {0x8b,0x4c,0x24,4}; // MOV ECX,[ESP+4]
    emit(bytes, &n, argument, sizeof(argument));
#endif
    const size_t loop = n;
    memset(bytes + n, 0x90, 16); n += 16;
    // Only the low immediate byte of MOV EAX,1 is mutable (1 <-> 2).
    // Both values are independently checked before their observation is ORed.
    bytes[n++] = 0xb8; *version_offset = n;
    const uint32_t version = 1; emit(bytes, &n, &version, sizeof(version));
    const unsigned char observe[] = {
        0x83,0xf8,1, 0x74,9, 0x83,0xf8,2, 0x74,4,
        0xf0,0xff,0x41,16, // LOCK INC [ECX/RCX+16] on invalid version
        0xf0,0x09,0x41,12  // LOCK OR [ECX/RCX+12],EAX
    };
    emit(bytes, &n, observe, sizeof(observe));
    if (x87) {
        const unsigned char math[] = {0xdb,0xe3,0xd9,0xe8,0xd9,0xe8,0xde,0xc1,0xdb,0x59,8};
        emit(bytes, &n, math, sizeof(math)); // FNINIT; FLD1; FLD1; FADDP; FISTP [arg+8]
    } else {
        const unsigned char math[] = {0xb8,1,0,0,0,0x83,0xc0,1,0x89,0x41,8};
        emit(bytes, &n, math, sizeof(math));
    }
    // Preserve the Win32 nonvolatile ESI/EDI and our argument ECX; copy only
    // our stable source/destination. REP is one long x86 instruction whose
    // translated execution may be interrupted by own-page invalidation.
    const unsigned char copy[] = {
        0x56,0x57,0x51, 0x89,0xca, // PUSH ESI,EDI,ECX; MOV EDX,ECX
        0x8b,0x72,20, 0x8b,0x7a,24, 0x8b,0x4a,28,
        0xfc,0xf3,0xa5, // CLD; REP MOVSD
        0x59,0x5f,0x5e // POP ECX,EDI,ESI
    };
    emit(bytes, &n, copy, sizeof(copy));
    const unsigned char check_progress[] = {
        0x83,0x79,8,2, 0x74,4, 0xf0,0xff,0x41,16, // count every bad math result
        0xf0,0xff,0x41,4, 0x83,0x39,0 // LOCK INC ticks; CMP stop,0
    };
    emit(bytes, &n, check_progress, sizeof(check_progress));
    bytes[n++] = 0x0f; bytes[n++] = 0x84;
    const int32_t displacement = (int32_t)loop - (int32_t)(n + 4);
    emit(bytes, &n, &displacement, 4);
    bytes[n++] = 0xb8; const uint32_t tag = TAG; emit(bytes, &n, &tag, 4);
    bytes[n++] = 0xc3;
    return n;
}

static DWORD WINAPI consume(void *parameter) {
    struct consumer *c = parameter;
    typedef int (__cdecl *own_function)(struct state *);
    c->result = ((own_function)(void *)c->entry)(&c->state);
    ++c->calls;
    return c->result == TAG && c->state.math == 2 && !c->state.errors ? 0 : 86;
}

static LONG read_long(volatile LONG *value) { return InterlockedCompareExchange(value, 0, 0); }

static int wait_version(struct consumer *c, HANDLE worker, LONG version, ULONGLONG deadline) {
    while (!(read_long(&c->state.seen) & version)) {
        if (read_long(&c->state.errors) || GetTickCount64() >= deadline ||
            WaitForSingleObject(worker, 0) != WAIT_TIMEOUT) return 0;
        Sleep(1);
    }
    return 1;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc != 4 || (strcmp(argv[2], "integer") && strcmp(argv[2], "x87"))) return 2;
    char *end; const unsigned long parsed = strtoul(argv[3], &end, 10);
    if (!*argv[3] || *end || parsed < 1 || parsed > 512) return 2;
    const unsigned rounds = (unsigned)parsed;
    unsigned mode = MODE_COUNT;
    for (unsigned i = 0; i < MODE_COUNT; ++i) if (!strcmp(argv[1], names[i])) mode = i;
    if (mode == MODE_COUNT) return 2;
    SYSTEM_INFO info; GetSystemInfo(&info);
    const size_t page = info.dwPageSize, granularity = info.dwAllocationGranularity;
    if (page < 4096 || granularity < 65536 || granularity % page) return 87;
    // An explicit *free* hint pins the sampler to our code, not Wine startup.
    // VirtualAlloc must acquire it normally; occupied addresses are not replaced.
    unsigned char *mapping = VirtualAlloc((void *)(uintptr_t)0x25000000,
        granularity * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    unsigned char *expected = malloc(granularity * 2);
    if (!mapping || mapping != (void *)(uintptr_t)0x25000000 || !expected) return 87;
    unsigned char *neighbor = mapping + granularity;
    unsigned char bytes[160]; size_t version_offset;
    const size_t code_size = make_code(bytes, !strcmp(argv[2], "x87"), &version_offset);
    if (code_size > sizeof(bytes) || version_offset >= code_size) return 87;
    struct consumer c = {0}; c.entry = mapping + 32; c.state.math = -1;
    const size_t copy_bytes = 1024 * 1024;
    c.state.source = VirtualAlloc(NULL, copy_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    c.state.destination = VirtualAlloc(NULL, copy_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    unsigned char *source_snapshot = malloc(copy_bytes);
    if (!c.state.source || !c.state.destination || !source_snapshot) return 87;
    c.state.words = (DWORD)(copy_bytes / sizeof(uint32_t));
    for (DWORD i = 0; i < c.state.words; ++i) ((uint32_t *)c.state.source)[i] = i * UINT32_C(0x9e3779b9) ^ UINT32_C(0x5a123456);
    memset(c.state.destination, 0xa7, copy_bytes);
    memcpy(source_snapshot, c.state.source, copy_bytes);
    memcpy(c.entry, bytes, code_size);
    memcpy(expected, mapping, granularity * 2);
    DWORD previous;
    if (!VirtualProtect(mapping, page, PAGE_EXECUTE_READWRITE, &previous) ||
        !VirtualProtect(neighbor, page, PAGE_EXECUTE_READWRITE, &previous) ||
        !FlushInstructionCache(GetCurrentProcess(), mapping, page)) return 87;
    printf("BEGIN case=rep-active-page bits=%u mode=%s kind=%s rounds=%u entry=%p code-size=%u version-offset=%u\n",
        (unsigned)(sizeof(void *) * 8), names[mode], argv[2], rounds, c.entry,
        (unsigned)code_size, (unsigned)version_offset);
    printf("CODE "); for (size_t i = 0; i < code_size; ++i) printf("%02x", bytes[i]); printf("\n");
    HANDLE worker = CreateThread(NULL, 0, consume, &c, 0, NULL);
    if (!worker) return 87;
    unsigned failures = 0, completed = 0, writes = 0, changed = 0, flushes = 0, progress = 0, observed = 0;
    DWORD api_error = 0;
    const ULONGLONG deadline = GetTickCount64() + 12000;
    if (!wait_version(&c, worker, 1, GetTickCount64() + 1000)) failures |= 1;
    Sleep(600); // allow the existing sampler's discovery sweeps to choose our worker
    const LONG began = read_long(&c.state.ticks); observed |= (unsigned)read_long(&c.state.seen);
    for (unsigned i = 0; !failures && i < rounds; ++i) {
        const LONG before = read_long(&c.state.ticks);
        InterlockedExchange(&c.state.seen, 0);
        const unsigned char value = mode == SAME_BYTE ? 1 : (i % 2 ? 1 : 2);
        if (mode != IDLE) {
            unsigned char *target = mode == NEIGHBOR_CHANGED ? neighbor + 32 : c.entry + version_offset;
            const size_t offset = (size_t)(target - mapping);
            SIZE_T written = 0;
            if (!WriteProcessMemory(GetCurrentProcess(), target, &value, 1, &written) || written != 1) {
                api_error = GetLastError(); failures |= 2; break;
            }
            ++writes; changed += expected[offset] != value; expected[offset] = value;
            if (!FlushInstructionCache(GetCurrentProcess(), target, 1)) {
                api_error = GetLastError(); failures |= 2; break;
            }
            ++flushes;
        }
        const LONG wanted = mode == CHANGED_BYTE ? value : 1;
        if (!wait_version(&c, worker, wanted, GetTickCount64() + 1000)) { failures |= 4; break; }
        Sleep(1);
        observed |= (unsigned)read_long(&c.state.seen);
        if (read_long(&c.state.ticks) != before) ++progress;
        if (memcmp(mapping, expected, granularity * 2) || !query(mapping) || !query(neighbor) ||
            read_long(&c.state.errors) || WaitForSingleObject(worker, 0) != WAIT_TIMEOUT) { failures |= 8; break; }
        ++completed;
        if (GetTickCount64() >= deadline) { failures |= 16; break; }
    }
    const LONG ended = read_long(&c.state.ticks);
    Sleep(200);
    InterlockedExchange(&c.state.stop, 1);
    const int joined = WaitForSingleObject(worker, 3000) == WAIT_OBJECT_0;
    if (!joined) {
        printf("END case=rep-active-page failure-mask=%u joined=0 status=FAIL\n", failures | 32);
        return 88; // process exit, not freeing code while our worker is alive
    }
    DWORD worker_exit;
    if (!GetExitCodeThread(worker, &worker_exit) || !CloseHandle(worker)) return 87;
    const int bytes_ok = !memcmp(mapping, expected, granularity * 2);
    const int protections_ok = query(mapping) && query(neighbor);
    const int source_unchanged = !memcmp(c.state.source, source_snapshot, copy_bytes);
    const int destination_ok = !memcmp(c.state.destination, source_snapshot, copy_bytes);
    if (!source_unchanged || !destination_ok) failures |= 256;
    const unsigned expected_writes = mode == IDLE ? 0 : rounds;
    const unsigned expected_changed = mode == CHANGED_BYTE || mode == NEIGHBOR_CHANGED ? rounds : 0;
    if (worker_exit || c.calls != 1 || c.result != TAG || c.state.math != 2 || c.state.errors ||
        !bytes_ok || !protections_ok || completed != rounds || writes != expected_writes ||
        changed != expected_changed || flushes != expected_writes ||
        observed != (mode == CHANGED_BYTE ? 3u : 1u)) failures |= 64;
    if (!began || ended == began || !progress) failures |= 128;
    printf("OBS writer completed=%u writes=%u changed=%u flushes=%u progress=%u ticks-before=%lu ticks-after=%lu observed=%u bytes-ok=%u protections-ok=%u api-error=%lu\n",
        completed, writes, changed, flushes, progress, (DWORD)began, (DWORD)ended,
        observed, bytes_ok, protections_ok, api_error);
    printf("OBS consumer calls=%u math=%ld errors=%ld result=%d exit=%lu joined=%u\n",
        c.calls, c.state.math, c.state.errors, c.result, worker_exit, joined);
    printf("OBS copy bytes=%u source-unchanged=%u destination-ok=%u\n", (unsigned)copy_bytes, source_unchanged, destination_ok);
    free(expected);
    free(source_snapshot);
    if (!VirtualFree(c.state.source, 0, MEM_RELEASE) || !VirtualFree(c.state.destination, 0, MEM_RELEASE)) return 87;
    if (!VirtualFree(mapping, 0, MEM_RELEASE)) return 87;
    printf("END case=rep-active-page failure-mask=%u joined=%u status=%s\n", failures, joined, failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
