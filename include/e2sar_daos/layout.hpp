#pragma once
/**
    The DAOS layout of a run (docs/DAOS_LAYOUT.md): object ids, catalog keys and records,
    and the binary batch table stored in index[T]. Header-only; no DAOS calls.

    One run per container. Object ids keep the user part in oid.lo as decimal digits,
    so `daos container list-objects` shows them readably:

        oid.lo = K SSSS TTTTTTTTTT  = kind × 10^14 + slot × 10^10 + T
*/

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "e2sar_daos/run_catalog.hpp"  // Record, formatRecord, parseRecord

namespace e2sar_daos::layout {

// ---------------------------------------------------------------- object ids

enum class Kind : uint64_t { Catalog = 1, Seconds = 2, Index = 3, Data = 4 };

constexpr uint64_t kKindMul = 100000000000000ull;  // 10^14
constexpr uint64_t kSlotMul = 10000000000ull;      // 10^10
constexpr uint32_t kMaxSlot = 9999;
constexpr uint64_t kMaxSecond = kSlotMul - 1;      // 10 digits: until the year 2286

/** The catalog's object class is fixed, so a reader can open it before knowing the run's settings. */
constexpr const char* kCatalogClass = "S1";

inline uint64_t oidLo(Kind kind, uint32_t slot = 0, uint64_t second = 0) {
    if (slot > kMaxSlot)
        throw std::out_of_range("writer slot > 9999");
    if (second > kMaxSecond)
        throw std::out_of_range("UTC second has more than 10 digits");
    return static_cast<uint64_t>(kind) * kKindMul + slot * kSlotMul + second;
}

struct OidParts {
    Kind kind;
    uint32_t slot;
    uint64_t second;
};

inline OidParts splitOidLo(uint64_t lo) {
    return {static_cast<Kind>(lo / kKindMul), static_cast<uint32_t>(lo % kKindMul / kSlotMul), lo % kSlotMul};
}

// ---------------------------------------------------------------- index keys

/** dkey of a batch in index[T]: the slot in the top 16 bits, the slot's batch number below. */
inline uint64_t indexDkey(uint32_t slot, uint64_t batchSeq) { return (uint64_t{slot} << 48) | (batchSeq & 0xFFFFFFFFFFFFull); }
inline uint32_t indexDkeySlot(uint64_t dkey) { return static_cast<uint32_t>(dkey >> 48); }
inline uint64_t indexDkeySeq(uint64_t dkey) { return dkey & 0xFFFFFFFFFFFFull; }

/** akey of index entries and of seconds marks. */
constexpr uint64_t kAkey = 0;

// ---------------------------------------------------------------- batch table

constexpr char kTableCheck[4] = {'E', '2', 'S', 'B'};  // "E2SAR batch"
constexpr size_t kTableHeaderSize = 40;
constexpr size_t kTableEntrySize = 32;

struct BatchEntry {
    uint64_t evtNum = 0;
    uint64_t offset = 0;  // from the batch start
    uint64_t size = 0;
    uint16_t dataId = 0;
    uint16_t flags = 0;
};

struct BatchTable {
    uint32_t slot = 0;
    uint64_t tData = 0;   // data[tData, slot]
    uint64_t offset = 0;  // byte offset of the batch in that array
    uint64_t length = 0;  // bytes from offset to the end of the last event
    std::vector<BatchEntry> entries;
};

namespace detail {
// Little endian on the wire, whatever the host.
inline void put(std::vector<uint8_t>& b, size_t at, uint64_t v, size_t bytes) {
    for (size_t i = 0; i < bytes; i++)
        b[at + i] = static_cast<uint8_t>(v >> (8 * i));
}
inline uint64_t get(const uint8_t* p, size_t bytes) {
    uint64_t v = 0;
    for (size_t i = 0; i < bytes; i++)
        v |= uint64_t{p[i]} << (8 * i);
    return v;
}
}  // namespace detail

inline size_t tableSize(size_t n) { return kTableHeaderSize + kTableEntrySize * n; }

/**
    header:  char[4] "E2SB" | u32 n | u16 slot | u16 reserved | u32 reserved |
             u64 tData | u64 offset | u64 length                       (40 bytes)
    entry:   u64 evtNum | u64 offset | u64 size | u16 dataId | u16 flags | u32 reserved (32 bytes)
*/
inline void encodeTable(const BatchTable& t, std::vector<uint8_t>& out) {
    using detail::put;
    out.assign(tableSize(t.entries.size()), 0);
    std::memcpy(out.data(), kTableCheck, 4);
    put(out, 4, t.entries.size(), 4);
    put(out, 8, t.slot, 2);
    put(out, 16, t.tData, 8);
    put(out, 24, t.offset, 8);
    put(out, 32, t.length, 8);
    size_t at = kTableHeaderSize;
    for (const auto& e : t.entries) {
        put(out, at, e.evtNum, 8);
        put(out, at + 8, e.offset, 8);
        put(out, at + 16, e.size, 8);
        put(out, at + 24, e.dataId, 2);
        put(out, at + 26, e.flags, 2);
        at += kTableEntrySize;
    }
}

/** nullopt unless the table starts with "E2SB" and has exactly 40 + 32 × n bytes. */
inline std::optional<BatchTable> decodeTable(const uint8_t* p, size_t size) {
    using detail::get;
    if (size < kTableHeaderSize || std::memcmp(p, kTableCheck, 4) != 0)
        return std::nullopt;
    const size_t n = get(p + 4, 4);
    if (size != tableSize(n))
        return std::nullopt;
    BatchTable t;
    t.slot = static_cast<uint32_t>(get(p + 8, 2));
    t.tData = get(p + 16, 8);
    t.offset = get(p + 24, 8);
    t.length = get(p + 32, 8);
    t.entries.resize(n);
    const uint8_t* e = p + kTableHeaderSize;
    for (auto& x : t.entries) {
        x.evtNum = get(e, 8);
        x.offset = get(e + 8, 8);
        x.size = get(e + 16, 8);
        x.dataId = static_cast<uint16_t>(get(e + 24, 2));
        x.flags = static_cast<uint16_t>(get(e + 26, 2));
        e += kTableEntrySize;
    }
    return t;
}

// ---------------------------------------------------------------- catalog

inline std::string runKey() { return "run"; }
inline std::string writerKey(uint32_t slot) { return "writer/" + std::to_string(slot); }
inline std::string wmKey(uint32_t slot) { return "wm/" + std::to_string(slot); }
inline std::string doneKey(uint32_t slot) { return "done/" + std::to_string(slot); }

/** The `run` record: settings every writer of the run must share. */
struct RunSettings {
    uint64_t start = 0;  // UTC second of the first writer's start
    uint64_t chunk = 0;  // bytes
    std::string oclass;       // data arrays
    std::string indexOclass;  // seconds and index objects
    double batchThreshold = 0.9;

