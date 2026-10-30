/**
    e2sar_daos_read: read a run that e2sar_daos_recv stored in DAOS (docs/DAOS_READER.md,
    layout in docs/DAOS_LAYOUT.md). The DAOS container is the only link to the writers;
    start the writers first.

    Reading modes (exactly one), writing to -o DIR:
      --live        follow the run; one file per --interval seconds, the events glued back
                    to back in (evtNum, dataId) order; DAOS left as it is
      --live-rm     the same, and remove data from DAOS --rm-after intervals behind the
                    last file written; the container is destroyed at the end of the run
      --offline     one file per (evtNum, dataId), byte for byte the sent event, with its
                    SHA-256; DAOS left as it is
    Or, instead of a mode (no files):
      --list        run settings, writers, seconds, one line per event
      --fetch       read every event and print its SHA-256
      --delete      destroy the run's container

    Data is read asynchronously in pieces of at most --read-size. A --live file is laid out
    from the index tables first, so each piece is written straight to its place in the
    file (no buffering of a whole window). --offline and --fetch hash each event in order
    as its pieces arrive.
*/

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/evp.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <boost/program_options.hpp>

#include "e2sar_daos/catalog.hpp"
#include "e2sar_daos/daos_handles.hpp"
#include "e2sar_daos/layout.hpp"
#include "e2sar_daos/run_reader.hpp"

namespace po = boost::program_options;
namespace fs = std::filesystem;
using namespace e2sar_daos;
namespace L = e2sar_daos::layout;

namespace {

std::atomic<bool> running{true};
void onSignal(int) { running = false; }

// ------------------------------------------------------------------ logging and time

std::string ts() {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld.%03lld ", static_cast<long long>(ms / 1000),
                  static_cast<long long>(ms % 1000));
    return buf;
}
void logOut(const std::string& s) { std::cout << ts() << s << std::endl; }
void logErr(const std::string& s) { std::cerr << ts() << s << std::endl; }

double steadySec() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ------------------------------------------------------------------ SHA-256

class Sha256 {
public:
    Sha256() : ctx_(EVP_MD_CTX_new(), EVP_MD_CTX_free) { EVP_DigestInit_ex(ctx_.get(), EVP_sha256(), nullptr); }
    void update(const void* p, size_t n) { EVP_DigestUpdate(ctx_.get(), p, n); }
    std::string hex() {
        unsigned char d[EVP_MAX_MD_SIZE];
        unsigned int n = 0;
        EVP_DigestFinal_ex(ctx_.get(), d, &n);
        std::ostringstream o;
        for (unsigned i = 0; i < n; i++)
            o << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(d[i]);
        return o.str();
    }

private:
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx_;
};

// Hashes one event in order while its pieces arrive in any order.
class OrderedHasher {
public:
    void feed(uint64_t at, const uint8_t* p, size_t n) {
        if (at != next_) {
            pending_[at].assign(p, p + n);
            return;
        }
        sha_.update(p, n);
        next_ += n;
        for (auto it = pending_.find(next_); it != pending_.end(); it = pending_.find(next_)) {
            sha_.update(it->second.data(), it->second.size());
            next_ += it->second.size();
            pending_.erase(it);
        }
    }
    std::string hex() { return sha_.hex(); }

private:
    Sha256 sha_;
    uint64_t next_ = 0;
    std::map<uint64_t, std::vector<uint8_t>> pending_;
};

std::string fileSha256(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        throw Failure("open " + path + ": " + std::strerror(errno));
    Sha256 sha;
    std::vector<uint8_t> buf(8 << 20);
    for (;;) {
        const ssize_t n = ::read(fd, buf.data(), buf.size());
        if (n < 0) {
            ::close(fd);
            throw Failure("read " + path + ": " + std::strerror(errno));
        }
        if (n == 0)
            break;
        sha.update(buf.data(), static_cast<size_t>(n));
    }
    ::close(fd);
    return sha.hex();
}

// ------------------------------------------------------------------ files

void pwriteAll(int fd, const uint8_t* p, size_t n, uint64_t off, const std::string& path) {
    while (n > 0) {
        const ssize_t w = ::pwrite(fd, p, n, static_cast<off_t>(off));
        if (w < 0) {
            if (errno == EINTR)
                continue;
            throw Failure("write " + path + ": " + std::strerror(errno));
        }
        p += w;
        n -= static_cast<size_t>(w);
        off += static_cast<uint64_t>(w);
    }
}

int openTmp(const std::string& path) {
    const int fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0)
        throw Failure("create " + path + ": " + std::strerror(errno));
    return fd;
}

// fsync and close fd, then rename tmp -> final: other programs only see complete files.
void commitFile(int fd, const std::string& tmp, const std::string& final) {
    if (::fsync(fd) != 0 || ::close(fd) != 0)
        throw Failure("fsync " + tmp + ": " + std::strerror(errno));
    if (::rename(tmp.c_str(), final.c_str()) != 0)
        throw Failure("rename " + tmp + ": " + std::strerror(errno));
}

