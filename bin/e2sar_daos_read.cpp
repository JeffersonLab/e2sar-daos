/**
    e2sar_daos_read: read back events written by e2sar_daos_recv.

    Modes:
      (default)   list committed events under a key prefix, plus any incomplete ones
      --fetch     read the selected events chunk by chunk, check sizes, print SHA-256
                  in `sha256sum` format and whether the event is an EVIO file; with
                  --out DIR also write each event to DIR/<prefix>_<evtNum>_<dataId>.evio

    e2sar_ft sends each file as one event, so a restored event is byte-for-byte the
    original file (e.g. an EVIO v6 file ready for an EVIO viewer). Compare the printed
    digests with `sha256sum` of the files given to e2sar_ft -s.
    Opens pool and container read-only and never creates anything.
*/

#include <openssl/evp.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <boost/program_options.hpp>

#include "e2sar_daos/daos_handles.hpp"
#include "e2sar_daos/event_layout.hpp"

namespace po = boost::program_options;
using namespace e2sar_daos;

namespace {

using EventId = std::pair<uint64_t, uint16_t>;  // (evtNum, dataId)

struct Listing {
    std::map<EventId, EventMeta> committed;
    std::map<EventId, size_t> chunksSeen;  // number of chunk keys found per event
    size_t badMeta = 0;
};

bool parseUInt(const std::string& s, uint64_t* out) {
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos)
        return false;
    *out = std::stoull(s);
    return true;
}

// Split "<prefix>/<evt>/<dataId>[/<chunk>]" and classify into the listing.
Listing scan(DaosKV& kv, const std::string& prefix) {
    Listing l;
    const std::string head = prefix + "/";
    std::vector<std::string> metaKeys;

    kv.listKeys([&](const std::string& key) {
        if (key.compare(0, head.size(), head) != 0)
            return;
        std::vector<std::string> parts;
        size_t start = head.size(), pos;
        while ((pos = key.find('/', start)) != std::string::npos) {
            parts.push_back(key.substr(start, pos - start));
            start = pos + 1;
        }
        parts.push_back(key.substr(start));

        uint64_t evt, did, idx;
        if (parts.size() < 2 || parts.size() > 3 || !parseUInt(parts[0], &evt) ||
            !parseUInt(parts[1], &did) || did > UINT16_MAX)
            return;
        const EventId id{evt, static_cast<uint16_t>(did)};
        if (parts.size() == 3 && parseUInt(parts[2], &idx))
            l.chunksSeen[id]++;
        else if (parts.size() == 2)
            metaKeys.push_back(key);
    });

    for (const auto& key : metaKeys) {
        std::string val(256, '\0');
        size_t size = val.size();
        kv.get(key, val.data(), &size);
        val.resize(std::min(size, val.size()));
        auto meta = EventMeta::parse(val);
        const auto slash2 = key.rfind('/');
        const auto slash1 = key.rfind('/', slash2 - 1);
        const EventId id{std::stoull(key.substr(slash1 + 1, slash2 - slash1 - 1)),
                         static_cast<uint16_t>(std::stoul(key.substr(slash2 + 1)))};
        if (meta)
            l.committed[id] = *meta;
        else
            l.badMeta++;
    }
    return l;
}

// EVIO v4 block headers and v6 file/record headers keep the magic word 0xc0da0100 at
// word 7 and the version in the low byte of word 5; the magic's byte order gives the
// file's endianness.
std::string describeEvio(const unsigned char* p, size_t n) {
    if (n < 32)
        return "not EVIO";
    auto word = [p](int i, bool be) {
        const unsigned char* b = p + 4 * i;
        return be ? (uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3])
                  : (uint32_t(b[3]) << 24 | uint32_t(b[2]) << 16 | uint32_t(b[1]) << 8 | b[0]);
    };
    for (bool be : {true, false})
        if (word(7, be) == 0xc0da0100)
            return "EVIO v" + std::to_string(word(5, be) & 0xff) + (be ? " big-endian" : " little-endian");
    return "not EVIO";
}

std::string toHex(const unsigned char* md, unsigned int len) {
    std::string hex;
    char b[3];
    for (unsigned int i = 0; i < len; i++) {
        std::snprintf(b, sizeof(b), "%02x", md[i]);
        hex += b;
    }
    return hex;
}

