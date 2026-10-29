/**
    e2sar_daos_read: read back events written by e2sar_daos_recv.

    Modes:
      --prefixes  list the runs in the container's run catalog, with each run's
                  receivers' final statistics
      --prefix P  list run P's committed events, plus any incomplete ones and the
                  receivers' statistics
      --fetch     read the selected events chunk by chunk, check sizes, print SHA-256
                  in `sha256sum` format and whether the event is an EVIO file; with
                  --out DIR also write each event to DIR/<prefix>_<evtNum>_<dataId>.evio
      --bundle N  with --fetch --out: write N events per file instead, in (evtNum, dataId)
                  order, as DIR/<prefix>_<k>.evio plus an index <prefix>_<k>.idx
                  ("evtNum dataId offset size") and <prefix>_missing.txt (event number
                  gaps per dataId). For many small events, e.g. one streaming frame each
      --delete    delete run P: its object and its catalog entries

    A run is found through the run catalog (e2sar_daos/run_catalog.hpp). A prefix that
    isn't in the catalog, or --oid, reads the one shared object written before per-run
    objects (default id 0xE25A), so older containers still read.

    e2sar_ft sends each file as one event, so a restored event is byte-for-byte the
    original file (e.g. an EVIO v6 file ready for an EVIO viewer). Compare the printed
    digests with `sha256sum` of the files given to e2sar_ft -s.
    --pool is required: the reader never assumes a pool.
    Opens pool and container read-only and never creates anything, except with --delete.
*/

#include <openssl/evp.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
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
#include "e2sar_daos/run_catalog.hpp"

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

using Digest = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

Digest newDigest() {
    Digest md(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    EVP_DigestInit_ex(md.get(), EVP_sha256(), nullptr);
    return md;
}

std::string finishDigest(Digest& md) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int dlen = 0;
    EVP_DigestFinal_ex(md.get(), digest, &dlen);
    return toHex(digest, dlen);
}

// Read one event's chunks in order into buf (resized to the event size). Returns false
// (and prints why) if a chunk is missing or has the wrong size.
bool readEvent(DaosKV& kv, const std::string& prefix, const EventId& id, const EventMeta& meta,
               std::vector<char>& buf) {
    const std::string base = eventKey(prefix, id.first, id.second);
    buf.resize(meta.size);
    for (uint64_t i = 0; i < meta.nchunks; i++) {
        const uint64_t expect = std::min<uint64_t>(meta.chunk, meta.size - i * meta.chunk);
        size_t size = expect;
        try {
            kv.get(chunkKey(base, i), buf.data() + i * meta.chunk, &size);
        } catch (const DaosError& e) {  // e.g. a stored chunk larger than the metadata says
            std::cerr << base << ": chunk " << i << ": " << e.what() << std::endl;
            return false;
        }
        if (size != expect) {
            std::cerr << base << ": chunk " << i << " is " << size << " bytes, expected " << expect
                      << (size == 0 ? " (missing)" : "") << std::endl;
            return false;
        }
    }
    return true;
}

// Read one event chunk by chunk. Returns false (and prints why) on any mismatch.
bool fetchEvent(DaosKV& kv, const std::string& prefix, const EventId& id, const EventMeta& meta,
                const std::string& outDir, const std::string& extension) {
    const std::string base = eventKey(prefix, id.first, id.second);
    const std::string name = prefix + "_" + std::to_string(id.first) + "_" + std::to_string(id.second) +
                             (extension.empty() ? "" : "." + extension);
    std::string kind = "empty";

    Digest md = newDigest();

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

    std::cout << finishDigest(md) << "  " << name << "  (" << meta.size << " bytes, "
              << meta.nchunks << " chunks, " << kind << ")" << std::endl;
    return true;
}

std::string bundleName(const std::string& prefix, uint64_t k) {
    char num[16];
    std::snprintf(num, sizeof(num), "%06llu", static_cast<unsigned long long>(k));
    return prefix + "_" + num;
}

