/**
    Unit tests for LatencyHistogram (used for e2sar_daos_recv's timing summary).
*/

#define BOOST_TEST_MODULE LatencyHistogramTest
#include <boost/test/included/unit_test.hpp>

#include "e2sar_daos/latency_histogram.hpp"

using e2sar_daos::LatencyHistogram;

BOOST_AUTO_TEST_CASE(Empty) {
    LatencyHistogram h;
    BOOST_CHECK_EQUAL(h.count(), 0u);
    BOOST_CHECK_EQUAL(h.minNs(), 0u);
    BOOST_CHECK_EQUAL(h.maxNs(), 0u);
    BOOST_CHECK_EQUAL(h.percentileNs(50), 0u);
}

BOOST_AUTO_TEST_CASE(SmallValuesAreExact) {
    LatencyHistogram h;
    for (uint64_t v = 0; v < 8; v++)
        h.record(v);
    BOOST_CHECK_EQUAL(h.percentileNs(50), 3u);  // 4th of 8 samples
    BOOST_CHECK_EQUAL(h.percentileNs(100), 7u);
    BOOST_CHECK_EQUAL(h.minNs(), 0u);
}

BOOST_AUTO_TEST_CASE(ExactStatsAndBoundedPercentileError) {
    LatencyHistogram h;
    // 1..100000 us in ns: uniform, so p-th percentile ~ p * 1000 us
    for (uint64_t us = 1; us <= 100000; us++)
        h.record(us * 1000);
    BOOST_CHECK_EQUAL(h.count(), 100000u);
    BOOST_CHECK_EQUAL(h.minNs(), 1000u);
    BOOST_CHECK_EQUAL(h.maxNs(), 100000000u);
    BOOST_CHECK_CLOSE(h.meanNs(), 50000.5 * 1000, 1e-9);
    for (double p : {1.0, 50.0, 90.0, 99.0}) {
        const double exact = p * 1000 * 1000;
        const double got = static_cast<double>(h.percentileNs(p));
        BOOST_TEST_CONTEXT("p" << p) {
            BOOST_CHECK_GE(got, exact * 0.999);  // upper bound of the bucket, never below
            BOOST_CHECK_LE(got, exact * 1.125);  // within one sub-bucket (12.5%)
        }
    }
    BOOST_CHECK_EQUAL(h.percentileNs(100), h.maxNs());  // clamped to max
}

BOOST_AUTO_TEST_CASE(Merge) {
    LatencyHistogram a, b;
    a.record(10);
    a.record(20);
    b.record(5);
    b.record(1000000);
    a.merge(b);
    BOOST_CHECK_EQUAL(a.count(), 4u);
    BOOST_CHECK_EQUAL(a.minNs(), 5u);
    BOOST_CHECK_EQUAL(a.maxNs(), 1000000u);
    BOOST_CHECK_CLOSE(a.meanNs(), (10 + 20 + 5 + 1000000) / 4.0, 1e-9);
}

BOOST_AUTO_TEST_CASE(HugeValues) {
    LatencyHistogram h;
    h.record(UINT64_MAX);
    h.record(uint64_t(1) << 63);
    BOOST_CHECK_EQUAL(h.maxNs(), UINT64_MAX);
    BOOST_CHECK_EQUAL(h.percentileNs(100), UINT64_MAX);
}