// Read one event chunk by chunk. Returns false (and prints why) on any mismatch.
bool fetchEvent(DaosKV& kv, const std::string& prefix, const EventId& id, const EventMeta& meta,
                const std::string& outDir, const std::string& extension) {
    const std::string base = eventKey(prefix, id.first, id.second);
    const std::string name = prefix + "_" + std::to_string(id.first) + "_" + std::to_string(id.second) +
                             (extension.empty() ? "" : "." + extension);
    std::string kind = "empty";

    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> md(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    EVP_DigestInit_ex(md.get(), EVP_sha256(), nullptr);

    std::ofstream out;
    if (!outDir.empty()) {
        out.open(outDir + "/" + name, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::cerr << "cannot create " << outDir << "/" << name << std::endl;
            return false;
        }
    }

    std::vector<char> buf(meta.chunk);
    for (uint64_t i = 0; i < meta.nchunks; i++) {
        const uint64_t expect = std::min<uint64_t>(meta.chunk, meta.size - i * meta.chunk);
        size_t size = buf.size();
        kv.get(chunkKey(base, i), buf.data(), &size);
        if (size != expect) {
            std::cerr << base << ": chunk " << i << " is " << size << " bytes, expected " << expect
                      << (size == 0 ? " (missing)" : "") << std::endl;
            return false;
        }
        if (i == 0)
            kind = describeEvio(reinterpret_cast<const unsigned char*>(buf.data()), size);
        EVP_DigestUpdate(md.get(), buf.data(), size);
        if (out)
            out.write(buf.data(), static_cast<std::streamsize>(size));
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int dlen = 0;
    EVP_DigestFinal_ex(md.get(), digest, &dlen);
    std::cout << toHex(digest, dlen) << "  " << name << "  (" << meta.size << " bytes, "
              << meta.nchunks << " chunks, " << kind << ")" << std::endl;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    po::options_description od("Command-line options");
    std::string pool, cont, oclass, prefix, outDir, extension;
    uint64_t oidLo;
    std::vector<uint64_t> events;
    int dataIdFilter;

    auto opts = od.add_options();
    opts("help,h", "show this help message");
    opts("pool", po::value<std::string>(&pool)->default_value("sc26"), "DAOS pool label");
    opts("cont", po::value<std::string>(&cont)->default_value("e2sar"), "DAOS container label");
    opts("oid", po::value<uint64_t>(&oidLo)->default_value(0xE25A), "low 64 bits of the KV object id (as given to e2sar_daos_recv)");
    opts("oclass", po::value<std::string>(&oclass)->default_value(""), "object class used by e2sar_daos_recv, if set");
    opts("prefix", po::value<std::string>(&prefix)->required(), "key prefix used by e2sar_daos_recv");
    opts("fetch", po::bool_switch()->default_value(false), "read selected events back and print SHA-256");
    opts("event", po::value<std::vector<uint64_t>>(&events)->multitoken(), "only these event numbers (default: all)");
    opts("dataid", po::value<int>(&dataIdFilter)->default_value(-1), "only this data id (default: all)");
    opts("out,o", po::value<std::string>(&outDir), "with --fetch: also write events as files into this directory");
    opts("extension,e", po::value<std::string>(&extension)->default_value("evio"), "file extension for --out files (empty for none)");

    po::variables_map vm;
    try {
        po::store(po::parse_command_line(argc, argv, od), vm);
        if (vm.count("help")) {
            std::cout << od << std::endl
                      << "Examples:\n"
                      << "  e2sar_daos_read --prefix run42                      # list events\n"
                      << "  e2sar_daos_read --prefix run42 --fetch              # verify all, print sha256\n"
                      << "  e2sar_daos_read --prefix run42 --fetch -o ~/restored   # write run42_<evt>_<dataId>.evio" << std::endl;
            return 0;
        }
        po::notify(vm);
    } catch (const po::error& e) {
        std::cerr << "Unable to parse command line: " << e.what() << std::endl;
        return 1;
    }
    if (vm.count("out") && !vm["fetch"].as<bool>()) {
        std::cerr << "--out requires --fetch" << std::endl;
        return 1;
    }

    try {
        DaosRuntime daos;
        DaosPool daosPool(pool, DAOS_PC_RO);
        DaosContainer daosCont(daosPool, cont, /*createIfMissing=*/false, DAOS_COO_RO);
        DaosKV kv(daosCont, oidLo, 0, oclass, DAOS_OO_RO);

        Listing l = scan(kv, prefix);
        const std::set<uint64_t> wanted(events.begin(), events.end());
        auto selected = [&](const EventId& id) {
            return (wanted.empty() || wanted.count(id.first)) &&
                   (dataIdFilter < 0 || id.second == dataIdFilter);
        };

        if (!vm["fetch"].as<bool>()) {
            size_t n = 0;
            uint64_t bytes = 0;
            std::cout << "evtNum\tdataId\tbytes\tchunks\tchunk_size" << std::endl;
            for (const auto& [id, meta] : l.committed) {
                if (!selected(id))
                    continue;
                std::cout << id.first << "\t" << id.second << "\t" << meta.size << "\t"
                          << meta.nchunks << "\t" << meta.chunk << std::endl;
                n++;
                bytes += meta.size;
            }
            std::cout << n << " committed events, " << bytes << " bytes under '" << prefix << "/'" << std::endl;

            size_t incomplete = 0;
            for (const auto& [id, seen] : l.chunksSeen)
                if (selected(id) && !l.committed.count(id)) {
                    if (incomplete++ == 0)
                        std::cout << "Incomplete (chunks without metadata):" << std::endl;
                    std::cout << "  " << id.first << "/" << id.second << ": " << seen << " chunks" << std::endl;
                }
            if (l.badMeta)
                std::cout << l.badMeta << " unparsable metadata values" << std::endl;
            return 0;
        }

        size_t ok = 0, bad = 0;
        for (const auto& [id, meta] : l.committed) {
            if (!selected(id))
                continue;
            (fetchEvent(kv, prefix, id, meta, outDir, extension) ? ok : bad)++;
        }
        for (uint64_t e : wanted) {
            bool found = false;
            for (const auto& entry : l.committed)
                found |= entry.first.first == e;
            if (!found) {
                std::cerr << "event " << e << " not found under '" << prefix << "/'" << std::endl;
                bad++;
            }
        }
        std::cout << ok << " events verified, " << bad << " failed" << std::endl;
        return bad == 0 ? 0 : 2;
    } catch (const DaosError& e) {
        std::cerr << "DAOS error: " << e.what() << std::endl;
        return 1;
    }
}