/*
    --bundle N: write the selected events, in (evtNum, dataId) order, N per file instead of
    one file per event:
      <prefix>_<k>.<ext>   the events' payloads back to back, as stored
      <prefix>_<k>.idx     "evtNum dataId offset size" per event in that file
      <prefix>_missing.txt per dataId, the event numbers missing between its first and last
    An event that can't be read is left out of its file and counted as failed.
    Returns the number of events that couldn't be read.
*/
size_t writeBundles(DaosKV& kv, const std::string& prefix, const std::vector<std::pair<EventId, EventMeta>>& sel,
                    uint64_t perFile, const std::string& outDir, const std::string& extension, size_t* ok) {
    size_t bad = 0;
    std::vector<char> buf;
    for (size_t first = 0, k = 0; first < sel.size(); first += perFile, k++) {
        const size_t last = std::min<size_t>(first + perFile, sel.size());
        const std::string name = bundleName(prefix, k);
        const std::string dataName = name + (extension.empty() ? "" : "." + extension);
        std::ofstream data(outDir + "/" + dataName, std::ios::binary | std::ios::trunc);
        std::ofstream idx(outDir + "/" + name + ".idx", std::ios::trunc);
        if (!data || !idx) {
            std::cerr << "cannot create " << outDir << "/" << name << ".*" << std::endl;
            return bad + (sel.size() - first);
        }
        idx << "# evtNum dataId offset size" << std::endl;
        Digest md = newDigest();
        uint64_t offset = 0;
        size_t inFile = 0;
        for (size_t i = first; i < last; i++) {
            const auto& [id, meta] = sel[i];
            if (!readEvent(kv, prefix, id, meta, buf)) {
                bad++;
                continue;
            }
            data.write(buf.data(), static_cast<std::streamsize>(buf.size()));
            EVP_DigestUpdate(md.get(), buf.data(), buf.size());
            idx << id.first << " " << id.second << " " << offset << " " << buf.size() << "\n";
            offset += buf.size();
            inFile++;
        }
        data.close();
        idx.close();
        if (!data || !idx) {
            std::cerr << "write to " << outDir << "/" << name << ".* failed" << std::endl;
            return bad + inFile;
        }
        *ok += inFile;
        std::cout << finishDigest(md) << "  " << dataName << "  (" << inFile << " events, evtNum "
                  << sel[first].first.first << ".." << sel[last - 1].first.first << ", " << offset
                  << " bytes)" << std::endl;
    }

    // Gaps per dataId between its first and last event number.
    std::map<uint16_t, std::vector<uint64_t>> byId;
    for (const auto& [id, meta] : sel)
        byId[id.second].push_back(id.first);
    std::ofstream miss(outDir + "/" + prefix + "_missing.txt", std::ios::trunc);
    miss << "# dataId first_missing last_missing count" << std::endl;
    for (auto& [did, nums] : byId) {
        std::sort(nums.begin(), nums.end());
        uint64_t missing = 0;
        for (size_t i = 1; i < nums.size(); i++)
            if (nums[i] > nums[i - 1] + 1) {
                const uint64_t n = nums[i] - nums[i - 1] - 1;
                miss << did << " " << nums[i - 1] + 1 << " " << nums[i] - 1 << " " << n << "\n";
                missing += n;
            }
        std::cout << "dataId " << did << ": evtNum " << nums.front() << ".." << nums.back() << ", "
                  << nums.size() << " events, " << missing << " missing" << std::endl;
    }
    return bad;
}

std::string fmtTime(int64_t unixSec) {
    if (unixSec <= 0)
        return "-";
    const std::time_t t = static_cast<std::time_t>(unixSec);
    std::tm tmLocal{};
    localtime_r(&t, &tmLocal);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmLocal);
    return buf;
}

uint64_t field(const Record& r, const char* key) {
    const auto it = r.find(key);
    if (it == r.end())
        return 0;
    try {
        return std::stoull(it->second);
    } catch (const std::exception&) {
        return 0;
    }
}

