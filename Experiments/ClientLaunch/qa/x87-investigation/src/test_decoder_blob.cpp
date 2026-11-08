/* Generated-byte integrity checks; only owned emitter output, no patch target. */
#include "stub_asm.hpp"
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <utility>

// buildDecodeHook receives its opcode explicitly; build() is not tested here.
auto opcode_internal_to_host(uint16_t value) -> uint16_t { return value; }

#ifdef NEAR_OPTIONS
static uint64_t branch_target(const std::vector<uint8_t>& bytes, size_t offset, uint64_t from)
{
    uint32_t word;
    std::memcpy(&word, bytes.data() + offset, 4);
    assert((word & 0xfc000000U) == 0x14000000U);  // B, not BL
    int64_t words = word & 0x03ffffffU;
    if (words & 0x02000000) words -= int64_t{1} << 26;
    for (size_t i = offset + 4; i < offset + 16; i += 4) {
        std::memcpy(&word, bytes.data() + i, 4);
        assert(word == 0xd503201fU);
    }
    return static_cast<uint64_t>(static_cast<int64_t>(from) + words * 4);
}
#endif

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    const uint8_t prologue[16] = {0xff, 0xc3, 0x01, 0xd1, 0xfc, 0x6f, 0x01, 0xa9,
                                0xfa, 0x67, 0x02, 0xa9, 0xf8, 0x5f, 0x03, 0xa9};
    auto original = stub_asm::buildDecodeHook(0x208073288, 0x208059b60, prologue, 0x777);
    assert(original.entry.size() == 16 && original.handler.size() == 360);
    assert(std::memcmp(original.handler.data() + 82 * 4, prologue, 16) == 0);
#ifdef EXPERIMENT_OPTIONS
    for (bool arpl : {false, true}) for (bool fcomp : {false, true}) {
        auto changed = stub_asm::buildDecodeHook(0x208073288, 0x208059b60, prologue, 0x777, arpl, fcomp);
        assert(changed.entry == original.entry && changed.handler.size() == original.handler.size());
        for (size_t offset = 0; offset < changed.handler.size(); ++offset) {
            bool expected_change = (!arpl && offset / 4 == 20) || (!fcomp && offset / 4 == 22);
            if (!expected_change) assert(changed.handler[offset] == original.handler[offset]);
        }
        assert((arpl && fcomp) == (changed.handler == original.handler));
    }
#endif
#ifdef NEAR_OPTIONS
    constexpr uint64_t entry = 0x208059b60, handler = 0x208073288;
    auto near = stub_asm::buildDecodeHook(handler, entry, prologue, 0x777, true, true, true);
    assert(near.entry.size() == 16 && near.handler.size() == 360);
    assert(branch_target(near.entry, 0, entry) == handler);
    assert(branch_target(near.handler, 344, handler + 344) == entry + 16);
    assert(near.entry != original.entry);
    assert(std::memcmp(near.handler.data(), original.handler.data(), 344) == 0);
    constexpr int64_t limit = int64_t{1} << 27;
    // Validate BOTH directions: the jump back begins 344 bytes into the
    // handler, so a positive entry displacement can be valid while return
    // is already too far. Never accept a silently wrapped jump.
    for (const auto [distance, accepted] : {
        std::pair{-limit - 4, false}, std::pair{-limit, true},
        std::pair{-int64_t{4}, true}, std::pair{int64_t{0}, true},
        std::pair{int64_t{4}, true}, std::pair{limit - 328, true},
        std::pair{limit - 324, false}, std::pair{limit - 4, false},
        std::pair{limit, false}}) {
        constexpr uint64_t from = 0x20000000;
        const uint64_t target = static_cast<uint64_t>(static_cast<int64_t>(from) + distance);
        auto blob = stub_asm::buildDecodeHook(target, from, prologue, 0x777, true, true, true);
        assert((!blob.entry.empty()) == accepted);
        if (accepted) {
            assert(blob.entry.size() == 16 && blob.handler.size() == 360);
            assert(branch_target(blob.entry, 0, from) == target);
            assert(branch_target(blob.handler, 344, target + 344) == from + 16);
        } else assert(blob.handler.empty());
    }
    for (unsigned misalignment : {1U, 2U, 3U}) {
        auto badEntry = stub_asm::buildDecodeHook(handler, entry + misalignment,
                                                 prologue, 0x777, true, true, true);
        auto badHandler = stub_asm::buildDecodeHook(handler + misalignment, entry,
                                                   prologue, 0x777, true, true, true);
        assert(badEntry.entry.empty() && badEntry.handler.empty());
        assert(badHandler.entry.empty() && badHandler.handler.empty());
    }
    auto badHigh = stub_asm::buildDecodeHook(uint64_t{1} << 48, entry,
                                            prologue, 0x777, true, true, true);
    assert(badHigh.entry.empty() && badHigh.handler.empty());
    std::puts("END case=relative-branch-targets-bounds status=PASS");
