/* Bounded frame walking of OWN native mock code only. No game/runtime image.
 * A frame-chain difference is not an exact Rosetta assertion reproduction. */
#include "stub_asm.hpp"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <libkern/OSCacheControl.h>

auto opcode_internal_to_host(uint16_t value) -> uint16_t { return value; }
struct Capture {
    uintptr_t incomingSP, incomingFP, incomingLR;
    uintptr_t parentFP, parentSP, returnedSP, returnedFP;
    int result, status;
    size_t walkCount;
    bool hasCallerReturn;
    bool inspectSpills, spillsSaved, argumentsPreserved;
};
static_assert(offsetof(Capture, parentFP) == 24 && offsetof(Capture, parentSP) == 32);
static_assert(offsetof(Capture, returnedSP) == 40 && offsetof(Capture, returnedFP) == 48);
static_assert(offsetof(Capture, result) == 56);
extern "C" int owned_frame_caller(void *entry, Capture *capture);
extern "C" char owned_frame_caller_return;

extern "C" __attribute__((noinline)) int owned_frame_decode(Capture *capture, uint32_t offset,
                                                           void *out, void *outLen) {
    const uintptr_t high = reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(pthread_self()));
    const uintptr_t low = high - pthread_get_stacksize_np(pthread_self());
    uintptr_t fp = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
    const uintptr_t sought = reinterpret_cast<uintptr_t>(&owned_frame_caller_return);
    capture->walkCount = 0;
    capture->hasCallerReturn = false;
    capture->argumentsPreserved = offset == 0x1234U &&
        reinterpret_cast<uintptr_t>(out) == 0x2345U &&
        reinterpret_cast<uintptr_t>(outLen) == 0x3456U;
    capture->spillsSaved = false;
    if (capture->inspectSpills) {
        // Only our own wrapper's live stack frame is inspected. The mock has
        // recorded incoming SP before its compiler-generated prologue.
        const uintptr_t sp = capture->incomingSP;
        assert(!(sp & 15U) && sp >= low && sp <= high - 40);
        const auto *spills = reinterpret_cast<const uintptr_t *>(sp);
        capture->spillsSaved = spills[0] == reinterpret_cast<uintptr_t>(capture) &&
            spills[1] == 0x1234U && spills[2] == 0x2345U && spills[3] == 0x3456U &&
            spills[4] == sought;
    }
    for (unsigned n = 0; n < 32; ++n) {
        if ((fp & 15U) || fp < low || fp > high - 16) break;
        const auto *record = reinterpret_cast<const uintptr_t *>(fp);
        ++capture->walkCount;
        if (record[1] == sought) capture->hasCallerReturn = true;
        const uintptr_t next = record[0];
        if (next <= fp || next > high - 16) break;
        fp = next;
    }
    return capture->status;
}

static void word(uint8_t *where, uint32_t value) { std::memcpy(where, &value, 4); }
static void own_absolute_jump(uint8_t *where, uintptr_t target) {
    assert((target >> 48) == 0);
    word(where, 0xd2800000U | ((target & 0xffffU) << 5) | 17U);
    word(where + 4, 0xf2800000U | (1U << 21) | (((target >> 16) & 0xffffU) << 5) | 17U);
    word(where + 8, 0xf2800000U | (2U << 21) | (((target >> 32) & 0xffffU) << 5) | 17U);
    word(where + 12, 0xd61f0000U | (17U << 5));
}
enum class Style { Plain, RelativeTail, PureWrapper, FrameChainPrototype };
static const char *style_name(Style style) {
    switch (style) {
        case Style::Plain: return "plain";
        case Style::RelativeTail: return "relative-tail";
        case Style::PureWrapper: return "pure-wrapper";
        case Style::FrameChainPrototype: return "frame-chain-prototype";
    }
    std::abort();
}