void writeTextFile(const std::string& path, const std::string& text) {
    const std::string tmp = path + ".tmp";
    const int fd = openTmp(tmp);
    pwriteAll(fd, reinterpret_cast<const uint8_t*>(text.data()), text.size(), 0, tmp);
    commitFile(fd, tmp, path);
}

void appendLine(const std::string& path, const std::string& line) {
    const int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd < 0)
        throw Failure("open " + path + ": " + std::strerror(errno));
    const std::string s = line + "\n";
    const bool ok = ::write(fd, s.data(), s.size()) == static_cast<ssize_t>(s.size()) && ::fsync(fd) == 0;
    ::close(fd);
    if (!ok)
        throw Failure("append to " + path + ": " + std::strerror(errno));
}

std::vector<std::string> readLines(const std::string& path) {
    std::vector<std::string> out;
    std::ifstream in(path);
    for (std::string l; std::getline(in, l);)
        if (!l.empty())
            out.push_back(l);
    return out;
}

// Leftovers of a crash: never seen by other programs, removed on restart.
void removeTmpFiles(const std::string& dir, const std::string& run) {
    for (const auto& e : fs::directory_iterator(dir)) {
        const std::string n = e.path().filename().string();
        if (n.rfind(run + "_", 0) == 0 && n.size() > 4 && n.compare(n.size() - 4, 4, ".tmp") == 0)
            fs::remove(e.path());
    }
}

// ------------------------------------------------------------------ events

struct Event {
    uint64_t evtNum = 0;
    uint16_t dataId = 0;
    uint64_t size = 0;
    uint32_t slot = 0;
    uint64_t second = 0;  // index (commit) second
    size_t batch = 0;
    size_t entry = 0;
};

bool byEvent(const Event& a, const Event& b) {
    if (a.evtNum != b.evtNum)
        return a.evtNum < b.evtNum;
    if (a.dataId != b.dataId)
        return a.dataId < b.dataId;
    return a.slot < b.slot;
}

std::vector<Event> eventsOf(const std::vector<IndexedBatch>& batches) {
    std::vector<Event> evs;
    for (size_t b = 0; b < batches.size(); b++) {
        const auto& t = batches[b].table;
        for (size_t i = 0; i < t.entries.size(); i++)
            evs.push_back({t.entries[i].evtNum, t.entries[i].dataId, t.entries[i].size, t.slot, batches[b].second, b, i});
    }
    std::sort(evs.begin(), evs.end(), byEvent);
    return evs;
}

std::vector<DataRange> rangesOf(const std::vector<IndexedBatch>& batches) {
    std::vector<DataRange> r;
    for (const auto& b : batches)
        r.push_back({b.table.slot, b.table.tData, b.table.offset, b.table.length});
    return r;
}

// For each part of an entry inside a piece: fn(entry index, offset in the entry, bytes, length).
template <class Fn>
void forEntriesIn(const L::BatchTable& t, const DataPiece& p, Fn fn) {
    const uint64_t lo = p.at, hi = p.at + p.len;
    for (size_t i = 0; i < t.entries.size(); i++) {
        const auto& e = t.entries[i];
        const uint64_t s = std::max(lo, e.offset), end = std::min(hi, e.offset + e.size);
        if (s < end)
            fn(i, s - e.offset, p.data + (s - lo), static_cast<size_t>(end - s));
    }
}

std::string eventName(const std::string& run, uint64_t evtNum, uint16_t dataId) {
    return run + "_" + std::to_string(evtNum) + "_" + std::to_string(dataId) + ".evio";
}

// Event number gaps per dataId. Events come in order within a call; an event lower than
// one already seen is late: it fills its gap if it was missing, or else is a duplicate.
class GapTracker {
public:
    void add(const std::vector<Event>& evs) {
        for (const auto& e : evs) {
            auto it = last_.find(e.dataId);
            if (it == last_.end()) {
                last_[e.dataId] = e.evtNum;
                continue;
            }
            if (e.evtNum > it->second) {
                if (e.evtNum > it->second + 1)
                    gaps_[e.dataId][it->second + 1] = e.evtNum - 1;
                it->second = e.evtNum;
            } else if (fill(e.dataId, e.evtNum)) {
                late_++;
            } else {
                dups_++;
            }
        }
    }

    /** One line per gap still open: "dataId first last count". */
    std::string text() const {
        std::ostringstream o;
        for (const auto& [id, ranges] : gaps_)
            for (const auto& [first, last] : ranges)
                o << id << ' ' << first << ' ' << last << ' ' << last - first + 1 << '\n';
        return o.str();
    }
    uint64_t missing() const {
        uint64_t n = 0;
        for (const auto& [id, ranges] : gaps_)
            for (const auto& [first, last] : ranges)
                n += last - first + 1;
        return n;
    }
    uint64_t late() const { return late_; }
    uint64_t duplicates() const { return dups_; }

private:
    // Take n out of its gap, if it is in one.
    bool fill(uint16_t id, uint64_t n) {
        auto g = gaps_.find(id);
        if (g == gaps_.end())
            return false;
        auto& ranges = g->second;
        auto it = ranges.upper_bound(n);
        if (it == ranges.begin())
            return false;
        --it;
        const uint64_t first = it->first, last = it->second;
        if (n > last)
            return false;
        ranges.erase(it);
        if (first < n)
            ranges[first] = n - 1;
        if (n < last)
            ranges[n + 1] = last;
        return true;
    }

