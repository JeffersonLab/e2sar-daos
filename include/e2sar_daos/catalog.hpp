#pragma once
/**
    The catalog of a run (docs/DAOS_LAYOUT.md#catalog): one KV object per container,
    oid.lo = 1 0000 0000000000, fixed class layout::kCatalogClass.

        run            settings shared by every writer (start, chunk, classes, threshold)
        writer/<slot>  a writer exists; claimed with a conditional insert
        wm/<slot>      that writer's watermarks, once a second
        done/<slot>    that writer's final statistics, when it stops
*/

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "e2sar_daos/daos_handles.hpp"
#include "e2sar_daos/layout.hpp"

namespace e2sar_daos {

class Catalog {
public:
    explicit Catalog(const DaosContainer& cont, unsigned int mode = DAOS_OO_RW);

    /**
        Create the run record with `mine`, or read the one another writer created.
        Returns the run's settings and whether this call created them. Throws DaosError
        (-DER_MISMATCH) if the existing settings differ from `mine` in chunk or classes.
    */
    std::pair<layout::RunSettings, bool> createOrJoin(const layout::RunSettings& mine);

    std::optional<layout::RunSettings> run();

    /** Claim the first free writer slot 0, 1, 2, … Throws if all 10,000 are taken. */
    uint32_t claimSlot(uint64_t startSecond);

    void putWatermark(uint32_t slot, const layout::Watermark& wm);
    void putDone(uint32_t slot, const Record& stats);

    /** Claimed slots, sorted. */
    std::vector<uint32_t> slots();
    std::optional<layout::Watermark> watermark(uint32_t slot);
    std::optional<Record> done(uint32_t slot);

private:
    std::optional<std::string> value(const std::string& key);
    DaosKV kv_;
};

}  // namespace e2sar_daos
