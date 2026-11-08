/* Owned-code decoder fixture. No game, debugger, external-process memory or
 * protection-component access. Negative controls explicitly report unsupported
 * encodings; the runner checks those separately from successful substitution.
 */
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

static unsigned char *code, *entry, *decode_site, *trap_site, *return_site;
static unsigned char *guarded_data, *decode_next;
static size_t page_size;
static volatile int illegal_count, trap_count, fault_count, step_count, bad_pc, bad_fault;
static int restore_trap;
static int partial;
static int recover_memory_fault, single_step;
static unsigned char restore_byte;
static uintptr_t observed_pc, observed_fault;
static DWORD fault_flags;

static void flush(void *address, size_t size)
{
    if (!FlushInstructionCache(GetCurrentProcess(), address, size)) ExitProcess(87);
}

static LONG CALLBACK handler(EXCEPTION_POINTERS *exception)
{
    uintptr_t pc = (uintptr_t)PC(exception->ContextRecord);
    if (pc < (uintptr_t)code || pc >= (uintptr_t)(code + page_size))
        return EXCEPTION_CONTINUE_SEARCH;
    observed_pc = pc;
    DWORD kind = exception->ExceptionRecord->ExceptionCode;
    if (kind == EXCEPTION_ILLEGAL_INSTRUCTION) {
        ++illegal_count;
        if (pc != (uintptr_t)decode_site) bad_pc = 1;
        AX(exception->ContextRecord) = 0xdec0de;
        PC(exception->ContextRecord) = (uintptr_t)return_site;
        exception->ContextRecord->EFlags &= ~0x100u;
    } else if (kind == EXCEPTION_BREAKPOINT) {
        ++trap_count;
        if (pc != (uintptr_t)trap_site) bad_pc = 1;
        if (restore_trap) {
            *trap_site = restore_byte;
            flush(trap_site, 1);
            PC(exception->ContextRecord) = (uintptr_t)trap_site;
        } else {
            PC(exception->ContextRecord) = (uintptr_t)(trap_site + 1);
        }
    } else if (kind == EXCEPTION_SINGLE_STEP && single_step) {
        ++step_count;
        if (pc != (uintptr_t)decode_next) bad_pc = 1;
        exception->ContextRecord->EFlags &= ~0x100u;
    } else if (kind == EXCEPTION_ACCESS_VIOLATION && recover_memory_fault) {
        ++fault_count;
        if (pc != (uintptr_t)decode_site) bad_pc = 1;
        fault_flags = exception->ContextRecord->EFlags;
        observed_fault = exception->ExceptionRecord->ExceptionInformation[1];
        if (exception->ExceptionRecord->NumberParameters < 2
            || observed_fault != (uintptr_t)guarded_data) bad_fault = 1;
        DWORD previous;
        if (fault_count != 1 || !VirtualProtect(guarded_data, page_size, PAGE_READWRITE, &previous))
            ExitProcess(88);
        /* Wine supplies a fault context without TF even for stock ADD. Ask
         * explicitly to single-step the retried instruction; do not mislabel
         * that shared context behavior as a decoder-hook mapping defect. */
        if (single_step) exception->ContextRecord->EFlags |= 0x100u;
        /* Resume the same instruction, now with only our operand page readable. */
    } else {
        ++fault_count;
        if (pc != (uintptr_t)decode_site) bad_pc = 1;
        AX(exception->ContextRecord) = 0xbad;
        PC(exception->ContextRecord) = (uintptr_t)return_site;
        exception->ContextRecord->EFlags &= ~0x100u;
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void append_u32(unsigned char *bytes, size_t *size, uint32_t value)
{
    memcpy(bytes + *size, &value, sizeof(value));
    *size += sizeof(value);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc != 2) return 2;
    const char *name = argv[1];
    int arpl = strncmp(name, "arpl-", 5) == 0;
    int fcomp = strncmp(name, "fcomp-", 6) == 0;
    if ((!arpl && !fcomp) || (arpl && sizeof(void *) != 4)) return 2;
    int overlap = strstr(name, "overlap") != NULL;
    int guard = strstr(name, "guard") != NULL;
    int memory = strstr(name, "memory") != NULL;
    recover_memory_fault = strstr(name, "memory-fault") != NULL;
    single_step = strstr(name, "step") != NULL;
    partial = strstr(name, "partial") != NULL;
    int opcode_trap = strstr(name, "opcode") != NULL;
    int warm = strstr(name, "wpm") != NULL || strstr(name, "direct") != NULL;
    int direct = strstr(name, "direct") != NULL;
    int canonical = strstr(name, "canonical") != NULL;
    /* Warm cases insert their breakpoint only after the first execution. */
    int breakpoint = !warm && (strstr(name, "int3") != NULL || overlap);
    SYSTEM_INFO system;
    GetSystemInfo(&system);
    page_size = system.dwPageSize;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    code = VirtualAlloc(NULL, 2 * page_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    unsigned char *data = VirtualAlloc(NULL, 2 * page_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!code || !data) return 87;
    guarded_data = data + page_size;
    uint16_t *selector = (uint16_t *)(void *)(recover_memory_fault ? guarded_data
                                            : guarded_data - (canonical ? 4 : 2));
    *selector = 4;
    DWORD previous;
    if (!VirtualProtect(code + page_size, page_size, PAGE_NOACCESS, &previous)
        || !VirtualProtect(data + page_size, page_size, PAGE_NOACCESS, &previous)) return 87;
    unsigned char bytes[64];
    size_t size = 0, decode_offset = 0, trap_offset = 0;
    int expected = arpl ? 7 : 0x4000;
    if (overlap) expected = 4;
    if (arpl) {
        bytes[size++] = 0xb8; append_u32(bytes, &size, 4); /* mov eax,4 */
        bytes[size++] = 0xba; append_u32(bytes, &size, 3); /* mov edx,3 */
        if (single_step) {
            bytes[size++] = 0x9c; /* pushfd */
            bytes[size++] = 0x81; bytes[size++] = 0x0c; bytes[size++] = 0x24;
            append_u32(bytes, &size, 0x100); /* or dword [esp],TF */
            bytes[size++] = 0x9d; /* popfd; the next instruction is single-stepped */
        }
        if (overlap) {
            bytes[size++] = 0xeb; bytes[size++] = 1; /* jump into second byte */
            decode_offset = size;
            bytes[size++] = canonical ? 0x01 : 0x63;
            trap_offset = size; bytes[size++] = 0xcc;
        } else {
            decode_offset = size;
            bytes[size++] = canonical ? 0x01 : 0x63;
            if (!partial || memory) bytes[size++] = memory ? 0x15 : 0xd0;
            if (partial && memory) {
                bytes[size++] = 0x12; /* incomplete disp32 at page edge */
            } else if (memory) {
                append_u32(bytes, &size, (uint32_t)(uintptr_t)selector);
                bytes[size++] = 0x0f; bytes[size++] = 0xb7; bytes[size++] = 0x05;
                append_u32(bytes, &size, (uint32_t)(uintptr_t)selector);
            }
        }
    } else {
        if (overlap) {
            bytes[size++] = 0xb8; append_u32(bytes, &size, 4);
        }
        bytes[size++] = 0xdb; bytes[size++] = 0xe3; /* fninit */
        bytes[size++] = 0xd9; bytes[size++] = 0xe8; /* fld1 */
        if (overlap) { bytes[size++] = 0xeb; bytes[size++] = 1; }
        decode_offset = size;
        bytes[size++] = canonical ? 0xd8 : 0xdc;
        if (!partial) bytes[size++] = 0xd8;
        if (overlap) {
            bytes[size++] = 0xd8; /* actual target D8 D8, overlapping DC D8 */
            trap_offset = size; bytes[size++] = 0xcc;
        } else if (!partial) {
            bytes[size++] = 0xdf; bytes[size++] = 0xe0; /* fnstsw ax */
            bytes[size++] = 0x0f; bytes[size++] = 0xb7; bytes[size++] = 0xc0;
        }
    }
    if (breakpoint && !overlap) { trap_offset = size; bytes[size++] = 0xcc; }
    if (warm) trap_offset = opcode_trap ? decode_offset : size;
    size_t return_offset = size;
    if (!partial) bytes[size++] = 0xc3;
    entry = guard ? code + page_size - size : code + 32;
    memcpy(entry, bytes, size);
    decode_site = entry + decode_offset;
    decode_next = decode_site + (memory ? 6 : 2);
    trap_site = entry + trap_offset;
    return_site = partial ? code + 16 : entry + return_offset;
    if (partial) *return_site = 0xc3; /* safe exception-recovery landing */
    if (!VirtualProtect(code, page_size, PAGE_EXECUTE_READWRITE, &previous)) return 87;
    flush(entry, size);
    PVOID cookie = AddVectoredExceptionHandler(1, handler);
    if (!cookie) return 87;
    printf("BEGIN case=%s bits=%u entry=%p decode=%p return=%p guard=%d\n",
           name, (unsigned)(sizeof(void *) * 8), (void *)entry, (void *)decode_site,
           (void *)return_site, guard);
    int value = ((int (*)(void))(void *)entry)();
    if (warm && illegal_count == 0 && fault_count == 0) {
        printf("WARM value=%d expected=%d\n", value, expected);
        if (value != expected) return 1;
        restore_byte = *trap_site;
        restore_trap = 1;
        const unsigned char int3 = 0xcc;
        if (direct) *trap_site = int3;
        else {
            SIZE_T written = 0;
            if (!WriteProcessMemory(GetCurrentProcess(), trap_site, &int3, 1, &written)
                || written != 1) return 87;
        }
        flush(trap_site, 1);
        value = ((int (*)(void))(void *)entry)();
    }
    int unsupported = illegal_count > 0 || (partial && fault_count > 0);
    int expected_traps = breakpoint || warm ? 1 : 0;
    int ok = partial ? !bad_pc && !trap_count && illegal_count + fault_count == 1
                       && value == (illegal_count ? 0xdec0de : 0xbad)
        : !bad_fault && !bad_pc && (unsupported
        ? illegal_count == 1 && value == 0xdec0de && !overlap && !canonical
          && !fault_count && !step_count
        : value == expected && trap_count == expected_traps
          && fault_count == recover_memory_fault && step_count == single_step);
    printf("OBS support=%s value=%d expected=%d illegal=%d traps=%d faults=%d bad-pc=%d pc=%p"
           " steps=%d bad-fault=%d fault-address=%p fault-flags=%08lx\n",
           unsupported ? "unsupported" : "supported", value, expected, illegal_count,
           trap_count, fault_count, bad_pc, (void *)observed_pc, step_count, bad_fault,
           (void *)observed_fault, (unsigned long)fault_flags);
    RemoveVectoredExceptionHandler(cookie);
    VirtualFree(code, 0, MEM_RELEASE);
    VirtualFree(data, 0, MEM_RELEASE);
    printf("END case=%s status=%s\n", name, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
