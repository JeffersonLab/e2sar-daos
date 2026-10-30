#pragma once
/**
    Per-run KV objects and the run catalog of layout v1.

    Legacy: layout v1, before one container per run (#31). The applications no longer use it;
    the run catalog is now Catalog (catalog.hpp). Only the record helpers (Record,
    formatRecord, parseRecord) are still used, by layout.hpp.

    Each run (key prefix) is written to its own KV object in the container. The object id
    comes from the prefix, so every receiver of a run (e.g. two nodes behind the LB) opens
    the same object without talking to the others:

        run object     oid.hi = kRunOidHi,     oid.lo = FNV-1a 64-bit hash of the prefix
        run catalog    oid.hi = kCatalogOidHi, oid.lo = 0
        legacy object  oid.hi = 0,             oid.lo = --oid (0xE25A): all runs in one
                       object, as written before per-run objects

    DAOS leaves the low 32 bits of oid.hi and all of oid.lo to the user; it sets the high
    32 bits (object type and class). Keys inside a run object keep the prefix
    (<prefix>/<evt>/<dataId>[/<chunk>], see event_layout.hpp), so the event layout and
    the reader's scan are the same for run objects and the legacy object.

    Catalog keys; values are records of "key=value" pairs separated by spaces:

        run/<prefix>          oid=<16 hex> oclass=<name or -> chunk=<bytes>
                              created=<unix s> host=<receiver that created the entry>
        oid/<16 hex>          <prefix>: claims the object id, so two prefixes with the
                              same hash are caught instead of sharing one object
        recv/<prefix>/<host>  that receiver's final statistics, written when it stops

    The first receiver of a run creates run/<prefix> with a conditional insert; the others
    find it and must use the same object class, since the class is part of the object id.
*/

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "e2sar_daos/daos_handles.hpp"

namespace e2sar_daos {

constexpr uint32_t kRunOidHi = 0xE25A0001;
constexpr uint32_t kCatalogOidHi = 0xE25A0000;
constexpr uint64_t kLegacyOidLo = 0xE25A;

/** FNV-1a, 64 bits: a stable hash, the same on every node and build. */
inline uint64_t prefixHash(const std::string& prefix) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : prefix) {
        h ^= c;
        h *= 0x100000001b3ull;
    }
    return h;
}

inline std::string hex16(uint64_t v) {
    static const char* digits = "0123456789abcdef";
    std::string s(16, '0');
    for (int i = 15; i >= 0; i--, v >>= 4)
        s[i] = digits[v & 0xf];
    return s;
}

inline std::string catalogRunKey(const std::string& prefix) { return "run/" + prefix; }
inline std::string catalogOidKey(uint64_t oidLo) { return "oid/" + hex16(oidLo); }
inline std::string catalogRecvKey(const std::string& prefix, const std::string& host) {
    return "recv/" + prefix + "/" + host;
}

/** A catalog value: "key=value" pairs separated by spaces. */
using Record = std::map<std::string, std::string>;

/** Spaces in a value become '_', so the record always parses back. */
inline std::string formatRecord(const Record& r) {
    std::string s;
    for (const auto& [k, v] : r) {
        if (!s.empty())
            s += ' ';
        std::string val = v.empty() ? "-" : v;
        for (auto& c : val)
            if (c == ' ')
                c = '_';
        s += k + "=" + val;
    }
    return s;
}

/** Words without '=' are skipped. */
inline Record parseRecord(const std::string& s) {
    Record r;
    size_t pos = 0;
    while (pos < s.size()) {
        const size_t end = std::min(s.find(' ', pos), s.size());
        const std::string word = s.substr(pos, end - pos);
        const size_t eq = word.find('=');
        if (eq != std::string::npos && eq > 0)
            r[word.substr(0, eq)] = word.substr(eq + 1);
        pos = end + 1;
    }
    return r;
}

/** One run in the catalog. */
struct RunInfo {
    std::string prefix;
    uint64_t oidLo = 0;
    std::string oclass;  // empty: the container default
    uint64_t chunk = 0;
    int64_t created = 0;  // unix seconds
    std::string host;     // the receiver that created the entry

    Record toRecord() const {
        return {{"oid", hex16(oidLo)}, {"oclass", oclass.empty() ? "-" : oclass},
                {"chunk", std::to_string(chunk)}, {"created", std::to_string(created)}, {"host", host}};
    }

    /** nullopt if the record has no valid oid. */
    static std::optional<RunInfo> fromRecord(const std::string& prefix, const Record& r) {
        RunInfo i;
        i.prefix = prefix;
        try {
            i.oidLo = std::stoull(r.at("oid"), nullptr, 16);
            if (r.count("chunk"))
                i.chunk = std::stoull(r.at("chunk"));
            if (r.count("created"))
                i.created = std::stoll(r.at("created"));
        } catch (const std::exception&) {
            return std::nullopt;
        }
        if (r.count("oclass") && r.at("oclass") != "-")
            i.oclass = r.at("oclass");
        if (r.count("host"))
            i.host = r.at("host");
        return i;
    }
};

/** The catalog object of one container. */
class RunCatalog {
public:
    explicit RunCatalog(const DaosContainer& cont, unsigned int mode = DAOS_OO_RW);

    /**
        Find or create the entry for `prefix`. Returns the entry and whether this call
        created it. A new entry records oclass, chunk and host. Throws DaosError
        (-DER_EXIST) if another prefix already holds the same object id.
    */
    std::pair<RunInfo, bool> registerRun(const std::string& prefix, const std::string& oclass,
                                         uint64_t chunk, const std::string& host);
    std::optional<RunInfo> find(const std::string& prefix);
    /** All runs, sorted by prefix. */
    std::vector<RunInfo> list();

    void putReceiver(const std::string& prefix, const std::string& host, const Record& stats);
    /** host -> final statistics, for the receivers of `prefix` that have stopped. */
    std::map<std::string, Record> receivers(const std::string& prefix);

    /** Remove the run's catalog entries (not its object). */
    void removeRun(const std::string& prefix);

private:
    std::optional<std::string> value(const std::string& key);
    std::vector<std::string> keysWithPrefix(const std::string& head);

    DaosKV kv_;
};

}  // namespace e2sar_daos
