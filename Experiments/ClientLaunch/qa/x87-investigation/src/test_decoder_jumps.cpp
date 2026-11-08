/* Exercise only emitted entry/return jump words on our own ARM64 mock.
 * This does NOT establish that the real Rosetta decoder must preserve x16.
 * No runtime/game image is opened or changed by this executable. */
#include "stub_asm.hpp"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
#include <libkern/OSCacheControl.h>

auto opcode_internal_to_host(uint16_t value) -> uint16_t { return value; }
extern "C" uint64_t decoder_jump_probe_call(void *entry, uint64_t marker);

static void word(uint8_t *where, uint32_t value) {
    std::memcpy(where, &value, sizeof(value));
}

enum class Style { Plain, Absolute, Relative, AbsoluteTail, RelativeTail };

static const char *style_name(Style style) {
    switch (style) {
        case Style::Plain: return "plain";
        case Style::Absolute: return "absolute";
        case Style::Relative: return "relative";
        case Style::AbsoluteTail: return "absolute-tail";
        case Style::RelativeTail: return "relative-tail";
    }
    std::abort();
}

static void check(bool forward, Style style, uint64_t marker) {
    const size_t size = static_cast<size_t>(getpagesize());
    auto *memory = static_cast<uint8_t *>(mmap(nullptr, size, PROT_READ | PROT_WRITE,
                                             MAP_PRIVATE | MAP_ANON, -1, 0));
    if (memory == MAP_FAILED) { std::perror("owned mmap"); std::abort(); }
    uint8_t *entry = memory + (forward ? 256 : 2048);
    uint8_t *handler = memory + (forward ? 1024 : 256);
    uint8_t prologue[16];
    for (int i = 0; i < 4; ++i) word(prologue + i * 4, 0xd503201fU); // NOP
    std::memcpy(entry, prologue, 16);
    word(entry + 16, 0xaa1003e0U); // MOV x0, x16: expose incoming register
    word(entry + 20, 0xd65f03c0U); // RET
    if (style != Style::Plain) {
        const bool relative = style == Style::Relative || style == Style::RelativeTail;
        auto blob = stub_asm::buildDecodeHook(reinterpret_cast<uint64_t>(handler),
                         reinterpret_cast<uint64_t>(entry), prologue, 0x777,
                         true, true, relative
#ifdef NEAR_TAIL_OPTIONS
                         , style == Style::AbsoluteTail || style == Style::RelativeTail
#endif
                         );
        assert(blob.entry.size() == 16 && blob.handler.size() == 360);
        std::memcpy(entry, blob.entry.data(), 16);
        // Use the emitted displaced-prologue and return stub verbatim. A mock
        // local B skips the decoder wrapper for the wrapper-entry styles.
        // Tail-entry styles reach STASH directly using their emitted word.
        word(handler, 0x14000000U | 82U); // B handler+328
        std::memcpy(handler + 328, blob.handler.data() + 328, 32);
    }
    if (mprotect(memory, size, PROT_READ | PROT_EXEC) != 0) {
        std::perror("owned mprotect"); std::abort();
    }
    sys_icache_invalidate(memory, size);
    const uint64_t observed = decoder_jump_probe_call(entry, marker);
    const uint64_t expected = style == Style::Absolute || style == Style::AbsoluteTail
        ? reinterpret_cast<uint64_t>(entry + 16) : marker;
    const bool ok = observed == expected;
    std::printf("OBS direction=%s style=%s marker=0x%llx observed=0x%llx expected=0x%llx status=%s\n",
        forward ? "forward" : "backward",
        style_name(style),
        static_cast<unsigned long long>(marker), static_cast<unsigned long long>(observed),
        static_cast<unsigned long long>(expected), ok ? "PASS" : "FAIL");
    assert(ok);
    assert(munmap(memory, size) == 0);
}

int main() {
    for (bool forward : {false, true})
        for (Style style : {Style::Plain, Style::Absolute, Style::Relative
#ifdef NEAR_TAIL_OPTIONS
                           , Style::AbsoluteTail, Style::RelativeTail
#endif
                           })
            for (uint64_t marker : {uint64_t{0}, uint64_t{1}, uint64_t{0xffff},
                                   uint64_t{0x123456789abcdef0}, uint64_t{0x8000000000000000},
                                   uint64_t{0xffffffffffffffff}, uint64_t{0x208073288}})
                check(forward, style, marker);
#ifdef NEAR_TAIL_OPTIONS
    std::puts("END case=owned-jump-register-observation checks=70 status=PASS");
#else
    std::puts("END case=owned-jump-register-observation checks=42 status=PASS");
#endif
    return 0;
}
