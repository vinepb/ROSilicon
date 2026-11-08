/* OWN native x86 code/context fixture. Reuses upstream's voluntary handshake
 * and signal-pacing/same-content context technique, combining it with cold
 * main-thread publication. No other process, game or installed Wine prefix. */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>
#include <libkern/OSCacheControl.h>

_Static_assert(ATOMIC_INT_LOCK_FREE == 2 && ATOMIC_LONG_LOCK_FREE == 2,
               "Handler counters/address access must be lock-free");
static _Atomic uintptr_t code_begin, code_end, expected_trap, expected_alias;
static _Atomic unsigned expected_tag, phase, progress, delivered, code_hits;
static _Atomic unsigned publish_hits, invoke_hits, traps, negative_aliases;
static _Atomic int running, producer_error;
static pthread_t victim;
static unsigned rewrite_context, alias_probe, alias_supported, breakpoint_probe = 1;

static void report_number(const char *label, size_t length, uintptr_t value) {
    static const char digits[] = "0123456789abcdef";
    char buffer[sizeof(value) * 2 + 1];
    for (unsigned i = 0; i < sizeof(value) * 2; ++i)
        buffer[i] = digits[(value >> ((sizeof(value) * 2 - i - 1) * 4)) & 15];
    buffer[sizeof(value) * 2] = '\n';
    (void)write(STDERR_FILENO, label, length);
    (void)write(STDERR_FILENO, buffer, sizeof(buffer));
}

static _STRUCT_X86_FLOAT_STATE64 *float_state(ucontext_t *context) {
    // Same native/full-layout distinction as the pinned upstream context test.
    if (context->uc_mcsize == sizeof(_STRUCT_MCONTEXT64_FULL) ||
        context->uc_mcsize == sizeof(_STRUCT_MCONTEXT_AVX64_FULL) ||
        context->uc_mcsize == sizeof(_STRUCT_MCONTEXT_AVX512_64_FULL))
        return &((_STRUCT_MCONTEXT64_FULL *)context->uc_mcontext)->__fs;
    return &context->uc_mcontext->__fs;
}

static void own_signal(int signal, siginfo_t *info, void *raw) {
    (void)info;
    const int saved_errno = errno;
    ucontext_t *context = raw;
    const uintptr_t pc = context->uc_mcontext->__ss.__rip;
    if (signal == SIGUSR1) {
        atomic_fetch_add_explicit(&delivered, 1, memory_order_relaxed);
        const unsigned state = atomic_load_explicit(&phase, memory_order_relaxed);
        if (state == 1) atomic_fetch_add_explicit(&publish_hits, 1, memory_order_relaxed);
        if (state == 2) atomic_fetch_add_explicit(&invoke_hits, 1, memory_order_relaxed);
        if (pc >= atomic_load_explicit(&code_begin, memory_order_relaxed) &&
            pc < atomic_load_explicit(&code_end, memory_order_relaxed))
            atomic_fetch_add_explicit(&code_hits, 1, memory_order_relaxed);
        if (rewrite_context) {
            context->uc_mcontext->__ss.__rflags |= 2; // already-set x86 reserved bit
            if (context->uc_mcsize >= sizeof(*context->uc_mcontext)) {
                volatile unsigned char *state_bytes = (volatile unsigned char *)float_state(context);
                // Same bytes written back: exercise the restore path without
                // changing intended floating-point state or using handler memcpy.
                for (size_t i = 0; i < sizeof(_STRUCT_X86_FLOAT_STATE64); ++i) {
                    const unsigned char byte = state_bytes[i];
                    state_bytes[i] = byte;
                }
            }
        }
    } else if (signal == SIGTRAP && breakpoint_probe && pc == atomic_load_explicit(&expected_trap, memory_order_relaxed) + 1 &&
               (unsigned)context->uc_mcontext->__ss.__rax == atomic_load_explicit(&expected_tag, memory_order_relaxed)) {
        // Darwin saves RIP after our exact INT3. No context rewrite needed.
        atomic_fetch_add_explicit(&traps, 1, memory_order_relaxed);
    } else if (signal == SIGILL && alias_probe && !alias_supported &&
               pc == atomic_load_explicit(&expected_alias, memory_order_relaxed)) {
        // Explicit negative test on OUR DC D8 only, not a game workaround.
        // Stored math is already verified; next call FNINIT resets unused state.
        atomic_fetch_add_explicit(&negative_aliases, 1, memory_order_relaxed);
        context->uc_mcontext->__ss.__rip = pc + 2;
    } else {
        static const char message[] = "FAIL unexpected own signal/context\n";
        (void)write(STDERR_FILENO, message, sizeof(message) - 1);
        report_number("DETAIL signal=", sizeof("DETAIL signal=") - 1, (uintptr_t)signal);
        report_number("DETAIL si-code=", sizeof("DETAIL si-code=") - 1, (uintptr_t)info->si_code);
        report_number("DETAIL pc=", sizeof("DETAIL pc=") - 1, pc);
        report_number("DETAIL trap=", sizeof("DETAIL trap=") - 1, atomic_load(&expected_trap));
        report_number("DETAIL rax=", sizeof("DETAIL rax=") - 1, context->uc_mcontext->__ss.__rax);
        report_number("DETAIL tag=", sizeof("DETAIL tag=") - 1, atomic_load(&expected_tag));
        report_number("DETAIL flags=", sizeof("DETAIL flags=") - 1, context->uc_mcontext->__ss.__rflags);
        report_number("DETAIL phase=", sizeof("DETAIL phase=") - 1, atomic_load(&phase));
        report_number("DETAIL traps=", sizeof("DETAIL traps=") - 1, atomic_load(&traps));
        _exit(86);
    }
    errno = saved_errno;
}

