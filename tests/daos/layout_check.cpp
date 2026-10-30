/**
    layout_check POOL CONT [--synthetic]: walk a run written by e2sar_daos_recv
    (docs/concepts/daos-layout.md) and check it.

      - catalog: run settings, writer slots, watermarks, done entries
      - seconds -> index[T] -> batch tables (check value, size, slot)
      - every event's bytes are read back from data[T_data, slot]
      - with --synthetic: each event must be what e2sar_daos_recv --synthetic generated
        (event number in the first 8 bytes, the xorshift template after it)

    Prints a summary per slot and exits 1 on any error. A test tool, not the reader.
*/

#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "e2sar_daos/catalog.hpp"
#include "e2sar_daos/daos_objects.hpp"
#include "e2sar_daos/layout.hpp"

using namespace e2sar_daos;
namespace L = e2sar_daos::layout;

namespace {

bool syntheticOk(const uint8_t* p, size_t size, uint64_t evtNum) {
    uint64_t x = 0x9E3779B97F4A7C15ull;  // same generator as e2sar_daos_recv --synthetic
    for (size_t i = 0; i < size; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        uint8_t want = static_cast<uint8_t>(x);
        if (i < sizeof(evtNum))
            want = static_cast<uint8_t>(evtNum >> (8 * i));
        if (p[i] != want)
            return false;
    }
    return true;
}

struct SlotSummary {
    uint64_t batches = 0, events = 0, bytes = 0, bad = 0;
    uint64_t firstSecond = ~0ull, lastSecond = 0;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: layout_check POOL CONT [--synthetic]" << std::endl;
        return 2;
    }
    const bool synth = argc > 3 && std::string(argv[3]) == "--synthetic";
    int errors = 0;
    try {
        DaosRuntime rt;
        DaosPool pool(argv[1], DAOS_PC_RO);
        DaosContainer cont(pool, argv[2], false, DAOS_COO_RO);
        Catalog cat(cont, DAOS_OO_RO);

        const auto run = cat.run();
        if (!run) {
            std::cerr << "no readable run record" << std::endl;
            return 1;
        }
        std::cout << "run: " << formatRecord(run->toRecord()) << std::endl;
        for (uint32_t s : cat.slots()) {
            const auto wm = cat.watermark(s);
            const auto done = cat.done(s);
            std::cout << "slot " << s << ": wm " << (wm ? formatRecord(wm->toRecord()) : "-") << " | done "
                      << (done ? formatRecord(*done) : "-") << std::endl;
        }

        DaosIntObject seconds(cont, L::oidLo(L::Kind::Seconds), run->indexOclass, DAOS_OO_RO);
        std::set<uint64_t> secs;
        seconds.listDkeys([&](uint64_t t) { secs.insert(t); });
        std::cout << "seconds with index entries: " << secs.size();
        if (!secs.empty())
            std::cout << " (" << *secs.begin() << " .. " << *secs.rbegin() << ")";
        std::cout << std::endl;

        std::map<uint32_t, SlotSummary> perSlot;
        std::set<std::pair<uint32_t, uint64_t>> batchKeys;
        std::map<std::pair<uint32_t, uint64_t>, std::unique_ptr<DaosArray>> arrays;
        std::vector<uint8_t> tableBuf, data;
        for (uint64_t t : secs) {
            DaosIntObject index(cont, L::oidLo(L::Kind::Index, 0, t), run->indexOclass, DAOS_OO_RO);
            std::vector<uint64_t> dkeys;
            index.listDkeys([&](uint64_t k) { dkeys.push_back(k); });
            if (dkeys.empty()) {
                std::cerr << "second " << t << " is marked but index[" << t << "] is empty" << std::endl;
                errors++;
            }
            for (uint64_t dk : dkeys) {
                size_t size = index.valueSize(dk, L::kAkey);
                tableBuf.resize(size);
                index.get(dk, L::kAkey, tableBuf.data(), &size);
                const auto tab = L::decodeTable(tableBuf.data(), size);
                const uint32_t slot = L::indexDkeySlot(dk);
                if (!tab || tab->slot != slot) {
                    std::cerr << "index[" << t << "] dkey " << dk << ": bad batch table" << std::endl;
                    errors++;
                    continue;
                }
                if (!batchKeys.insert({slot, L::indexDkeySeq(dk)}).second) {
                    std::cerr << "batch " << slot << "/" << L::indexDkeySeq(dk) << " indexed twice" << std::endl;
                    errors++;
                }
                auto& sum = perSlot[slot];
                sum.batches++;
                sum.firstSecond = std::min(sum.firstSecond, t);
                sum.lastSecond = std::max(sum.lastSecond, t);

                auto& arr = arrays[{slot, tab->tData}];
                if (!arr)
                    arr = std::make_unique<DaosArray>(cont, L::oidLo(L::Kind::Data, slot, tab->tData), run->oclass,
                                                      run->chunk, DAOS_OO_RO);
                data.resize(tab->length);
                if (tab->length)
                    arr->read(tab->offset, data.data(), tab->length);
                for (const auto& e : tab->entries) {
                    sum.events++;
                    sum.bytes += e.size;
                    if (e.offset + e.size > tab->length ||
                        (synth && !syntheticOk(data.data() + e.offset, e.size, e.evtNum))) {
                        sum.bad++;
                        if (sum.bad <= 5)
                            std::cerr << "event " << e.evtNum << "/" << e.dataId << " in slot " << slot
                                      << ": bytes differ" << std::endl;
                    }
                }
            }
        }
        for (const auto& [slot, s] : perSlot) {
            std::cout << "slot " << slot << ": " << s.batches << " batches, " << s.events << " events, " << s.bytes
                      << " bytes, index seconds " << s.firstSecond << ".." << s.lastSecond << ", bad " << s.bad
                      << std::endl;
            errors += s.bad > 0;
        }
    } catch (const DaosError& e) {
        std::cerr << "DAOS error: " << e.what() << std::endl;
        return 1;
    }
    std::cout << (errors ? "FAILED" : "OK") << std::endl;
    return errors ? 1 : 0;
}