static void check(Style style, int status) {
    const size_t page = static_cast<size_t>(getpagesize());
    auto *code = static_cast<uint8_t *>(mmap(nullptr, page, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANON, -1, 0));
    assert(code != MAP_FAILED && page >= 4096);
    auto *entry = code + 256, *handler = code + 1024;
    uint8_t prologue[16];
    for (int i = 0; i < 4; ++i) word(prologue + i * 4, 0xd503201fU);
    std::memcpy(entry, prologue, 16);
    // Record decoder-entry SP/FP/LR in our own Capture without altering x0.
    word(entry + 16, 0x910003f1U); // MOV x17, SP
    word(entry + 20, 0xf9000011U); // STR x17, [x0]
    word(entry + 24, 0xf900041dU); // STR x29, [x0,#8]
    word(entry + 28, 0xf900081eU); // STR x30, [x0,#16]
    own_absolute_jump(entry + 32, reinterpret_cast<uintptr_t>(&owned_frame_decode));
    if (style != Style::Plain) {
        auto blob = stub_asm::buildDecodeHook(reinterpret_cast<uintptr_t>(handler),
            reinterpret_cast<uintptr_t>(entry), prologue, 0x777, false, false, true,
            style == Style::RelativeTail, true
#ifdef FRAME_CHAIN_OPTIONS
            , style == Style::FrameChainPrototype
#endif
            );
        assert(blob.entry.size() == 16 && blob.handler.size() == 360);
#ifndef FRAME_CHAIN_OPTIONS
        if (style == Style::FrameChainPrototype) {
            // Same 96-byte depth, but a linked FP/LR pair. This is an OWN mock
            // prototype, not a helper/runtime installation or shipped change.
            const uint32_t words[] = {
                0xd10183ffU, // SUB SP,SP,#96
                0xa9057bfdU, // STP x29,x30,[SP,#80]
                0x910143fdU, // ADD x29,SP,#80
                0x9400004fU, // BL handler+328 from handler+12
                0xa9457bfdU, // LDP x29,x30,[SP,#80]
                0x910183ffU, // ADD SP,SP,#96
                0xd65f03c0U  // RET
            };
            std::memcpy(blob.handler.data(), words, sizeof(words));
        }
#endif
        std::memcpy(entry, blob.entry.data(), 16);
        std::memcpy(handler, blob.handler.data(), blob.handler.size());
    }
    assert(mprotect(code, page, PROT_READ | PROT_EXEC) == 0);
    sys_icache_invalidate(code, page);
    Capture capture{};
    capture.status = status;
    capture.inspectSpills = style == Style::PureWrapper
#ifdef FRAME_CHAIN_OPTIONS
        || style == Style::FrameChainPrototype
#endif
        ;
    const int result = owned_frame_caller(entry, &capture);
    const bool addedFrame = style == Style::PureWrapper || style == Style::FrameChainPrototype;
    const uintptr_t expectedSP = capture.parentSP - (addedFrame ? 96 : 0);
    const uintptr_t expectedFP = style == Style::FrameChainPrototype
        ? capture.parentSP - 16 : capture.parentFP;
    constexpr size_t linkedReturnOffset =
#ifdef FRAME_CHAIN_OPTIONS
        28; // v2's BL is at index 6; existing spill stores are retained.
#else
        16; // The original seven-instruction, owned mock prototype.
#endif
    const uintptr_t expectedLR = style == Style::PureWrapper
        ? reinterpret_cast<uintptr_t>(handler + 20)
        : style == Style::FrameChainPrototype ? reinterpret_cast<uintptr_t>(handler + linkedReturnOffset)
        : reinterpret_cast<uintptr_t>(&owned_frame_caller_return);
    const bool ok = result == status && capture.result == status &&
        capture.returnedSP == capture.parentSP && capture.returnedFP == capture.parentFP &&
        capture.incomingSP == expectedSP && capture.incomingFP == expectedFP &&
        capture.incomingLR == expectedLR &&
        capture.argumentsPreserved && (!capture.inspectSpills || capture.spillsSaved) &&
        capture.hasCallerReturn == (style != Style::PureWrapper);
    std::printf("OBS style=%s status-value=%d extra-stack=%llu walk-count=%zu "
                "caller-return-visible=%s returns-preserved=%s "
                "argument-registers-preserved=%s old-spills-preserved=%s status=%s\n",
        style_name(style), status, static_cast<unsigned long long>(capture.parentSP - capture.incomingSP),
        capture.walkCount, capture.hasCallerReturn ? "yes" : "no",
        result == status && capture.returnedSP == capture.parentSP &&
        capture.returnedFP == capture.parentFP ? "yes" : "no",
        capture.argumentsPreserved ? "yes" : "no",
        capture.inspectSpills ? (capture.spillsSaved ? "yes" : "no") : "not-checked",
        ok ? "PASS" : "FAIL");
    assert(ok);
    assert(munmap(code, page) == 0);
}
int main() {
    for (Style style : {Style::Plain, Style::RelativeTail,
                        Style::PureWrapper, Style::FrameChainPrototype})
        for (int status : {0, 1, 2}) check(style, status);
    std::puts("END case=owned-decoder-frame-contract checks=12 status=PASS");
    return 0;
}