    Record toRecord() const {
        char th[32];
        std::snprintf(th, sizeof(th), "%g", batchThreshold);
        return {{"start", std::to_string(start)}, {"chunk", std::to_string(chunk)}, {"oclass", oclass},
                {"index_oclass", indexOclass}, {"batch_threshold", th}};
    }

    static std::optional<RunSettings> fromRecord(const Record& r) {
        RunSettings s;
        try {
            s.start = std::stoull(r.at("start"));
            s.chunk = std::stoull(r.at("chunk"));
            s.oclass = r.at("oclass");
            s.indexOclass = r.at("index_oclass");
            if (r.count("batch_threshold"))
                s.batchThreshold = std::stod(r.at("batch_threshold"));
        } catch (const std::exception&) {
            return std::nullopt;
        }
        if (s.chunk == 0)
            return std::nullopt;
        return s;
    }

    /** Empty if a writer with `mine` can join this run; otherwise what differs. */
    std::string mismatch(const RunSettings& mine) const {
        std::string d;
        if (chunk != mine.chunk)
            d += " chunk " + std::to_string(chunk) + " (yours " + std::to_string(mine.chunk) + ")";
        if (oclass != mine.oclass)
            d += " oclass " + oclass + " (yours " + mine.oclass + ")";
        if (indexOclass != mine.indexOclass)
            d += " index_oclass " + indexOclass + " (yours " + mine.indexOclass + ")";
        return d;
    }
};

/** `wm/<slot>`: every index write started before `index` and every batch opened before `data` is done. */
struct Watermark {
    uint64_t index = 0;
    uint64_t data = 0;

    Record toRecord() const { return {{"index", std::to_string(index)}, {"data", std::to_string(data)}}; }
    static std::optional<Watermark> fromRecord(const Record& r) {
        try {
            return Watermark{std::stoull(r.at("index")), std::stoull(r.at("data"))};
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }
};

// ---------------------------------------------------------------- batches

/** Chunks a batch of `bytes` takes; at least 1 unless empty. */
inline uint64_t chunksFor(uint64_t bytes, uint64_t chunk) { return (bytes + chunk - 1) / chunk; }

}  // namespace e2sar_daos::layout
