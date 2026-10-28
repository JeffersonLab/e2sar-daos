/**
    Unit tests for the per-run object ids and run catalog records (run_catalog.hpp).
    Header-only logic; needs the DAOS headers but no DAOS server.
*/

#define BOOST_TEST_MODULE RunCatalogTest
#include <boost/test/included/unit_test.hpp>

#include "e2sar_daos/run_catalog.hpp"

using namespace e2sar_daos;

BOOST_AUTO_TEST_CASE(PrefixHashIsFnv1a64) {
    // Published FNV-1a 64-bit test vectors: every node must compute the same object id.
    BOOST_CHECK_EQUAL(prefixHash(""), 0xcbf29ce484222325ull);
    BOOST_CHECK_EQUAL(prefixHash("a"), 0xaf63dc4c8601ec8cull);
    BOOST_CHECK_EQUAL(prefixHash("foobar"), 0x85944171f73967e8ull);
    BOOST_CHECK_NE(prefixHash("pm_20261027_143005"), prefixHash("pm_20261027_143006"));
}

BOOST_AUTO_TEST_CASE(ObjectIdsDontOverlap) {
    // Run objects, the catalog and the legacy object differ in oid.hi.
    BOOST_CHECK_NE(kRunOidHi, kCatalogOidHi);
    BOOST_CHECK_NE(kRunOidHi, 0u);
    BOOST_CHECK_NE(kCatalogOidHi, 0u);
}

BOOST_AUTO_TEST_CASE(CatalogKeys) {
    BOOST_CHECK_EQUAL(hex16(0xE25A), "000000000000e25a");
    BOOST_CHECK_EQUAL(hex16(~0ull), "ffffffffffffffff");
    BOOST_CHECK_EQUAL(catalogRunKey("run42"), "run/run42");
    BOOST_CHECK_EQUAL(catalogOidKey(0xE25A), "oid/000000000000e25a");
    BOOST_CHECK_EQUAL(catalogRecvKey("run42", "ebpf2201.jlab.org"), "recv/run42/ebpf2201.jlab.org");
}

BOOST_AUTO_TEST_CASE(RecordRoundTrip) {
    const Record r{{"committed", "81500"}, {"host", "ebpf2201"}, {"oclass", "SX"}};
    const std::string s = formatRecord(r);
    BOOST_CHECK_EQUAL(s, "committed=81500 host=ebpf2201 oclass=SX");
    BOOST_CHECK(parseRecord(s) == r);
}

BOOST_AUTO_TEST_CASE(RecordEdgeCases) {
    // Empty values become "-", spaces become '_', so the record always parses back.
    BOOST_CHECK_EQUAL(formatRecord({{"a", ""}, {"b", "x y"}}), "a=- b=x_y");
    // Words without '=' and empty keys are skipped; a value may contain '='.
    const Record r = parseRecord("junk k=v =x eq=a=b  ");
    BOOST_CHECK_EQUAL(r.size(), 2u);
    BOOST_CHECK_EQUAL(r.at("k"), "v");
    BOOST_CHECK_EQUAL(r.at("eq"), "a=b");
    BOOST_CHECK(parseRecord("").empty());
}

BOOST_AUTO_TEST_CASE(RunInfoRoundTrip) {
    RunInfo i;
    i.prefix = "pm_20261027_143005";
    i.oidLo = prefixHash(i.prefix);
    i.oclass = "RP_2G1";
    i.chunk = 1048576;
    i.created = 1793111405;
    i.host = "ebpf2201.jlab.org";
    const auto back = RunInfo::fromRecord(i.prefix, parseRecord(formatRecord(i.toRecord())));
    BOOST_REQUIRE(back.has_value());
    BOOST_CHECK_EQUAL(back->oidLo, i.oidLo);
    BOOST_CHECK_EQUAL(back->oclass, i.oclass);
    BOOST_CHECK_EQUAL(back->chunk, i.chunk);
    BOOST_CHECK_EQUAL(back->created, i.created);
    BOOST_CHECK_EQUAL(back->host, i.host);

    // The container default object class is stored as "-" and read back as empty.
    i.oclass.clear();
    BOOST_CHECK_EQUAL(i.toRecord().at("oclass"), "-");
    BOOST_CHECK(RunInfo::fromRecord(i.prefix, i.toRecord())->oclass.empty());
}

BOOST_AUTO_TEST_CASE(RunInfoRejectsBadRecords) {
    BOOST_CHECK(!RunInfo::fromRecord("p", {}).has_value());                   // no oid
    BOOST_CHECK(!RunInfo::fromRecord("p", {{"oid", "xyz"}}).has_value());     // not hex
    BOOST_CHECK(!RunInfo::fromRecord("p", {{"oid", "1f"}, {"chunk", "big"}}).has_value());
    BOOST_CHECK(RunInfo::fromRecord("p", {{"oid", "1f"}}).has_value());      // the rest is optional
}
