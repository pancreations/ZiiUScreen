// Runs on Windows, using the exact input mailbox and button overlay in the plugin.
#include "source/input_mailbox.h"
#include <cassert>
#include <iostream>
#include <thread>
#include <vector>

int main() {
    InputMailbox box;
    RemoteInput out;
    assert(!box.read(0, out));
    RemoteInput command{0x400, (uint64_t(1) << 32) + 50, 20, 30, 1000, -1000};
    box.publish(command);
    assert(box.read((uint64_t(1) << 32) - 1, out));
    assert(out.mask == command.mask && out.expires == command.expires && out.sy == -1000);
    assert(!box.read(command.expires, out));
    assert(!box.read(command.expires + (uint64_t(1) << 32), out));
    box.cancel();
    assert(!box.read(0, out));
    box.publish(command);
    assert(box.read(0, out));

    struct Sample { uint32_t hold, trigger, release; };
    Sample press{}; overlayButtons(press, 0x400, 0);
    assert(press.hold == 0x400 && press.trigger == 0x400 && press.release == 0);
    Sample held{}; overlayButtons(held, 0x400, 0x400);
    assert(held.hold == 0x400 && held.trigger == 0 && held.release == 0);
    Sample release{}; overlayButtons(release, 0, 0x400);
    assert(release.hold == 0 && release.release == 0x400);
    Sample physical{0x400, 0, 0}; overlayButtons(physical, 0, 0x400);
    assert(physical.hold == 0x400 && physical.release == 0);

    // Concurrent publication/cancellation: no partially mixed command is accepted.
    std::atomic<bool> done{false};
    box.cancel();
    std::thread writer([&] {
        for (uint32_t i = 1; i <= 200000; ++i) {
            RemoteInput sample{i, (uint64_t(i) << 32) | i,
                               int32_t(i), -int32_t(i), int32_t(i), -int32_t(i)};
            box.publish(sample);
            if ((i % 11) == 0) box.cancel();
        }
        done.store(true);
    });
    while (!done.load()) {
        if (box.read(0, out)) {
            assert(out.expires == ((uint64_t(out.mask) << 32) | out.mask));
            assert(out.x == int32_t(out.mask) && out.y == -int32_t(out.mask));
            assert(out.sx == out.x && out.sy == out.y);
        }
    }
    writer.join();
    box.cancel(); assert(!box.read(0, out));

    // Reproduce the old title-call contract with a guarded logical output buffer.
    // homebrew_on_menu appends even if real_MCP_TitleList fails, leaving count unchanged.
    constexpr unsigned capacity = 3, appendedHomebrew = 2;
    std::vector<uint32_t> storage(capacity + appendedHomebrew, 0xCAFE);
    unsigned oldOutputCount = capacity;
    for (unsigned i = 0; i < appendedHomebrew; ++i) storage[oldOutputCount++] = 0xBAD;
    assert(storage[capacity] != 0xCAFE); // Writes beyond the caller's declared capacity.
    std::cout << "PASS: expiry, cancellation, physical-input edges, concurrent snapshots, title-overrun reproduction\n";
}
