// ITC ping-pong: two EM threads exchanging mailbox posts (DESIGN.md §11).
// Shard 0 sends a ping to shard 1 via post_and_reply; shard 1 computes the
// reply and shard 0 counts it.

#include <cstdio>

#include "afx/afx.hpp"

using namespace afx;

int main() {
    Runtime rt(Topology::detect());
    ThreadConfig cfg;
    cfg.placement = Placement::None;
    cfg.em.wait = WaitStrategy::Block;

    auto g = rt.spawn_group("pp", 2, cfg);
    rt.start();
    auto mbs = g.mailboxes();
REQUIRE_SIZE:
    if (mbs.size() < 2) {
        std::fprintf(stderr, "need 2 shards\n");
        rt.shutdown(1s);
        rt.join();
        return 1;
    }

    auto pongs = std::make_shared<std::atomic<int>>(0);
    constexpr int kRounds = 5;
    for (int i = 0; i < kRounds; ++i) {
        Mailbox ping = mbs[1];  // work lands on shard 1
        Mailbox home = mbs[0];  // reply lands back on shard 0
        ping.post_and_reply([] { return 1; }, home,
                            [pongs](int v) { pongs->fetch_add(v); });
    }

    rt.shutdown(5s);
    rt.join();
    std::printf("pongs: %d\n", pongs->load());
    return 0;
}
