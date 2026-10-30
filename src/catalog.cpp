#include "e2sar_daos/catalog.hpp"

#include <algorithm>

namespace e2sar_daos {

using namespace layout;

Catalog::Catalog(const DaosContainer& cont, unsigned int mode)
    : kv_(cont, oidLo(Kind::Catalog), 0, kCatalogClass, mode) {}

std::optional<std::string> Catalog::value(const std::string& key) {
    size_t size = kv_.valueSize(key);
    if (size == 0)
        return std::nullopt;
    std::string s(size, '\0');
    kv_.get(key, s.data(), &size);
    s.resize(size);
    return s;
}

std::pair<RunSettings, bool> Catalog::createOrJoin(const RunSettings& mine) {
    const std::string rec = formatRecord(mine.toRecord());
    if (kv_.putIfAbsent(runKey(), rec.data(), rec.size()))
        return {mine, true};
    const auto existing = run();
    if (!existing)
        throw DaosError("the container's run record is unreadable", -DER_INVAL);
    const std::string diff = existing->mismatch(mine);
    if (!diff.empty())
        throw DaosError("this run was started with different settings:" + diff, -DER_MISMATCH);
    return {*existing, false};
}

std::optional<RunSettings> Catalog::run() {
    const auto v = value(runKey());
    if (!v)
        return std::nullopt;
    return RunSettings::fromRecord(parseRecord(*v));
}

uint32_t Catalog::claimSlot(uint64_t startSecond) {
    const std::string rec = formatRecord({{"started", std::to_string(startSecond)}});
    for (uint32_t slot = 0; slot <= kMaxSlot; slot++)
        if (kv_.putIfAbsent(writerKey(slot), rec.data(), rec.size()))
            return slot;
    throw DaosError("all writer slots (0-9999) are taken", -DER_NOSPACE);
}

void Catalog::putWatermark(uint32_t slot, const Watermark& wm) {
    const std::string rec = formatRecord(wm.toRecord());
    kv_.put(wmKey(slot), rec.data(), rec.size());
}

void Catalog::putDone(uint32_t slot, const Record& stats) {
    const std::string rec = formatRecord(stats);
    kv_.put(doneKey(slot), rec.data(), rec.size());
}

std::vector<uint32_t> Catalog::slots() {
    std::vector<uint32_t> out;
    kv_.listKeys([&](const std::string& k) {
        if (k.rfind("writer/", 0) == 0) {
            try {
                out.push_back(static_cast<uint32_t>(std::stoul(k.substr(7))));
            } catch (const std::exception&) {}
        }
    });
    std::sort(out.begin(), out.end());
    return out;
}

std::optional<Watermark> Catalog::watermark(uint32_t slot) {
    const auto v = value(wmKey(slot));
    if (!v)
        return std::nullopt;
    return Watermark::fromRecord(parseRecord(*v));
}

std::optional<Record> Catalog::done(uint32_t slot) {
    const auto v = value(doneKey(slot));
    if (!v)
        return std::nullopt;
    return parseRecord(*v);
}

}  // namespace e2sar_daos