    std::map<uint16_t, uint64_t> last_;
    std::map<uint16_t, std::map<uint64_t, uint64_t>> gaps_;  // first -> last
    uint64_t late_ = 0, dups_ = 0;
};

// ------------------------------------------------------------------ the run

struct Options {
    std::string pool, cont, dir;
    bool live = false, liveRm = false, offline = false, list = false, fetch = false, del = false;
    uint64_t interval = 5, rmAfter = 10, from = 0;
    double stall = 30, endWait = 10;
    bool follow = false;
    std::optional<uint64_t> event;
    std::optional<uint16_t> dataId;
    size_t inflight = 32, readSize = 16 << 20;
};

struct Run {
    std::unique_ptr<DaosRuntime> rt;
    std::unique_ptr<DaosPool> pool;
    std::unique_ptr<DaosContainer> cont;
    std::unique_ptr<Catalog> catalog;
    L::RunSettings settings;
    std::unique_ptr<RunReader> reader;

    // write: change objects in the container (--live-rm); destroy: the container may be
    // destroyed at the end, which needs a read-write pool connection.
    void open(const Options& o, bool write, bool destroy = false) {
        rt = std::make_unique<DaosRuntime>();
        pool = std::make_unique<DaosPool>(o.pool, write || destroy ? DAOS_PC_RW : DAOS_PC_RO);
        try {
            cont = std::make_unique<DaosContainer>(*pool, o.cont, false, write ? DAOS_COO_RW : DAOS_COO_RO);
        } catch (const DaosError& e) {
            if (e.rc() == -DER_NONEXIST)
                throw Failure("container " + o.pool + "/" + o.cont + " not found: start the writers first");
            throw;
        }
        catalog = std::make_unique<Catalog>(*cont, write ? DAOS_OO_RW : DAOS_OO_RO);
        const auto r = catalog->run();
        if (!r)
            throw Failure("no run record in " + o.cont + ": not a run written by e2sar_daos_recv, or start the writers first");
        settings = *r;
        reader = std::make_unique<RunReader>(*cont, settings, write ? DAOS_OO_RW : DAOS_OO_RO, o.inflight, o.readSize);
    }

    // Close everything of the container, then destroy it.
    void destroy(const std::string& label) {
        reader.reset();
        catalog.reset();
        cont.reset();
        check(daos_cont_destroy(pool->handle(), label.c_str(), 1, nullptr), "daos_cont_destroy(" + label + ")");
    }
};

struct SlotState {
    bool done = false;
    bool stalled = false;
    std::optional<L::Watermark> wm;
    double lastChange = 0;  // reader's steady clock
};

// The writers' state from the catalog. done/<slot> is read before wm/<slot>: a writer writes
// its final watermark before its done entry, so a done slot's watermark is its final one.
void refreshSlots(Catalog& cat, std::map<uint32_t, SlotState>& slots, double stall) {
    const double now = steadySec();
    for (uint32_t s : cat.slots()) {
        auto [it, isNew] = slots.try_emplace(s);
        SlotState& st = it->second;
        if (isNew) {
            st.lastChange = now;
            logOut("writer slot " + std::to_string(s) + " found");
        }
        if (!st.done && cat.done(s)) {
            st.done = true;
            logOut("writer slot " + std::to_string(s) + " is done");
        }
        const auto wm = cat.watermark(s);
        if (wm && (!st.wm || wm->index != st.wm->index || wm->data != st.wm->data)) {
            st.wm = wm;
            st.lastChange = now;
            if (st.stalled) {
                st.stalled = false;
                logErr("writer slot " + std::to_string(s) + " moves again; its entries in seconds already read are not re-read");
            }
        }
        if (!st.done && !st.stalled && now - st.lastChange > stall) {
            st.stalled = true;
            logErr("writer slot " + std::to_string(s) + ": no watermark progress for " + std::to_string(stall) +
                   " s; continuing without it");
        }
    }
}

// ------------------------------------------------------------------ --live, --live-rm

struct LiveWindow {
    uint64_t events = 0, bytes = 0;
    bool written = false;
    std::set<std::pair<uint64_t, uint32_t>> arrays;  // (tData, slot) read in this window
};

