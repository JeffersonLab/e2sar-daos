#pragma once
/**
    Reading a run written by e2sar_daos_recv (docs/DAOS_LAYOUT.md), for e2sar_daos_read.

    RunReader opens the objects of one run and reads them asynchronously on one DAOS
    event queue:
      - listSeconds():   the seconds that have index entries (the `seconds` object)
      - readIndex(T…):   every batch table committed in those seconds
      - read(ranges):    byte ranges of data arrays, in pieces of at most readSize,
                         handed to a callback as each piece arrives (in any order)
      - removal:         index[T], seconds[T] and data[T, s], for --live-rm

    Single-threaded: one RunReader per thread.
*/

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "e2sar_daos/async_ops.hpp"
#include "e2sar_daos/daos_handles.hpp"
#include "e2sar_daos/daos_objects.hpp"
#include "e2sar_daos/layout.hpp"

namespace e2sar_daos {

/** One batch as found in index[second]. */
struct IndexedBatch {
    uint64_t second = 0;  // the index second (commit second)
    uint64_t dkey = 0;
    layout::BatchTable table;
};

/** A byte range of data[tData, slot]. */
struct DataRange {
    uint32_t slot = 0;
    uint64_t tData = 0;
    uint64_t offset = 0;
    uint64_t length = 0;
};

/** One piece of a range as it arrives: bytes [at, at + len) of ranges[range]. */
struct DataPiece {
    size_t range = 0;
    uint64_t at = 0;
    const uint8_t* data = nullptr;
    size_t len = 0;
};

class RunReader {
public:
    RunReader(const DaosContainer& cont, const layout::RunSettings& run, unsigned int mode, size_t inflight,
              size_t readSize);

    const layout::RunSettings& run() const noexcept { return run_; }

    /** Seconds with index entries, sorted. */
    std::vector<uint64_t> listSeconds();

    /** Every batch committed in these seconds. Throws DaosError if an index can't be read. */
    std::vector<IndexedBatch> readIndex(const std::vector<uint64_t>& seconds);

    /**
        Read every range; fn gets each piece as it completes. Returns the indexes of ranges
        that failed (after one retry per piece); their other pieces may have been delivered.
    */
    std::vector<size_t> read(const std::vector<DataRange>& ranges, const std::function<void(const DataPiece&)>& fn);

    /** --live-rm: remove index[T] and seconds[T]. */
    void removeSecond(uint64_t t);
    /** --live-rm: remove data[T, slot]. */
    void removeData(uint64_t t, uint32_t slot);

private:
    DaosArray& array(uint32_t slot, uint64_t t);

    const DaosContainer& cont_;
    layout::RunSettings run_;
    unsigned int mode_;
    size_t readSize_;
    DaosIntObject seconds_;
    std::map<std::pair<uint32_t, uint64_t>, std::unique_ptr<DaosArray>> arrays_;
    AsyncOps ops_;
};

}  // namespace e2sar_daos
