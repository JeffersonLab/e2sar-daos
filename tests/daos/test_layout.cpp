/**
    Unit tests for the run layout (layout.hpp): object ids, index keys, batch tables and
    catalog records. Header-only logic; needs the DAOS headers but no DAOS server.
*/

#define BOOST_TEST_MODULE LayoutTest
#include <boost/test/included/unit_test.hpp>

#include "e2sar_daos/layout.hpp"

using namespace e2sar_daos;
using namespace e2sar_daos::layout;

BOOST_AUTO_TEST_CASE(ObjectIdsReadAsDecimalDigits) {
    BOOST_CHECK_EQUAL(oidLo(Kind::Catalog), 100000000000000ull);
    BOOST_CHECK_EQUAL(oidLo(Kind::Seconds), 200000000000000ull);
    BOOST_CHECK_EQUAL(oidLo(Kind::Index, 0, 1793280042), 300001793280042ull);
    BOOST_CHECK_EQUAL(oidLo(Kind::Data, 1, 1793280042), 400011793280042ull);
    BOOST_CHECK_EQUAL(std::to_string(oidLo(Kind::Data, 9999, 1793280042)), "499991793280042");

    const auto p = splitOidLo(400011793280042ull);
    BOOST_CHECK(p.kind == Kind::Data);
    BOOST_CHECK_EQUAL(p.slot, 1u);
    BOOST_CHECK_EQUAL(p.second, 1793280042ull);

    BOOST_CHECK_THROW(oidLo(Kind::Data, 10000, 1), std::out_of_range);
    BOOST_CHECK_THROW(oidLo(Kind::Data, 0, 10000000000ull), std::out_of_range);
}

BOOST_AUTO_TEST_CASE(IndexKeys) {
    BOOST_CHECK_EQUAL(indexDkey(1, 391), 0x0001000000000187ull);
    BOOST_CHECK_EQUAL(indexDkeySlot(indexDkey(9999, 5)), 9999u);
    BOOST_CHECK_EQUAL(indexDkeySeq(indexDkey(9999, 5)), 5ull);
}

BOOST_AUTO_TEST_CASE(BatchTableRoundTrip) {
    BatchTable t;
    t.slot = 1;
    t.tData = 1793280042;
    t.offset = 2000ull * 1048576;
    t.length = 100000;
    t.entries = {{1043, 0, 20000, 4321, 0}, {1047, 20000, 20000, 4321, 0}, {~0ull, 40000, 60000, 65535, 7}};

    std::vector<uint8_t> buf;
    encodeTable(t, buf);
    BOOST_CHECK_EQUAL(buf.size(), 40u + 3 * 32);
    BOOST_CHECK_EQUAL(std::string(buf.begin(), buf.begin() + 4), "E2SB");

    const auto d = decodeTable(buf.data(), buf.size());
    BOOST_REQUIRE(d);
    BOOST_CHECK_EQUAL(d->slot, 1u);
    BOOST_CHECK_EQUAL(d->tData, 1793280042ull);
    BOOST_CHECK_EQUAL(d->offset, 2000ull * 1048576);
    BOOST_CHECK_EQUAL(d->length, 100000ull);
    BOOST_REQUIRE_EQUAL(d->entries.size(), 3u);
    BOOST_CHECK_EQUAL(d->entries[2].evtNum, ~0ull);  // any 64-bit event number
    BOOST_CHECK_EQUAL(d->entries[2].dataId, 65535);
    BOOST_CHECK_EQUAL(d->entries[2].flags, 7);
    BOOST_CHECK_EQUAL(d->entries[1].offset, 20000ull);

    // Little endian on the wire: n = 3 right after "E2SB"
    BOOST_CHECK_EQUAL(buf[4], 3);
    BOOST_CHECK_EQUAL(buf[5], 0);
}

BOOST_AUTO_TEST_CASE(BatchTableRejectsBadInput) {
    BatchTable t;
    t.entries.resize(2);
    std::vector<uint8_t> buf;
    encodeTable(t, buf);
    BOOST_CHECK(!decodeTable(buf.data(), buf.size() - 1));  // wrong size for n
    BOOST_CHECK(!decodeTable(buf.data(), 39));              // shorter than a header
    buf[0] = 'X';
    BOOST_CHECK(!decodeTable(buf.data(), buf.size()));  // no "E2SB"

    BatchTable empty;
    encodeTable(empty, buf);
    BOOST_CHECK(decodeTable(buf.data(), buf.size()));  // n = 0 is valid
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

BOOST_AUTO_TEST_CASE(CatalogRecords) {
    BOOST_CHECK_EQUAL(writerKey(3), "writer/3");
    BOOST_CHECK_EQUAL(wmKey(3), "wm/3");
    BOOST_CHECK_EQUAL(doneKey(3), "done/3");

    RunSettings s;
    s.start = 1793280000;
    s.chunk = 1048576;
    s.oclass = "SX";
    s.indexOclass = "S8";
    s.batchThreshold = 0.9;
    const std::string rec = formatRecord(s.toRecord());
    BOOST_CHECK_EQUAL(rec, "batch_threshold=0.9 chunk=1048576 index_oclass=S8 oclass=SX start=1793280000");
    const auto back = RunSettings::fromRecord(parseRecord(rec));
    BOOST_REQUIRE(back);
    BOOST_CHECK(back->mismatch(s).empty());

    RunSettings other = s;
    other.chunk = 2097152;
    BOOST_CHECK(!back->mismatch(other).empty());
    BOOST_CHECK(!RunSettings::fromRecord(parseRecord("chunk=0 oclass=SX index_oclass=S8 start=1")));

    const Watermark wm{1793280042, 1793280041};
    BOOST_CHECK_EQUAL(formatRecord(wm.toRecord()), "data=1793280041 index=1793280042");
    const auto w = Watermark::fromRecord(parseRecord("index=5 data=4"));
    BOOST_REQUIRE(w);
    BOOST_CHECK_EQUAL(w->index, 5ull);
    BOOST_CHECK_EQUAL(w->data, 4ull);
}

BOOST_AUTO_TEST_CASE(Chunks) {
    BOOST_CHECK_EQUAL(chunksFor(0, 1048576), 0ull);
    BOOST_CHECK_EQUAL(chunksFor(1, 1048576), 1ull);
    BOOST_CHECK_EQUAL(chunksFor(1048576, 1048576), 1ull);
    BOOST_CHECK_EQUAL(chunksFor(1776313996, 1048576), 1695ull);
}
