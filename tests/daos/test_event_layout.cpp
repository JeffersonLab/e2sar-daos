/**
    Unit tests for the event -> KV layout of layout v1 (event_layout.hpp; no longer used by the apps).
    Header-only logic; needs no DAOS server.
*/

#define BOOST_TEST_MODULE EventLayoutTest
#include <boost/test/included/unit_test.hpp>

#include "e2sar_daos/event_layout.hpp"

using namespace e2sar_daos;

namespace {
constexpr uint64_t MiB = 1024 * 1024;
}

BOOST_AUTO_TEST_CASE(Keys) {
    const auto base = eventKey("run42", 7, 4321);
    BOOST_CHECK_EQUAL(base, "run42/7/4321");
    BOOST_CHECK_EQUAL(chunkKey(base, 0), "run42/7/4321/0");
    BOOST_CHECK_EQUAL(chunkKey(base, 12), "run42/7/4321/12");
}

BOOST_AUTO_TEST_CASE(ChunkingAroundBoundaries) {
    struct Case { uint64_t size, chunk, expChunk, expN; };
    const Case cases[] = {
        {100 * 1024, MiB, 100 * 1024, 1},      // smaller than a chunk: one value of its own size
        {MiB, MiB, MiB, 1},                     // exactly one chunk
        {MiB + 1, MiB, MiB, 2},                 // one byte over
        {3500 * 1024, MiB, MiB, 4},             // 3 full + 428 KiB
        {20 * MiB, MiB, MiB, 20},               // exact multiple
        {5 * MiB, 0, 5 * MiB, 1},               // --chunk 0: whole event in one value
        {0, MiB, 1, 0},                         // empty event: metadata only
    };
    for (const auto& c : cases) {
        const auto m = EventMeta::forSize(c.size, c.chunk);
        BOOST_TEST_CONTEXT("size=" << c.size << " chunk=" << c.chunk) {
            BOOST_CHECK_EQUAL(m.size, c.size);
            BOOST_CHECK_EQUAL(m.chunk, c.expChunk);
            BOOST_CHECK_EQUAL(m.nchunks, c.expN);
            // chunks cover the event exactly
            if (m.nchunks > 0) {
                BOOST_CHECK_LT((m.nchunks - 1) * m.chunk, m.size);
                BOOST_CHECK_GE(m.nchunks * m.chunk, m.size);
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(MetaRoundTrip) {
    const auto m = EventMeta::forSize(93456789, MiB);
    BOOST_CHECK_EQUAL(m.toString(), "size=93456789 chunk=1048576 nchunks=90");
    const auto p = EventMeta::parse(m.toString());
    BOOST_REQUIRE(p.has_value());
    BOOST_CHECK_EQUAL(p->size, m.size);
    BOOST_CHECK_EQUAL(p->chunk, m.chunk);
    BOOST_CHECK_EQUAL(p->nchunks, m.nchunks);
}

BOOST_AUTO_TEST_CASE(MetaRejectsGarbage) {
    BOOST_CHECK(!EventMeta::parse("").has_value());
    BOOST_CHECK(!EventMeta::parse("hello").has_value());
    BOOST_CHECK(!EventMeta::parse("size=10 chunk=0 nchunks=1").has_value());       // zero chunk
    BOOST_CHECK(!EventMeta::parse("size=3000000 chunk=1048576 nchunks=2").has_value());  // inconsistent
}