LiveWindow writeWindow(Run& run, const Options& o, uint64_t W, GapTracker& gaps) {
    std::vector<uint64_t> secs;
    for (uint64_t t = W; t < W + o.interval; t++)
        secs.push_back(t);
    const auto batches = run.reader->readIndex(secs);
    const auto evs = eventsOf(batches);
    LiveWindow res;
    for (const auto& b : batches)
        res.arrays.insert({b.table.tData, b.table.slot});
    if (evs.empty())
        return res;

    // Lay out the file: event k at offset fileOff[k]; entry (batch, i) -> event k.
    std::vector<std::vector<uint64_t>> target(batches.size());
    for (size_t b = 0; b < batches.size(); b++)
        target[b].resize(batches[b].table.entries.size());
    uint64_t off = 0;
    std::ostringstream idx;
    for (const auto& e : evs) {
        target[e.batch][e.entry] = off;
        idx << e.evtNum << ' ' << e.dataId << ' ' << off << ' ' << e.size << '\n';
        off += e.size;
    }

    const std::string base = o.dir + "/" + o.cont + "_" + std::to_string(W);
    const std::string tmp = base + ".evio.tmp";
    const int fd = openTmp(tmp);
    if (::ftruncate(fd, static_cast<off_t>(off)) != 0)
        throw Failure("ftruncate " + tmp + ": " + std::strerror(errno));
    const auto failed = run.reader->read(rangesOf(batches), [&](const DataPiece& p) {
        forEntriesIn(batches[p.range].table, p, [&](size_t i, uint64_t inEntry, const uint8_t* data, size_t n) {
            pwriteAll(fd, data, n, target[p.range][i] + inEntry, tmp);
        });
    });
    if (!failed.empty()) {
        ::close(fd);
        throw Failure(std::to_string(failed.size()) + " batches of window " + std::to_string(W) + " could not be read");
    }
    if (::fsync(fd) != 0)
        throw Failure("fsync " + tmp + ": " + std::strerror(errno));
    const std::string sha = fileSha256(tmp);
    writeTextFile(base + ".idx", idx.str());
    commitFile(fd, tmp, base + ".evio");

    std::ostringstream m;
    m << sha << "  " << fs::path(base + ".evio").filename().string() << "  " << W << ' ' << o.interval << ' '
      << evs.size() << ' ' << off << ' ' << evs.front().evtNum << ' ' << evs.back().evtNum;
    appendLine(o.dir + "/" + o.cont + "_manifest.txt", m.str());
    gaps.add(evs);
    writeTextFile(o.dir + "/" + o.cont + "_missing.txt", gaps.text());

    res.events = evs.size();
    res.bytes = off;
    res.written = true;
    return res;
}

// --live-rm: what to remove once the reader is far enough ahead.
class Remover {
public:
    // data[T, s] seen in batch tables; becomes removable once the slot's data watermark
    // passed T and the reader has read every index second up to the slot's index
    // watermark seen at that time.
    void saw(uint64_t tData, uint32_t slot) { arrays_.try_emplace({tData, slot}, kUnset); }

    void update(const std::map<uint32_t, SlotState>& slots) {
        for (auto& [key, need] : arrays_) {
            if (need != kUnset)
                continue;
            const auto it = slots.find(key.second);
            if (it == slots.end())
                continue;
            if (it->second.done)
                need = 0;
            else if (it->second.wm && it->second.wm->data > key.first)
                need = it->second.wm->index;
        }
    }