static void *send_signals(void *unused) {
    (void)unused;
    sigset_t block; sigemptyset(&block); sigaddset(&block, SIGUSR1);
    if (pthread_sigmask(SIG_BLOCK, &block, NULL)) { atomic_store(&producer_error, 87); return NULL; }
    unsigned previous = 0;
    while (atomic_load_explicit(&running, memory_order_relaxed)) {
        const unsigned current = atomic_load_explicit(&progress, memory_order_relaxed);
        if (current != previous || atomic_load_explicit(&phase, memory_order_relaxed) == 2) {
            previous = current;
            const int error = pthread_kill(victim, SIGUSR1);
            if (error) { atomic_store(&producer_error, error); break; }
        } else __asm__ volatile("pause");
        // Cached upstream context tests use this bounded producer delay.
        // Continue sending while invoking, not just before compilation starts;
        // otherwise cold code may never be observed by a saved signal PC.
        const struct timespec delay = {0, 20000};
        if (nanosleep(&delay, NULL)) { atomic_store(&producer_error, errno); break; }
    }
    return NULL;
}

static double now(void) {
    struct timespec stamp;
    if (clock_gettime(CLOCK_MONOTONIC, &stamp)) _exit(87);
    return (double)stamp.tv_sec + (double)stamp.tv_nsec * 1e-9;
}

static size_t block(unsigned char *bytes, unsigned pad, unsigned tag, unsigned spin,
                    size_t *trap_offset, size_t *alias_offset) {
    size_t n = 0;
    memset(bytes, 0x90, pad); n += pad;
    const unsigned char load[] = {0xdb,0xe3,0xd9,0xe8,0xd9,0xe8}; // FNINIT; FLD1; FLD1
    memcpy(bytes + n, load, sizeof(load)); n += sizeof(load);
    bytes[n++] = 0xb9; memcpy(bytes + n, &spin, 4); n += 4; // ECX = bounded spin
    const unsigned char loop[] = {0xff,0xc9,0x75,0xfc}; // DEC ECX; JNZ back
    memcpy(bytes + n, loop, sizeof(loop)); n += sizeof(loop);
    const unsigned char store[] = {0xde,0xc1,0xdb,0x1f}; // FADDP; FISTP dword [RDI]
    memcpy(bytes + n, store, sizeof(store)); n += sizeof(store);
    bytes[n++] = 0xb8; memcpy(bytes + n, &tag, 4); n += 4;
    *alias_offset = 0;
    if (alias_probe) {
        bytes[n++] = 0xd9; bytes[n++] = 0xe8; // FLD1
        *alias_offset = n; bytes[n++] = 0xdc; bytes[n++] = 0xd8;
    }
    *trap_offset = n;
    if (breakpoint_probe) bytes[n++] = 0xcc;
    bytes[n++] = 0xc3;
    return n;
}

