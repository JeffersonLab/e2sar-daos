#include "e2sar_daos/run_catalog.hpp"

#include <algorithm>
#include <ctime>

namespace e2sar_daos {

RunCatalog::RunCatalog(const DaosContainer& cont, unsigned int mode)
    : kv_(cont, 0, kCatalogOidHi, "", mode) {}

std::optional<std::string> RunCatalog::value(const std::string& key) {
    const size_t size = kv_.valueSize(key);
    if (size == 0)
        return std::nullopt;
    std::string v(size, '\0');
    size_t got = size;
    kv_.get(key, v.data(), &got);
    v.resize(std::min(got, size));
    return v;
}

std::vector<std::string> RunCatalog::keysWithPrefix(const std::string& head) {
    std::vector<std::string> keys;
    kv_.listKeys([&](const std::string& key) {
        if (key.compare(0, head.size(), head) == 0)
            keys.push_back(key);
    });
    return keys;
}

std::pair<RunInfo, bool> RunCatalog::registerRun(const std::string& prefix, const std::string& oclass,
                                                 uint64_t chunk, const std::string& host) {
    const uint64_t oidLo = prefixHash(prefix);

    // Claim the object id first: a different prefix with the same hash must not share it.
    const std::string oidKey = catalogOidKey(oidLo);
    if (!kv_.putIfAbsent(oidKey, prefix.data(), prefix.size())) {
        const auto owner = value(oidKey);
        if (owner && *owner != prefix)
            throw DaosError("run '" + prefix + "' has the same object id as run '" + *owner +
                                "' (hash collision): use another --prefix",
                            -DER_EXIST);
    }

    RunInfo info;
    info.prefix = prefix;
    info.oidLo = oidLo;
    info.oclass = oclass;
    info.chunk = chunk;
    info.created = static_cast<int64_t>(::time(nullptr));
    info.host = host;
    const std::string rec = formatRecord(info.toRecord());
    if (kv_.putIfAbsent(catalogRunKey(prefix), rec.data(), rec.size()))
        return {info, true};

    // Another receiver created it first (or this prefix was used before).
    auto existing = find(prefix);
    if (!existing)
        throw DaosError("catalog entry for run '" + prefix + "' exists but can't be read", -DER_INVAL);
    return {*existing, false};
}

std::optional<RunInfo> RunCatalog::find(const std::string& prefix) {
    const auto v = value(catalogRunKey(prefix));
    if (!v)
        return std::nullopt;
    return RunInfo::fromRecord(prefix, parseRecord(*v));
}

std::vector<RunInfo> RunCatalog::list() {
    std::vector<RunInfo> runs;
    for (const auto& key : keysWithPrefix("run/")) {
        const std::string prefix = key.substr(4);
        if (auto v = value(key))
            if (auto info = RunInfo::fromRecord(prefix, parseRecord(*v)))
                runs.push_back(*info);
    }
    std::sort(runs.begin(), runs.end(), [](const RunInfo& a, const RunInfo& b) { return a.prefix < b.prefix; });
    return runs;
}

void RunCatalog::putReceiver(const std::string& prefix, const std::string& host, const Record& stats) {
    const std::string rec = formatRecord(stats);
    kv_.put(catalogRecvKey(prefix, host), rec.data(), rec.size());
}

std::map<std::string, Record> RunCatalog::receivers(const std::string& prefix) {
    std::map<std::string, Record> out;
    const std::string head = "recv/" + prefix + "/";
    for (const auto& key : keysWithPrefix(head))
        if (auto v = value(key))
            out[key.substr(head.size())] = parseRecord(*v);
    return out;
}

void RunCatalog::removeRun(const std::string& prefix) {
    for (const auto& key : keysWithPrefix("recv/" + prefix + "/"))
        kv_.remove(key);
    // Release the object id only if this run holds it.
    const std::string oidKey = catalogOidKey(prefixHash(prefix));
    if (const auto owner = value(oidKey); owner && *owner == prefix)
        kv_.remove(oidKey);
    kv_.remove(catalogRunKey(prefix));
}

}  // namespace e2sar_daos