    // nextW: the first second not yet read; cutoff: remove what is older.
    void remove(RunReader& r, uint64_t nextW, uint64_t cutoff, const std::set<uint64_t>& keep) {
        for (; removedTo_ < cutoff; removedTo_++)
            if (!keep.count(removedTo_)) {
                try {
                    r.removeSecond(removedTo_);
                } catch (const DaosError& e) {
                    logErr(std::string("removing second ") + std::to_string(removedTo_) + ": " + e.what());
                }
            }
        for (auto it = arrays_.begin(); it != arrays_.end();) {
            const auto [t, slot] = it->first;
            if (t < cutoff && it->second != kUnset && it->second <= nextW && !keep.count(t)) {
                try {
                    r.removeData(t, slot);
                } catch (const DaosError& e) {
                    logErr("removing data[" + std::to_string(t) + ", " + std::to_string(slot) + "]: " + e.what());
                }
                it = arrays_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void startAt(uint64_t t) { removedTo_ = t; }

private:
    static constexpr uint64_t kUnset = ~0ull;
    std::map<std::pair<uint64_t, uint32_t>, uint64_t> arrays_;
    uint64_t removedTo_ = 0;
};

int runLive(const Options& o) {
    Run run;
    run.open(o, o.liveRm, o.liveRm);
    const uint64_t I = o.interval;
    fs::create_directories(o.dir);
    removeTmpFiles(o.dir, o.cont);

    const std::string manifest = o.dir + "/" + o.cont + "_manifest.txt";
    uint64_t W = 0;
    const auto lines = readLines(manifest);
    if (!lines.empty()) {
        std::istringstream in(lines.back());
        std::string sha, name;
        uint64_t lastW = 0, lastI = 0;
        in >> sha >> name >> lastW >> lastI;
        if (lastI != I)
            throw Failure("the manifest was written with --interval " + std::to_string(lastI) + "; restart with the same");
        W = lastW + I;
        logOut("restarting after " + name + " (" + std::to_string(lines.size()) + " files so far)");
    } else {
        W = o.from ? o.from : run.settings.start;
        W -= W % I;
    }
    logOut("run " + o.cont + ": " + formatRecord(run.settings.toRecord()));
    logOut(std::string(o.liveRm ? "--live-rm" : "--live") + ": one file per " + std::to_string(I) + " s from " +
           std::to_string(W) + " into " + o.dir +
           (o.liveRm ? "; removing from DAOS " + std::to_string(o.rmAfter) + " intervals behind" : ""));

    std::map<uint32_t, SlotState> slots;
    std::set<uint64_t> stalledSeconds;  // read without a stalled slot: never removed before the end
    GapTracker gaps;
    Remover remover;
    remover.startAt(W);
    uint64_t files = 0, events = 0, bytes = 0;
    double endSince = -1;

    while (running) {
        refreshSlots(*run.catalog, slots, o.stall);
        remover.update(slots);
        bool progressed = false;

        // Write every window that is complete.
        while (running && !slots.empty()) {
            const uint64_t last = W + I - 1;
            bool complete = true, anyStalled = false;
            uint64_t finalEnd = 0;  // with every slot done or stalled: no data at or after this second
            bool allEnded = true;
            for (const auto& [s, st] : slots) {
                if (st.stalled)
                    anyStalled = true;
                if (!st.done && !st.stalled && !(st.wm && st.wm->index > last))
                    complete = false;
                if (st.done || st.stalled)
                    finalEnd = std::max(finalEnd, st.wm ? st.wm->index + 1 : 0);
                else
                    allEnded = false;
            }
            if (!complete || (allEnded && W >= finalEnd))
                break;
            const double t0 = steadySec();
            const LiveWindow w = writeWindow(run, o, W, gaps);
            for (const auto& [t, s] : w.arrays)
                remover.saw(t, s);
            remover.update(slots);
            if (anyStalled)
                for (uint64_t t = W; t <= last; t++)
                    stalledSeconds.insert(t);
            if (w.written) {
                files++;
                events += w.events;
                bytes += w.bytes;
                std::ostringstream m;
                m << std::fixed << std::setprecision(2) << "wrote " << o.cont << "_" << W << ".evio: " << w.events
                  << " events, " << w.bytes / 1048576.0 << " MiB in " << steadySec() - t0 << " s";
                logOut(m.str());
            }
            W += I;
            progressed = true;
            if (o.liveRm && W >= o.rmAfter * I) {
                // the last written window started at W - I
                const uint64_t D = W - I;
                remover.remove(*run.reader, W, D - o.rmAfter * I, stalledSeconds);
            }
            if (steadySec() - t0 > 1.0)
                break;  // re-read the catalog at least once a second while catching up
        }

        // End of run: every writer done (or stalled), everything read, no new writer for --end-wait.
        bool allEnded = !slots.empty();
        uint64_t finalEnd = 0;
        for (const auto& [s, st] : slots) {
            if (!st.done && !st.stalled)
                allEnded = false;
            finalEnd = std::max(finalEnd, st.wm ? st.wm->index + 1 : 0);
        }
        if (allEnded && W >= finalEnd && !o.follow) {
            if (endSince < 0)
                endSince = steadySec();
            const size_t nslots = slots.size();
            if (steadySec() - endSince >= o.endWait) {
                refreshSlots(*run.catalog, slots, o.stall);
                if (slots.size() == nslots)
                    break;
                endSince = -1;  // a new writer joined
            }
        } else {
            endSince = -1;
        }

        if (!progressed)
            for (int i = 0; i < 10 && running; i++)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::ostringstream m;
    m << std::fixed << std::setprecision(2) << (running ? "end of run: " : "stopped: ") << files << " files, "
      << events << " events, " << bytes / 1048576.0 << " MiB; " << gaps.missing() << " missing, " << gaps.late()
      << " late, " << gaps.duplicates() << " duplicate event numbers";
    logOut(m.str());
    if (o.liveRm && running) {
        run.destroy(o.cont);
        logOut("container " + o.cont + " destroyed");
    }
    return 0;
}

// ------------------------------------------------------------------ --offline, --fetch, --list

// Every committed event of the run, in seconds groups so the tables of a long run are
// not all in memory at once. fn(batches, events of these batches) per group.
template <class Fn>
void forEachGroup(Run& run, Fn fn) {
    const auto secs = run.reader->listSeconds();
    constexpr size_t kGroup = 16;
    for (size_t i = 0; i < secs.size() && running; i += kGroup) {
        const std::vector<uint64_t> g(secs.begin() + i, secs.begin() + std::min(secs.size(), i + kGroup));
        const auto batches = run.reader->readIndex(g);
        fn(batches, eventsOf(batches));
    }
}

bool selected(const Options& o, const Event& e) {
    return (!o.event || e.evtNum == *o.event) && (!o.dataId || e.dataId == *o.dataId);
}

int runOffline(const Options& o) {
    Run run;
    run.open(o, false);
    fs::create_directories(o.dir);
    removeTmpFiles(o.dir, o.cont);
    const std::string manifest = o.dir + "/" + o.cont + "_manifest.txt";
    const std::string shaFile = o.dir + "/" + o.cont + ".sha256";
    std::set<std::string> have;
    for (const auto& l : readLines(manifest)) {
        std::istringstream in(l);
        std::string sha, name;
        in >> sha >> name;
        have.insert(name);
    }
    if (!have.empty())
        logErr(std::to_string(have.size()) + " files already in the manifest are skipped");

    struct Out {
        std::string name;
        uint64_t size = 0, written = 0;
        int fd = -1;
        OrderedHasher sha;
        const Event* ev = nullptr;
    };
    uint64_t files = 0, bytes = 0, errors = 0, dups = 0;
    std::set<std::pair<uint16_t, uint64_t>> seen;
    GapTracker gaps;
    std::vector<Event> all;

    forEachGroup(run, [&](const std::vector<IndexedBatch>& batches, const std::vector<Event>& evs) {
        // entry (batch, i) -> output file; small events complete within one piece, so
        // only large events keep a file open across pieces
        std::map<std::pair<size_t, size_t>, std::unique_ptr<Out>> outs;
        std::set<size_t> needed;
        for (const auto& e : evs) {
            all.push_back(e);
            if (!selected(o, e))
                continue;
            if (!seen.insert({e.dataId, e.evtNum}).second) {
                dups++;
                logErr("event " + std::to_string(e.evtNum) + "/" + std::to_string(e.dataId) +
                       " is stored twice; the second copy is skipped");
                continue;
            }
            auto out = std::make_unique<Out>();
            out->name = eventName(o.cont, e.evtNum, e.dataId);
            out->size = e.size;
            out->ev = &e;
            if (have.count(out->name))
                continue;
            needed.insert(e.batch);
            outs[{e.batch, e.entry}] = std::move(out);
        }

        auto finish = [&](Out& out) {
            const std::string final = o.dir + "/" + out.name, tmp = final + ".tmp";
            if (out.fd < 0)
                out.fd = openTmp(tmp);  // an empty event
            commitFile(out.fd, tmp, final);
            out.fd = -1;
            const std::string line = out.sha.hex() + "  " + out.name;
            std::cout << line << std::endl;
            appendLine(shaFile, line);
            appendLine(manifest, line + "  " + std::to_string(out.ev->evtNum) + " " +
                                     std::to_string(out.ev->dataId) + " " + std::to_string(out.size));
            files++;
            bytes += out.size;
        };

        std::vector<IndexedBatch> sel;
        std::vector<size_t> selIndex;  // sel[j] is batches[selIndex[j]]
        for (size_t b : needed) {
            sel.push_back(batches[b]);
            selIndex.push_back(b);
        }
        const auto failed = run.reader->read(rangesOf(sel), [&](const DataPiece& p) {
            const size_t b = selIndex[p.range];
            forEntriesIn(sel[p.range].table, p, [&](size_t i, uint64_t inEntry, const uint8_t* data, size_t n) {
                const auto it = outs.find({b, i});
                if (it == outs.end())
                    return;
                Out& out = *it->second;
                const std::string tmp = o.dir + "/" + out.name + ".tmp";
                if (out.fd < 0)
                    out.fd = openTmp(tmp);
                pwriteAll(out.fd, data, n, inEntry, tmp);
                out.sha.feed(inEntry, data, n);
                out.written += n;
                if (out.written == out.size)
                    finish(out);
            });
        });
        for (auto& [k, out] : outs) {
            if (out->size == 0) {
                finish(*out);
            } else if (out->written != out->size) {
                errors++;
                logErr("event " + out->name + " could not be read completely");
                if (out->fd >= 0)
                    ::close(out->fd);
            }
        }
        (void)failed;  // their events are counted as incomplete above
    });

    std::sort(all.begin(), all.end(), byEvent);
    gaps.add(all);
    writeTextFile(o.dir + "/" + o.cont + "_missing.txt", gaps.text());

    std::ostringstream m;
    m << std::fixed << std::setprecision(2) << files << " files, " << bytes / 1048576.0 << " MiB written to "
      << o.dir << "; " << gaps.missing() << " missing event numbers (" << o.cont << "_missing.txt), " << dups
      << " duplicates, " << errors << " errors";
    logErr(m.str());

    if (errors || !running)
        return 1;
    bool allDone = true;
    for (uint32_t sl : run.catalog->slots())
        if (!run.catalog->done(sl))
            allDone = false;
    if (!allDone)
        logErr("not every writer is done: run again after the transfer for the rest");
    return 0;
}

int runFetch(const Options& o) {
    Run run;
    run.open(o, false);
    struct Res {
        uint64_t size = 0, got = 0;
        OrderedHasher sha;
    };
    uint64_t n = 0, bytes = 0, errors = 0;
    forEachGroup(run, [&](const std::vector<IndexedBatch>& batches, const std::vector<Event>& evs) {
        std::map<std::pair<size_t, size_t>, Res> res;
        std::set<size_t> needed;
        for (const auto& e : evs)
            if (selected(o, e)) {
                res[{e.batch, e.entry}].size = e.size;
                needed.insert(e.batch);
            }
        std::vector<IndexedBatch> sel;
        std::vector<size_t> selIndex;
        for (size_t b : needed) {
            sel.push_back(batches[b]);
            selIndex.push_back(b);
        }
        run.reader->read(rangesOf(sel), [&](const DataPiece& p) {
            const size_t b = selIndex[p.range];
            forEntriesIn(sel[p.range].table, p, [&](size_t i, uint64_t inEntry, const uint8_t* data, size_t len) {
                auto it = res.find({b, i});
                if (it == res.end())
                    return;
                it->second.sha.feed(inEntry, data, len);
                it->second.got += len;
            });
        });
        for (const auto& e : evs) {
            auto it = res.find({e.batch, e.entry});
            if (it == res.end())
                continue;
            if (it->second.got != e.size) {
                errors++;
                std::cerr << "event " << e.evtNum << "/" << e.dataId << ": read " << it->second.got << " of " << e.size
                          << " bytes" << std::endl;
                continue;
            }
            std::cout << it->second.sha.hex() << "  " << eventName(o.cont, e.evtNum, e.dataId) << "  " << e.size
                      << " bytes" << std::endl;
            n++;
            bytes += e.size;
        }
    });
    std::cout << n << " events read, " << bytes << " bytes, " << errors << " errors" << std::endl;
    return errors ? 1 : 0;
}

int runList(const Options& o) {
    Run run;
    run.open(o, false);
    std::cout << "Run " << o.cont << ": " << formatRecord(run.settings.toRecord()) << std::endl;
    for (uint32_t s : run.catalog->slots()) {
        const auto done = run.catalog->done(s);
        const auto wm = run.catalog->watermark(s);
        std::cout << "Writer " << s << ": ";
        if (done)
            std::cout << "committed " << (done->count("committed") ? done->at("committed") : "?") << " events, done: "
                      << formatRecord(*done);
        else
            std::cout << "running" << (wm ? ", watermark " + formatRecord(wm->toRecord()) : "");
        std::cout << std::endl;
    }
    const auto secs = run.reader->listSeconds();
    std::cout << "Seconds with index entries: " << secs.size();
    if (!secs.empty())
        std::cout << " (" << secs.front() << " .. " << secs.back() << ")";
    std::cout << std::endl << "evtNum\tdataId\tbytes\tsecond\tslot" << std::endl;
    uint64_t n = 0, bytes = 0;
    forEachGroup(run, [&](const std::vector<IndexedBatch>&, const std::vector<Event>& evs) {
        for (const auto& e : evs) {
            if (!selected(o, e))
                continue;
            std::cout << e.evtNum << '\t' << e.dataId << '\t' << e.size << '\t' << e.second << '\t' << e.slot << '\n';
            n++;
            bytes += e.size;
        }
    });
    std::cout << n << " committed events, " << bytes << " bytes" << std::endl;
    return 0;
}

size_t parseSize(const std::string& s) {
    size_t pos = 0;
    double v = 0;
    try {
        v = std::stod(s, &pos);
    } catch (const std::exception&) {
        return 0;
    }
    const std::string u = s.substr(pos);
    const double mult = u == "K" || u == "k" ? 1024.0 : u == "M" || u == "m" ? 1048576.0 : u == "G" || u == "g" ? 1073741824.0 : u.empty() ? 1.0 : 0.0;
    return v > 0 ? static_cast<size_t>(v * mult) : 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    std::string readSizeStr;
    uint64_t eventNum = 0;
    unsigned dataIdOpt = 0;
    po::options_description od("Command-line options");
    auto opts = od.add_options();
    opts("help,h", "show this help message");
    opts("pool", po::value<std::string>(&o.pool), "DAOS pool label (required)");
    opts("cont", po::value<std::string>(&o.cont), "the run's container label (required)");
    opts("out,o", po::value<std::string>(&o.dir), "output directory (required in a reading mode); holds the manifest, the restart point");
    opts("live", po::bool_switch(&o.live), "mode: follow the run, one file per --interval s, DAOS untouched");
    opts("live-rm", po::bool_switch(&o.liveRm), "mode: as --live, removing data from DAOS --rm-after intervals behind; destroys the container at the end");
    opts("offline", po::bool_switch(&o.offline), "mode: one file per (evtNum, dataId) with SHA-256; DAOS left as it is");
    opts("list", po::bool_switch(&o.list), "instead of a mode: run settings, writers, seconds, one line per event");
    opts("fetch", po::bool_switch(&o.fetch), "instead of a mode: read every event and print its SHA-256");
    opts("delete", po::bool_switch(&o.del), "instead of a mode: destroy the run's container");
    opts("interval", po::value<uint64_t>(&o.interval)->default_value(5), "seconds per file (--live, --live-rm)");
    opts("rm-after", po::value<uint64_t>(&o.rmAfter)->default_value(10), "intervals kept in DAOS behind the last file written (--live-rm)");
    opts("from", po::value<uint64_t>(&o.from)->default_value(0), "first UTC second to read (--live, --live-rm; default: the run start)");
    opts("stall", po::value<double>(&o.stall)->default_value(30), "seconds without watermark progress before a writer is skipped");
    opts("end-wait", po::value<double>(&o.endWait)->default_value(10), "seconds with every writer done and no new one before the run ends");
    opts("follow", po::bool_switch(&o.follow), "never end; keep waiting for new writers (--live)");
    opts("event", po::value<uint64_t>(&eventNum), "only this event number (--offline, --fetch, --list)");
    opts("dataid", po::value<unsigned>(&dataIdOpt), "only this dataId (--offline, --fetch, --list)");
    opts("inflight", po::value<size_t>(&o.inflight)->default_value(32), "DAOS reads in flight");
    opts("read-size", po::value<std::string>(&readSizeStr)->default_value("16M"), "largest single read (K/M/G); whole chunks");

    po::variables_map vm;
    try {
        po::store(po::parse_command_line(argc, argv, od), vm);
        po::notify(vm);
    } catch (const po::error& e) {
        std::cerr << "Unable to parse command line: " << e.what() << std::endl;
        return 1;
    }
    if (vm.count("help")) {
        std::cout << od << std::endl
                  << "Examples:\n"
                  << "  e2sar_daos_read --pool sc26 --cont e2sar_1793280000 -o DIR --live\n"
                  << "  e2sar_daos_read --pool sc26 --cont e2sar_1793280000 -o DIR --live-rm --rm-after 2\n"
                  << "  e2sar_daos_read --pool sc26 --cont e2sar_1793280000 -o DIR --offline\n"
                  << "  e2sar_daos_read --pool sc26 --cont e2sar_1793280000 --delete     # after checking the files\n"
                  << "  e2sar_daos_read --pool sc26 --cont e2sar_1793280000 --fetch\n";
        return 0;
    }
    if (vm.count("event"))
        o.event = eventNum;
    if (vm.count("dataid"))
        o.dataId = static_cast<uint16_t>(dataIdOpt);
    o.readSize = parseSize(readSizeStr);

    const int modes = o.live + o.liveRm + o.offline;
    const int tools = o.list + o.fetch + o.del;
    if (modes + tools != 1) {
        std::cerr << "Give exactly one of --live, --live-rm, --offline (reading modes) or --list, --fetch, --delete"
                  << std::endl;
        return 1;
    }
    if (o.pool.empty() || o.cont.empty()) {
        std::cerr << "--pool and --cont are required" << std::endl;
        return 1;
    }
    if (modes && o.dir.empty()) {
        std::cerr << "-o DIR is required with a reading mode" << std::endl;
        return 1;
    }
    const bool liveMode = o.live || o.liveRm;
    if ((!liveMode && (!vm["interval"].defaulted() || !vm["from"].defaulted() || o.follow)) ||
        (!o.liveRm && !vm["rm-after"].defaulted()) ||
        (liveMode && (o.event || o.dataId))) {
        std::cerr << "--interval, --from, --follow need --live or --live-rm; --rm-after needs --live-rm; "
                     "--event/--dataid don't go with --live or --live-rm"
                  << std::endl;
        return 1;
    }
    if (o.interval == 0 || o.readSize == 0 || o.inflight == 0) {
        std::cerr << "--interval, --read-size and --inflight must be > 0" << std::endl;
        return 1;
    }

    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    try {
        if (liveMode)
            return runLive(o);
        if (o.offline)
            return runOffline(o);
        if (o.fetch)
            return runFetch(o);
        if (o.list)
            return runList(o);
        Run run;  // --delete
        run.rt = std::make_unique<DaosRuntime>();
        run.pool = std::make_unique<DaosPool>(o.pool, DAOS_PC_RW);
        check(daos_cont_destroy(run.pool->handle(), o.cont.c_str(), 1, nullptr), "daos_cont_destroy(" + o.cont + ")");
        std::cout << "container " << o.cont << " destroyed" << std::endl;
        return 0;
    } catch (const DaosError& e) {
        logErr(std::string("DAOS error: ") + e.what());
    } catch (const Failure& e) {
        logErr(std::string("error: ") + e.what());
    } catch (const fs::filesystem_error& e) {
        logErr(std::string("error: ") + e.what());
    }
    return 1;
}
