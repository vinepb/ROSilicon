/* Execute emitted wrappers only on OWN mock decoder and code mappings.
 * Simulated revocation demonstrates an extra-read hazard, not the game's
 * Rosetta assertion or the actual proprietary decoder's full contract. */
#include "stub_asm.hpp"
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#include <libkern/OSCacheControl.h>

auto opcode_internal_to_host(uint16_t value) -> uint16_t { return value; }

struct MockContext {
    uint8_t cpuMode;
    uint8_t padding[7];
    const uint8_t *codeBase, *codeEnd, *insnStart, *cursor;
    void *ownedPage;
    size_t pageSize;
    int status;
    bool revoke;
    uint8_t length, observedByte;
};
struct MockOutput { uint8_t length; uint8_t decoded[128]; };
static_assert(offsetof(MockContext, codeBase) == stub_asm::kDecoderCtxCodeBase);
static_assert(offsetof(MockContext, codeEnd) == stub_asm::kDecoderCtxCodeEnd);
static_assert(offsetof(MockContext, insnStart) == stub_asm::kDecoderCtxInsnStart);
static_assert(offsetof(MockContext, cursor) == stub_asm::kDecoderCtxCursor);

extern "C" __attribute__((noinline)) int owned_mock_decode(
    MockContext *ctx, uint32_t offset, void *, uint8_t *length) {
    *length = ctx->length;
    ctx->observedByte = static_cast<const volatile uint8_t *>(ctx->codeBase)[offset];
    ctx->insnStart = ctx->codeBase + offset;
    ctx->cursor = ctx->insnStart + ctx->length;
    // Model mapping revocation at the after-decode boundary deterministically.
    // No concurrency, external process, runtime image or game is accessed.
    if (ctx->revoke && mprotect(ctx->ownedPage, ctx->pageSize, PROT_NONE) != 0)
        _exit(90);
    return ctx->status;
}

static sigjmp_buf trapReturn;
static volatile sig_atomic_t armed = 0, caughtSignal = 0;
static std::atomic<uintptr_t> caughtAddress{0};
static_assert(std::atomic<uintptr_t>::is_always_lock_free);

static void owned_fault(int signal, siginfo_t *info, void *) {
    if (!armed) _exit(128 + signal);
    caughtSignal = signal;
    caughtAddress.store(reinterpret_cast<uintptr_t>(info->si_addr), std::memory_order_relaxed);
    siglongjmp(trapReturn, 1);
}

static void word(uint8_t *where, uint32_t value) {
    std::memcpy(where, &value, sizeof(value));
}

static void own_absolute_jump(uint8_t *where, uintptr_t target) {
    assert((target >> 48) == 0);
    word(where, 0xd2800000U | ((target & 0xffffU) << 5) | 17U);
    word(where + 4, 0xf2800000U | (1U << 21) | (((target >> 16) & 0xffffU) << 5) | 17U);
    word(where + 8, 0xf2800000U | (2U << 21) | (((target >> 32) & 0xffffU) << 5) | 17U);
    word(where + 12, 0xd61f0000U | (17U << 5)); // BR x17, retains caller LR
}

enum class Style { Plain, RelativeTail, WrapperNoAliases, PureWrapper };
static const char *style_name(Style style) {
    switch (style) {
        case Style::Plain: return "plain";
        case Style::RelativeTail: return "relative-tail";
        case Style::WrapperNoAliases: return "wrapper-no-aliases";
        case Style::PureWrapper: return "pure-wrapper";
    }
    std::abort();
}

static void check(Style style, int status, uint8_t length, bool revoke) {
    const size_t page = static_cast<size_t>(getpagesize());
    auto *code = static_cast<uint8_t *>(mmap(nullptr, page, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANON, -1, 0));
    auto *guest = static_cast<uint8_t *>(mmap(nullptr, page, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANON, -1, 0));
    assert(code != MAP_FAILED && guest != MAP_FAILED && page >= 4096);
    guest[0] = 0x63;
    uint8_t prologue[16];
    for (int i = 0; i < 4; ++i) word(prologue + i * 4, 0xd503201fU);
    auto *entry = code + 256, *handler = code + 1024;
    std::memcpy(entry, prologue, 16);
    own_absolute_jump(entry + 16, reinterpret_cast<uintptr_t>(&owned_mock_decode));
    if (style != Style::Plain) {
        auto blob = stub_asm::buildDecodeHook(reinterpret_cast<uintptr_t>(handler),
            reinterpret_cast<uintptr_t>(entry), prologue, 0x777, false, false, true,
            style == Style::RelativeTail, style == Style::PureWrapper);
        assert(blob.entry.size() == 16 && blob.handler.size() == 360);
        std::memcpy(entry, blob.entry.data(), 16);
        std::memcpy(handler, blob.handler.data(), blob.handler.size());
    }
    assert(mprotect(code, page, PROT_READ | PROT_EXEC) == 0);
    sys_icache_invalidate(code, page);
    // Heap data stays defined across siglongjmp; changed non-volatile automatic
    // locals in this function would otherwise have indeterminate values.
    auto *ctx = new MockContext{1, {}, guest, guest + page, nullptr, nullptr,
                               guest, page, status, revoke, length, 0};
    auto *output = new MockOutput{255, {}};
    using Decoder = int (*)(MockContext *, uint32_t, void *, uint8_t *);
    volatile int result = -1;
    caughtSignal = 0;
    caughtAddress.store(0, std::memory_order_relaxed);
    if (sigsetjmp(trapReturn, 1) == 0) {
        armed = 1;
        result = reinterpret_cast<Decoder>(entry)(ctx, 0, output->decoded, &output->length);
    }
    armed = 0;
    const bool expectedFault = style == Style::WrapperNoAliases &&
        status == 1 && length == 0 && revoke;
    const bool faultInOwnPage = caughtAddress.load(std::memory_order_relaxed) >=
        reinterpret_cast<uintptr_t>(guest) &&
        caughtAddress.load(std::memory_order_relaxed) < reinterpret_cast<uintptr_t>(guest + page);
    const bool ok = ctx->observedByte == 0x63 && output->length == length &&
        (expectedFault ? (caughtSignal == SIGSEGV || caughtSignal == SIGBUS) && faultInOwnPage
                       : caughtSignal == 0 && result == status);
    std::printf("OBS style=%s decode-status=%d length=%u revoke=%s signal=%d "
                "fault-in-owned-page=%s expected-extra-read=%s status=%s\n",
        style_name(style), status, length, revoke ? "yes" : "no", caughtSignal,
        faultInOwnPage ? "yes" : "no", expectedFault ? "yes" : "no", ok ? "PASS" : "FAIL");
    assert(ok);
    delete output;
    delete ctx;
    assert(munmap(guest, page) == 0 && munmap(code, page) == 0);
}

int main() {
    struct sigaction action{};
    action.sa_sigaction = owned_fault;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    struct sigaction oldSegv{}, oldBus{};
    assert(sigaction(SIGSEGV, &action, &oldSegv) == 0);
    assert(sigaction(SIGBUS, &action, &oldBus) == 0);
    for (Style style : {Style::Plain, Style::RelativeTail,
                        Style::WrapperNoAliases, Style::PureWrapper}) {
        check(style, 0, 1, false);
        check(style, 1, 0, false);
        check(style, 2, 0, false);
        check(style, 0, 1, true);
        check(style, 1, 0, true);
    }
    assert(sigaction(SIGSEGV, &oldSegv, nullptr) == 0);
    assert(sigaction(SIGBUS, &oldBus, nullptr) == 0);
    std::puts("END case=owned-decoder-return-contract checks=20 status=PASS");
    return 0;
}
