// MessageQueue: order, multi-producer completeness, wake hook, wait_for.
#include "check.h"

#include "brosys/event_queue.h"
#include "brosys/power.h"

#include <atomic>
#include <thread>
#include <vector>

using namespace brosys;

static void test_order_and_drain() {
    MessageQueue<int> q;
    for (int i = 0; i < 5; ++i) q.push(i);
    CHECK_EQ(q.size(), size_t(5));
    auto v = q.drain();
    CHECK_EQ(v.size(), size_t(5));
    for (int i = 0; i < 5; ++i) CHECK_EQ(v[i], i);
    CHECK(q.drain().empty());
}

static void test_multi_producer() {
    MessageQueue<int> q;
    std::atomic<int> wakes{0};
    q.set_wake([&] { wakes.fetch_add(1); });
    std::vector<std::thread> producers;
    for (int t = 0; t < 4; ++t)
        producers.emplace_back([&q, t] {
            for (int i = 0; i < 1000; ++i) q.push(t * 1000 + i);
        });
    for (auto& p : producers) p.join();
    auto v = q.drain();
    CHECK_EQ(v.size(), size_t(4000));
    CHECK_EQ(wakes.load(), 4000);
    // Per-producer order is preserved.
    int last[4] = {-1, -1, -1, -1};
    for (int x : v) {
        int t = x / 1000;
        CHECK(x > last[t]);
        last[t] = x;
    }
}

static void test_wait_for() {
    PowerEventQueue q;
    CHECK(!q.wait_for(std::chrono::milliseconds(20)));
    std::thread producer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        q.push(SleepPrepare{true});
    });
    CHECK(q.wait_for(std::chrono::milliseconds(5000)));
    producer.join();
    auto v = q.drain();
    REQUIRE(v.size() == 1);
    CHECK(std::holds_alternative<SleepPrepare>(v[0]));
}

int main() {
    test_order_and_drain();
    test_multi_producer();
    test_wait_for();
    return bstest::finish("test_event_queue");
}
