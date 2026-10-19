#pragma once
/**
    How one E2SAR event is laid out in a DAOS KV object, shared by writer and reader.

        <prefix>/<evtNum>/<dataId>/<i>   chunk i of the payload, i = 0..nchunks-1
        <prefix>/<evtNum>/<dataId>       metadata: "size=<bytes> chunk=<bytes> nchunks=<n>"

    Chunk i covers bytes [i*chunk, min((i+1)*chunk, size)). The metadata key is written
    last, only after all chunks succeeded, so it doubles as the commit marker.
*/

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

namespace e2sar_daos {

inline std::string eventKey(const std::string& prefix, uint64_t evtNum, uint16_t dataId) {
    return prefix + "/" + std::to_string(evtNum) + "/" + std::to_string(dataId);
}

inline std::string chunkKey(const std::string& eventKey, size_t idx) {
    return eventKey + "/" + std::to_string(idx);
}

struct EventMeta {
    uint64_t size = 0;
    uint64_t chunk = 0;
    uint64_t nchunks = 0;

    /** Chunking for an event of `size` bytes; chunkSize 0 means one value per event. */
    static EventMeta forSize(uint64_t size, uint64_t chunkSize) {
        EventMeta m;
        m.size = size;
        m.chunk = (chunkSize == 0 || chunkSize > size) ? (size == 0 ? 1 : size) : chunkSize;
        m.nchunks = size == 0 ? 0 : (size + m.chunk - 1) / m.chunk;
        return m;
    }

    std::string toString() const {
        return "size=" + std::to_string(size) + " chunk=" + std::to_string(chunk) +
               " nchunks=" + std::to_string(nchunks);
    }

    static std::optional<EventMeta> parse(const std::string& s) {
        EventMeta m;
        unsigned long long sz, ch, n;
        if (std::sscanf(s.c_str(), "size=%llu chunk=%llu nchunks=%llu", &sz, &ch, &n) != 3)
            return std::nullopt;
        m.size = sz;
        m.chunk = ch;
        m.nchunks = n;
        if (m.chunk == 0 || m.nchunks != (m.size == 0 ? 0 : (m.size + m.chunk - 1) / m.chunk))
            return std::nullopt;
        return m;
    }
};

}  // namespace e2sar_daos
