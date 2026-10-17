// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstdint>

struct RemoteInput {
    uint32_t mask = 0;
    uint64_t expires = 0;
    int32_t x = -1, y = -1, sx = 0, sy = 0;
};

// One producer (network worker), arbitrarily many readers/cancellers.
// Readers take ONE snapshot attempt: no mutex, spin loop, or allocation in VPAD.
// Every shared word is atomic, including the split 64-bit deadline on PowerPC.
class InputMailbox {
    static_assert(std::atomic<uint32_t>::is_always_lock_free, "32-bit atomics must be lock free");
    std::atomic<uint32_t> sequence{0}, epoch{0}, publishedEpoch{0};
    std::atomic<uint32_t> mask{0}, high{0}, low{0};
    std::atomic<int32_t> x{-1}, y{-1}, sx{0}, sy{0};
public:
    void cancel() noexcept { epoch.fetch_add(1); }
    void publish(const RemoteInput &input) noexcept {
        const auto generation = epoch.load();
        sequence.fetch_add(1);
        mask.store(input.mask);
        high.store(static_cast<uint32_t>(input.expires >> 32));
        low.store(static_cast<uint32_t>(input.expires));
        x.store(input.x); y.store(input.y); sx.store(input.sx); sy.store(input.sy);
        publishedEpoch.store(generation);
        sequence.fetch_add(1);
    }
    bool read(uint64_t now, RemoteInput &out) const noexcept {
        const auto version = sequence.load();
        if (version & 1) return false;
        RemoteInput sample;
        sample.mask = mask.load();
        sample.expires = (uint64_t(high.load()) << 32) | low.load();
        sample.x = x.load(); sample.y = y.load(); sample.sx = sx.load(); sample.sy = sy.load();
        const auto generation = publishedEpoch.load();
        if (version != sequence.load() || generation != epoch.load() || now >= sample.expires)
            return false;
        out = sample;
        return true;
    }
};

// Physical held buttons suppress spurious release/press edges from remote input.
template <class Sample>
inline void overlayButtons(Sample &sample, uint32_t mask, uint32_t previous) noexcept {
    const uint32_t physical = sample.hold;
    sample.hold |= mask;
    sample.trigger |= mask & ~previous & ~physical;
    sample.release |= previous & ~mask & ~physical;
    sample.release &= ~mask;
}