// One line per receiver that has stopped, from the run catalog. No tabs: scripts take the
// tab-separated lines of a listing as events.
void printReceivers(const std::map<std::string, Record>& recv) {
    for (const auto& [host, r] : recv)
        std::cout << "Receiver " << host << ": committed " << field(r, "committed") << " events, "
                  << field(r, "bytes") << " bytes, failed " << field(r, "failed") << ", reassembly loss "
                  << field(r, "reassembly_loss") << ", enqueue loss " << field(r, "enqueue_loss")
                  << ", stopped " << fmtTime(static_cast<int64_t>(field(r, "stop"))) << std::endl;
}

// --prefixes: one tab-separated row per run in the catalog.
void printRuns(RunCatalog& catalog, const std::string& where) {
    const auto runs = catalog.list();
    std::cout << "prefix\tcreated\tcreated_by\toclass\tchunk\treceivers\tcommitted\tfailed\tlost" << std::endl;
    for (const auto& run : runs) {
        uint64_t committed = 0, failed = 0, lost = 0;
        const auto recv = catalog.receivers(run.prefix);
        for (const auto& [host, r] : recv) {
            committed += field(r, "committed");
            failed += field(r, "failed");
            lost += field(r, "reassembly_loss") + field(r, "enqueue_loss");
        }
        std::cout << run.prefix << "\t" << fmtTime(run.created) << "\t" << run.host << "\t"
                  << (run.oclass.empty() ? "-" : run.oclass) << "\t" << run.chunk << "\t" << recv.size()
                  << "\t" << committed << "\t" << failed << "\t" << lost << std::endl;
    }
    std::cout << runs.size() << " runs in " << where
              << " (receivers: how many have stopped and written their statistics)" << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
    po::options_description od("Command-line options");
    std::string pool, cont, oclass, prefix, outDir, extension;
    uint64_t oidLo = kLegacyOidLo;
    std::vector<uint64_t> events;
    int dataIdFilter;
    uint64_t bundle = 0;

    auto opts = od.add_options();
    opts("help,h", "show this help message");
    opts("pool", po::value<std::string>(&pool)->required(), "DAOS pool label (required)");
    opts("cont", po::value<std::string>(&cont)->default_value("e2sar"), "DAOS container label");
    opts("prefixes", po::bool_switch()->default_value(false), "list the runs in the run catalog");
    opts("prefix", po::value<std::string>(&prefix), "the run to read (the receiver's --prefix)");
    opts("oid", po::value<uint64_t>(&oidLo), "read the shared object with this id (low 64 bits, as given to e2sar_daos_recv --oid) instead of the run's own object");
    opts("oclass", po::value<std::string>(&oclass)->default_value(""), "with --oid, or a prefix not in the catalog: the object class the receiver used, if set");
    opts("delete", po::bool_switch()->default_value(false), "delete run --prefix: its object (all its events) and its catalog entries");
    opts("fetch", po::bool_switch()->default_value(false), "read selected events back and print SHA-256");
    opts("event", po::value<std::vector<uint64_t>>(&events)->multitoken(), "only these event numbers (default: all)");
    opts("dataid", po::value<int>(&dataIdFilter)->default_value(-1), "only this data id (default: all)");
    opts("out,o", po::value<std::string>(&outDir), "with --fetch: also write events as files into this directory");
    opts("extension,e", po::value<std::string>(&extension)->default_value("evio"), "file extension for --out files (empty for none)");
    opts("bundle", po::value<uint64_t>(&bundle)->default_value(0), "with --fetch --out: write N events per file, in (evtNum, dataId) order, with an index per file and a list of missing event numbers (0: one file per event)");

    po::variables_map vm;
    try {
        po::store(po::parse_command_line(argc, argv, od), vm);
        if (vm.count("help")) {
            std::cout << od << std::endl
                      << "Examples:\n"
                      << "  e2sar_daos_read --pool sc26 --cont e2sar --prefixes              # list runs\n"
                      << "  e2sar_daos_read --pool sc26 --cont e2sar --prefix run42          # list events\n"
                      << "  e2sar_daos_read --pool sc26 --cont e2sar --prefix run42 --fetch  # verify all, print sha256\n"
                      << "  e2sar_daos_read --pool sc26 --cont e2sar --prefix run42 --fetch -o ~/restored\n"
                      << "                                                    # write run42_<evt>_<dataId>.evio\n"
                      << "  e2sar_daos_read --pool sc26 --cont e2sar --prefix run42 --fetch -o ~/restored --bundle 100000\n"
                      << "                                                      # write run42_000000.evio, ... (100000 events each)\n"
                      << "  e2sar_daos_read --pool sc26 --cont e2sar --prefix run42 --delete # delete the run" << std::endl;
            return 0;
        }
        po::notify(vm);
    } catch (const po::error& e) {
        std::cerr << "Unable to parse command line: " << e.what() << std::endl;
        return 1;
    }
    const bool listRuns = vm["prefixes"].as<bool>(), del = vm["delete"].as<bool>();
    if (vm.count("out") && !vm["fetch"].as<bool>()) {
        std::cerr << "--out requires --fetch" << std::endl;
        return 1;
    }
    if (bundle > 0 && !vm.count("out")) {
        std::cerr << "--bundle requires --fetch and --out" << std::endl;
        return 1;
    }
    if (listRuns == !prefix.empty()) {
        std::cerr << "Give exactly one of --prefixes or --prefix" << std::endl;
        return 1;
    }
    if (del && (vm["fetch"].as<bool>() || vm.count("oid"))) {
        std::cerr << "--delete deletes a whole run: don't combine it with --fetch or --oid" << std::endl;
        return 1;
    }

    try {
        DaosRuntime daos;
        DaosPool daosPool(pool, del ? DAOS_PC_RW : DAOS_PC_RO);
        DaosContainer daosCont(daosPool, cont, /*createIfMissing=*/false, del ? DAOS_COO_RW : DAOS_COO_RO);
        const unsigned int mode = del ? DAOS_OO_RW : DAOS_OO_RO;
        RunCatalog catalog(daosCont, mode);

        if (listRuns) {
            printRuns(catalog, pool + "/" + cont);
            return 0;
        }

        // The run's own object, or the shared one (--oid, or data from before per-run objects).
        std::optional<RunInfo> run;
        if (!vm.count("oid"))
            run = catalog.find(prefix);
        if (del) {
            if (!run) {
                std::cerr << "Run '" << prefix << "' isn't in the run catalog of " << pool << "/" << cont
                          << ". Data in the shared object can't be deleted per run: destroy the container instead"
                          << std::endl;
                return 1;
            }
            DaosKV(daosCont, run->oidLo, kRunOidHi, run->oclass, mode).destroy();
            catalog.removeRun(prefix);
            std::cout << "Deleted run '" << prefix << "': its object and its catalog entries" << std::endl;
            return 0;
        }
        if (!run && !vm.count("oid"))
            std::cerr << "Run '" << prefix << "' isn't in the run catalog: reading the shared object 0x"
                      << std::hex << oidLo << std::dec << " (data written before per-run objects)" << std::endl;
        auto kvPtr = run ? std::make_unique<DaosKV>(daosCont, run->oidLo, kRunOidHi, run->oclass, mode)
                         : std::make_unique<DaosKV>(daosCont, oidLo, 0, oclass, mode);
        DaosKV& kv = *kvPtr;

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
            if (run)
                printReceivers(catalog.receivers(prefix));
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
        if (bundle > 0) {
            std::vector<std::pair<EventId, EventMeta>> sel;
            for (const auto& entry : l.committed)
                if (selected(entry.first))
                    sel.push_back(entry);
            bad += writeBundles(kv, prefix, sel, bundle, outDir, extension, &ok);
        } else {
            for (const auto& [id, meta] : l.committed) {
                if (!selected(id))
                    continue;
                (fetchEvent(kv, prefix, id, meta, outDir, extension) ? ok : bad)++;
            }
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