static int number(const char *text, unsigned low, unsigned high, unsigned *result) {
    char *end; const unsigned long value = strtoul(text, &end, 10);
    if (!*text || *end || value < low || value > high) return 0;
    *result = (unsigned)value; return 1;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    static const char *modes[] = {"hot", "rewrite", "remap"};
    static const char *signals[] = {"none", "plain", "modctx"};
    unsigned mode = 3, signal_mode = 3, rounds, spin;
    if ((argc != 7 && argc != 8) || !number(argv[3], 0, 1, &alias_probe) || !number(argv[4], 0, 1, &alias_supported) ||
        !number(argv[5], 1, 8192, &rounds) || !number(argv[6], 64, 65536, &spin) ||
        (argc == 8 && !number(argv[7], 0, 1, &breakpoint_probe))) return 2;
    for (unsigned i = 0; i < 3; ++i) {
        if (!strcmp(argv[1], modes[i])) mode = i;
        if (!strcmp(argv[2], signals[i])) signal_mode = i;
    }
    if (mode == 3 || signal_mode == 3) return 2;
    rewrite_context = signal_mode == 2;
    const size_t page = (size_t)getpagesize();
    printf("BEGIN case=cold-signal mode=%s signals=%s alias=%u expect-alias=%u rounds=%u spin=%u page=%zu breakpoints=%u\n",
           modes[mode], signals[signal_mode], alias_probe, alias_supported, rounds, spin, page, breakpoint_probe);
    struct sigaction action = {0};
    action.sa_sigaction = own_signal; action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask); sigaddset(&action.sa_mask, SIGUSR1);
    for (unsigned i = 0; i < 3; ++i) {
        static const int caught[] = {SIGUSR1, SIGTRAP, SIGILL};
        if (sigaction(caught[i], &action, NULL)) return 87;
    }
    victim = pthread_self();
    pthread_t producer;
    atomic_store(&running, 1);
    if (signal_mode && pthread_create(&producer, NULL, send_signals, NULL)) return 87;
    unsigned char *mapping = NULL;
    unsigned completed = 0, publications = 0, entry_changes = 0;
    uintptr_t old_entry = 0;
    const double deadline = now() + 10.0;
    int failed = 0;
    typedef int (*own_function)(volatile int *);
    for (unsigned round = 0; round < rounds; ++round) {
        const unsigned layout = mode ? round : 0;
        const unsigned tag = layout * 31 + 101;
        if (!round || mode) {
            atomic_store(&phase, 1); atomic_fetch_add(&progress, 1);
            if (!mapping) {
                mapping = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
                if (mapping == MAP_FAILED) { mapping = NULL; failed = 87; break; }
            } else if (mprotect(mapping, page, PROT_READ | PROT_WRITE)) { failed = 87; break; }
            unsigned char bytes[160]; size_t trap_offset, alias_offset;
            const size_t length = block(bytes, (layout * 13) % 96, tag, spin, &trap_offset, &alias_offset);
            if (length > sizeof(bytes) || length + 32 > page) { failed = 87; break; }
            unsigned char *entry = mapping + 32;
            memcpy(entry, bytes, length);
            atomic_store(&expected_trap, (uintptr_t)entry + trap_offset);
            atomic_store(&expected_alias, alias_probe ? (uintptr_t)entry + alias_offset : 0);
            atomic_store(&expected_tag, tag);
            atomic_store(&code_begin, (uintptr_t)entry); atomic_store(&code_end, (uintptr_t)entry + length);
            if (old_entry && old_entry != (uintptr_t)entry) ++entry_changes;
            old_entry = (uintptr_t)entry;
            if (mprotect(mapping, page, PROT_READ | PROT_EXEC)) { failed = 87; break; }
            sys_icache_invalidate(mapping, page); ++publications;
        }
        volatile int math = -1;
        atomic_store(&phase, 2); atomic_fetch_add(&progress, 1);
        const int got = ((own_function)(void *)(mapping + 32))(&math);
        atomic_store(&phase, 0);
        const unsigned negative = alias_probe && !alias_supported ? round + 1 : 0;
        if (got != (int)tag || math != 2 || atomic_load(&traps) != (breakpoint_probe ? round + 1 : 0) ||
            atomic_load(&negative_aliases) != negative) {
            printf("FAIL round=%u tag=%d expected=%u math=%d traps=%u alias-sigills=%u\n",
                   round, got, tag, math, atomic_load(&traps), atomic_load(&negative_aliases));
            failed = 1; break;
        }
        ++completed;
        if (mode == 2) {
            if (munmap(mapping, page)) { failed = 87; break; }
            mapping = NULL; atomic_store(&code_begin, 0); atomic_store(&code_end, 0);
        }
        if (now() > deadline) { failed = 88; break; }
    }
    atomic_store(&running, 0);
    if (signal_mode && pthread_join(producer, NULL)) failed = 87;
    // No producer remains; block pending delivery before cleanup/observations.
    sigset_t block_usr; sigemptyset(&block_usr); sigaddset(&block_usr, SIGUSR1);
    if (pthread_sigmask(SIG_BLOCK, &block_usr, NULL)) failed = 87;
    if (mapping && munmap(mapping, page)) failed = 87;
    if (atomic_load(&producer_error)) failed = 87;
    const int unverified = signal_mode &&
        (atomic_load(&delivered) < 20 || atomic_load(&code_hits) == 0 || atomic_load(&invoke_hits) == 0);
    printf("OBS completed=%u publications=%u traps=%u alias-sigills=%u signals=%u code-hits=%u publish-hits=%u invoke-hits=%u entry-changes=%u producer-error=%d\n",
           completed, publications, atomic_load(&traps), atomic_load(&negative_aliases),
           atomic_load(&delivered), atomic_load(&code_hits), atomic_load(&publish_hits),
           atomic_load(&invoke_hits), entry_changes, atomic_load(&producer_error));
    printf("END case=cold-signal mode=%s signals=%s status=%s\n", modes[mode], signals[signal_mode],
           failed ? "FAIL" : unverified ? "UNVERIFIED" : "PASS");
    return failed ? failed : unverified ? 89 : 0;
}