#endif
#ifdef TAIL_OPTIONS
    auto tail = stub_asm::buildDecodeHook(0x208073288, 0x208059b60, prologue, 0x777,
                                        true, true, true);
    assert(tail.handler == original.handler && tail.entry.size() == original.entry.size());
    uint64_t destination = 0;
    for (int index = 0; index < 3; ++index) {
        uint32_t word;
        std::memcpy(&word, tail.entry.data() + 4 * index, 4);
        destination |= uint64_t((word >> 5) & 0xffff) << (16 * index);
    }
    assert(destination == 0x208073288 + 82 * 4);
    assert(tail.entry != original.entry);
#endif
#ifdef NEAR_TAIL_OPTIONS
    constexpr uint64_t tailEntry = 0x208059b60, tailHandler = 0x208073288;
    auto relativeTail = stub_asm::buildDecodeHook(tailHandler, tailEntry,
        prologue, 0x777, true, true, true, true);
    auto relativeWrapper = stub_asm::buildDecodeHook(tailHandler, tailEntry,
        prologue, 0x777, true, true, true, false);
    assert(relativeTail.entry.size() == 16 && relativeTail.handler.size() == 360);
    assert(relativeTail.handler == relativeWrapper.handler);
    assert(branch_target(relativeTail.entry, 0, tailEntry) == tailHandler + 328);
    assert(branch_target(relativeTail.handler, 344, tailHandler + 344) == tailEntry + 16);
    assert(std::memcmp(relativeTail.handler.data() + 328, prologue, 16) == 0);
    auto absoluteTail = stub_asm::buildDecodeHook(tailHandler, tailEntry,
        prologue, 0x777, true, true, false, true);
    assert(absoluteTail.entry.size() == 16 && absoluteTail.handler == original.handler);
    uint64_t tailDestination = 0;
    for (int i = 0; i < 3; ++i) {
        uint32_t word;
        std::memcpy(&word, absoluteTail.entry.data() + 4 * i, 4);
        tailDestination |= uint64_t((word >> 5) & 0xffff) << (16 * i);
    }
    assert(tailDestination == tailHandler + 328);
    // Tail entry and return displacements are +/- (handler-entry+328), so
    // BOTH directions accept at most +2^27-4 for that total displacement.
    constexpr int64_t tailLimit = int64_t{1} << 27;
    for (const auto [distance, accepted] : {
        std::pair{-tailLimit - 332, false}, std::pair{-tailLimit - 328, false},
        std::pair{-tailLimit - 324, true}, std::pair{-int64_t{328}, true},
        std::pair{int64_t{0}, true}, std::pair{tailLimit - 332, true},
        std::pair{tailLimit - 328, false}, std::pair{tailLimit - 324, false}}) {
        constexpr uint64_t from = 0x20000000;
        const uint64_t target = static_cast<uint64_t>(static_cast<int64_t>(from) + distance);
        auto blob = stub_asm::buildDecodeHook(target, from, prologue,
            0x777, true, true, true, true);
        assert((!blob.entry.empty()) == accepted);
        if (accepted) {
            assert(blob.handler.size() == 360);
            assert(branch_target(blob.entry, 0, from) == target + 328);
            assert(branch_target(blob.handler, 344, target + 344) == from + 16);
        } else assert(blob.handler.empty());
    }
    for (unsigned misalignment : {1U, 2U, 3U}) {
        auto badEntry = stub_asm::buildDecodeHook(tailHandler, tailEntry + misalignment,
            prologue, 0x777, true, true, true, true);
        auto badHandler = stub_asm::buildDecodeHook(tailHandler + misalignment, tailEntry,
            prologue, 0x777, true, true, true, true);
        assert(badEntry.entry.empty() && badEntry.handler.empty());
        assert(badHandler.entry.empty() && badHandler.handler.empty());
    }
    auto badTailHigh = stub_asm::buildDecodeHook(uint64_t{1} << 48, tailEntry,
        prologue, 0x777, true, true, true, true);
    assert(badTailHigh.entry.empty() && badTailHigh.handler.empty());
    for (bool relative : {false, true}) {
        auto overflowingTail = stub_asm::buildDecodeHook((uint64_t{1} << 48) - 4,
            tailEntry, prologue, 0x777, true, true, relative, true);
        assert(overflowingTail.entry.empty() && overflowingTail.handler.empty());
    }
    std::puts("END case=relative-tail-targets-bounds status=PASS");
