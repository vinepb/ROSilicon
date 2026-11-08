/* Owned native fixture reusing the cached upstream 32-bit ARPL gate.
 * Only this process signals itself. No other process, game or installed prefix.
 */
#define main cached_decoder_main
#include "../source-v1.7.0/tests/test_decoder_arpl.c"
#undef main
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <sys/ucontext.h>

static _Atomic unsigned long completed;
static _Atomic int running;
static pthread_t victim;
static volatile sig_atomic_t delivered, gate_hits;
static int rewrite_context;

static void signal_handler(int sig, siginfo_t *info, void *context)
{
    (void)sig; (void)info;
    ++delivered;
    ucontext_t *uc = context;
    uintptr_t pc = uc->uc_mcontext->__ss.__rip;
    if (pc >= (uintptr_t)gate_stub && pc < (uintptr_t)gate_stub + 5) ++gate_hits;
    if (rewrite_context) uc->uc_mcontext->__ss.__rflags |= 2;
}

static void *signaller(void *unused)
{
    (void)unused;
    sigset_t mask;
    sigemptyset(&mask); sigaddset(&mask, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &mask, NULL);
    unsigned long previous = 0;
    while (atomic_load(&running)) {
        unsigned long now = atomic_load(&completed);
        if (now != previous) {
            previous = now;
            pthread_kill(victim, SIGUSR1);
        } else __asm__ volatile("pause");
    }
    return NULL;
}

static double seconds_now(void)
{
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return value.tv_sec + value.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc != 2 || (strcmp(argv[1], "none") && strcmp(argv[1], "plain")
                     && strcmp(argv[1], "modctx"))) return 2;
    printf("BEGIN case=arpl-signal-%s\n", argv[1]);
    install_sigill_guard();
    if (sigsetjmp(g_ill_env, 1)) {
        puts("OBS support=unsupported SIGILL");
        return 1;
    }
    setup_ldt();
    if (failures) return 87;
    check(0x1234BE04, 3); /* unsupported controls stop before any worker exists */
    if (failures) return 1;
    rewrite_context = !strcmp(argv[1], "modctx");
    struct sigaction action = {0};
    action.sa_sigaction = signal_handler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, NULL)) return 87;
    victim = pthread_self();
    pthread_t worker;
    int storm = strcmp(argv[1], "none") != 0;
    atomic_store(&running, 1);
    if (storm && pthread_create(&worker, NULL, signaller, NULL)) return 87;
    double until = seconds_now() + 1.0;
    unsigned long iterations = 0;
    do {
        uint32_t eax = 0x1234BE04u | (iterations & 3);
        uint32_t edx = (iterations >> 2) & 3;
        check(eax, edx);
        ++iterations;
        atomic_store(&completed, iterations);
        if (failures) break;
    } while ((iterations & 1023) || seconds_now() < until);
    atomic_store(&running, 0);
    if (storm) pthread_join(worker, NULL);
    sigset_t mask;
    sigemptyset(&mask); sigaddset(&mask, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &mask, NULL);
    int ok = !failures && (!storm || delivered >= 20);
    printf("OBS support=supported iterations=%lu signals=%d gate-hits=%d failures=%d\n",
           iterations, delivered, gate_hits, failures);
    printf("END case=arpl-signal-%s status=%s\n", argv[1], ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
