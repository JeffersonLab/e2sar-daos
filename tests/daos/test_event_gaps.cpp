// event_gaps.hpp: event number gaps per dataId, and sender restarts (a jump of more than
// restartGap starts a new segment instead of making every later event a duplicate).

#define BOOST_TEST_MODULE EventGapsTests
#include <boost/test/included/unit_test.hpp>

#include "e2sar_daos/event_gaps.hpp"

using e2sar_daos::EventGaps;
using K = EventGaps::Kind;

BOOST_AUTO_TEST_CASE(gaps_late_and_duplicates) {
    EventGaps g(1000);
    BOOST_TEST((g.add(100, 0) == K::Restart));  // the first event starts the first segment
    BOOST_TEST((g.add(101, 0) == K::Next));
    BOOST_TEST((g.add(110, 0) == K::Next));  // 102..109 open
    BOOST_TEST(g.open() == 8u);
    BOOST_TEST((g.add(105, 0) == K::Late));
    BOOST_TEST(g.open() == 7u);
    BOOST_TEST((g.add(105, 0) == K::Duplicate));
    BOOST_TEST((g.add(101, 0) == K::Duplicate));
    BOOST_TEST((g.add(99, 0) == K::Earlier));
    BOOST_TEST(g.current().first == 99u);
    BOOST_TEST(g.restarts() == 0u);
}

BOOST_AUTO_TEST_CASE(grace_and_ever_missing) {
    EventGaps g(1000);
    g.add(1, 0);
    g.add(5, 10);  // 2..4 open since t=10
    auto [m, p] = g.missing(12, 5);
    BOOST_TEST(m == 0u);
    BOOST_TEST(p == 3u);
    std::tie(m, p) = g.missing(20, 5);
    BOOST_TEST(m == 3u);
    BOOST_TEST(g.everMissing() == 3u);
    g.add(3, 21);  // late: still counted in everMissing
    std::tie(m, p) = g.missing(22, 5);
    BOOST_TEST(m == 2u);
    BOOST_TEST(g.everMissing() == 3u);
}

// LB 682: the sender restarted from ~67.5M to ~5.2M. The new numbers must not all be duplicates.
BOOST_AUTO_TEST_CASE(sender_restart_starts_a_segment) {
    EventGaps g(1000000);
    for (uint64_t e = 67500000; e < 67500100; e += 2)
        g.add(e, 0);  // every other number: 49 one-number gaps
    BOOST_TEST(g.open() == 49u);
    BOOST_TEST((g.add(5200000, 0) == K::Restart));
    BOOST_TEST(g.restarts() == 1u);
    BOOST_TEST(g.current().first == 5200000u);
    BOOST_TEST((g.add(5200001, 0) == K::Next));
    BOOST_TEST((g.add(5200010, 0) == K::Next));  // 8 missing in the new segment
    BOOST_TEST(g.open() == 49u + 8u);
    // a late event of the old numbering (another writer catching up) fills its old gap
    bool cur = true;
    BOOST_TEST((g.add(67500001, 0, &cur) == K::Late));
    BOOST_TEST(!cur);
    BOOST_TEST(g.open() == 48u + 8u);
    BOOST_TEST(g.current().max == 5200010u);  // the newest segment is still current
    // more old-numbering events extend the old segment, not the new one
    BOOST_TEST((g.add(67500200, 0) == K::Next));
    BOOST_TEST(g.current().max == 5200010u);
}

BOOST_AUTO_TEST_CASE(forward_jump_is_a_restart_too) {
    EventGaps g(1000);
    g.add(10, 0);
    BOOST_TEST((g.add(5000, 0) == K::Restart));
    BOOST_TEST(g.open() == 0u);
    BOOST_TEST((g.add(1005, 0) == K::Next));  // within restartGap of the first segment
    BOOST_TEST(g.open() == 994u);
}

BOOST_AUTO_TEST_CASE(old_segments_are_dropped) {
    EventGaps g(10);
    for (uint64_t i = 0; i < EventGaps::kMaxSegments + 3; i++)
        g.add(i * 1000, 0);
    BOOST_TEST(g.segments().size() == EventGaps::kMaxSegments);
    BOOST_TEST(g.restarts() == EventGaps::kMaxSegments + 2);
}