#endif
#ifdef PASSTHROUGH_OPTIONS
    auto wrapper = stub_asm::buildDecodeHook(0x208073288, 0x208059b60,
        prologue, 0x777, false, false, true, false, false);
    auto passthrough = stub_asm::buildDecodeHook(0x208073288, 0x208059b60,
        prologue, 0x777, false, false, true, false, true);
    assert(passthrough.entry == wrapper.entry && passthrough.handler.size() == 360);
    for (size_t offset = 0; offset < wrapper.handler.size(); ++offset) {
        if (offset / 4 != 5) assert(passthrough.handler[offset] == wrapper.handler[offset]);
    }
    uint32_t passBranch;
    std::memcpy(&passBranch, passthrough.handler.data() + 5 * 4, 4);
    assert(passBranch == (0x14000000U | (79U - 5U)));
    assert(passthrough.handler != wrapper.handler);
    std::puts("END case=passthrough-skips-post-decode-inspection status=PASS");
#endif
#ifdef FRAME_CHAIN_OPTIONS
    auto linked = stub_asm::buildDecodeHook(0x208073288, 0x208059b60,
        prologue, 0x777, false, false, true, false, true, true);
    const uint32_t linkedWords[] = {0xd10183ffU, 0xa90007e0U, 0xa9010fe2U,
        0xf90013feU, 0xa9057bfdU, 0x910143fdU, 0x9400004cU,
        0xa9457bfdU, 0x910183ffU, 0xd65f03c0U};
    assert(linked.entry == passthrough.entry && linked.handler.size() == 360);
    // Original frame size and x0/x1, x2/x3, caller-LR spills stay byte-identical.
    assert(std::memcmp(linked.handler.data(), passthrough.handler.data(), 16) == 0);
    assert(std::memcmp(linked.handler.data(), linkedWords, sizeof(linkedWords)) == 0);
    assert(std::memcmp(linked.handler.data() + sizeof(linkedWords),
                       passthrough.handler.data() + sizeof(linkedWords),
                       360 - sizeof(linkedWords)) == 0);
    assert(branch_target(linked.entry, 0, 0x208059b60) == 0x208073288);
    assert(branch_target(linked.handler, 344, 0x208073288 + 344) == 0x208059b60 + 16);
    for (unsigned invalid : {0U, 1U, 2U}) {
        auto refused = stub_asm::buildDecodeHook(0x208073288, 0x208059b60,
            prologue, 0x777, false, false, invalid != 1, invalid == 2, invalid != 0, true);
        assert(refused.entry.empty() && refused.handler.empty());
    }
    std::puts("END case=linked-frame-prefix-spills-targets-refusal status=PASS");
#endif
    std::ofstream output(argv[1], std::ios::binary);
    output.write(reinterpret_cast<const char *>(original.entry.data()), original.entry.size());
    output.write(reinterpret_cast<const char *>(original.handler.data()), original.handler.size());
    if (!output) return 87;
    std::puts("END case=decoder-blob status=PASS");
    return 0;
}
