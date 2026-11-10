/**
    e2sar_daos_monitor: watch one run's DAOS container (docs/DAOS_LAYOUT.md) and write its
    state every --every seconds (default 2) as one JSON line, for the live dashboard
    viz/daos_monitor/ (docs/STREAMING_WORKFLOW.md#monitoring).

    Read-only. It reads only the container, never the reader's files, so it runs with or
    without e2sar_daos_read and doesn't change what writers or readers do:

      incoming   events newly committed: index entries not seen before
      outgoing   events that left DAOS: their index second was removed (e2sar_daos_read --live-rm)
      in DAOS    incoming - outgoing
      missing    events the receivers lost, from their counters in the catalog (wm/<slot>):
                 lost in reassembly, lost in enqueue, failed DAOS writes. Events of which
                 no packet reached a receiver are not counted: nothing saw them
      gaps       event numbers absent, per dataId, for more than --grace seconds while a
                 higher number of that dataId is committed. Only a hint: a sender need not
                 number its events one after another. Younger gaps are "pending" (another
                 writer may still commit them). A gap filled later counts as late. Reported
                 both as still open and as a cumulative total that never goes down. A jump
                 of more than --restart-gap event numbers is a sender restart, not a gap:
                 the numbers after it are tracked as a new segment (event_gaps.hpp).

    Each line also holds, per dataId, the state of its event numbers
    (in DAOS / left DAOS / gap / pending) over its newest --span event numbers (default
    about 32 s of events), in --bins bins, run-length encoded: the dashboard draws them as a
    moving strip. Lines go to --out (appended), so a run can be replayed.

    It waits for the container if the run hasn't started, and exits when the container is
    destroyed (end of a --live-rm run) or on Ctrl-C.
*/

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <fstream>
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
#include "e2sar_daos/event_gaps.hpp"
#include "e2sar_daos/layout.hpp"
#include "e2sar_daos/run_reader.hpp"
#include "e2sar_daos/text_files.hpp"

namespace po = boost::program_options;
using namespace e2sar_daos;
namespace L = e2sar_daos::layout;

namespace {

std::atomic<bool> running{true};
void onSignal(int) { running = false; }

double unixNow() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string ts() {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f ", unixNow());
    return buf;
}
void logOut(const std::string& s) { std::cout << ts() << s << std::endl; }
void logErr(const std::string& s) { std::cerr << ts() << s << std::endl; }

// Sleep up to sec, waking early on Ctrl-C.
void nap(double sec) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(sec);
    while (running && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

std::string jsonStr(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\')
            o += '\\';
        if (static_cast<unsigned char>(c) >= 0x20)
            o += c;
    }
    return o + "\"";
}

struct Options {
    std::string pool, cont, out;
    double every = 2, grace = 5, stripSeconds = 32;
    uint64_t span = 0;  // event numbers per strip; 0 = from the rate (stripSeconds of events)
    uint64_t restartGap = EventGaps::kDefaultRestartGap;
    size_t bins = 600;
};

// ------------------------------------------------------------------ per-dataId state

enum : char { kUnknown = 'u', kOut = 'o', kIn = 'd', kPending = 'p', kGap = 'g' };
// A bin with any stored event shows it: with a sender that skips event numbers (a clock-based
// number) nearly every bin has an absent number, and a gap would hide the bin's events.
int rank(char c) { return c == kIn ? 4 : c == kOut ? 3 : c == kGap ? 2 : c == kPending ? 1 : 0; }

struct Stream {
    explicit Stream(uint64_t restartGap) : gaps(restartGap) {}

    uint64_t events = 0, bytes = 0, prevEvents = 0;
    EventGaps gaps;  // segments of event numbers, with their gaps
    // Committed event numbers of the newest segment near its top, as runs [start, end] with
    // one commit second: start -> (end, second). Events mostly come in order, so a second is
    // a few runs.
    std::map<uint64_t, std::pair<uint64_t, uint64_t>> runs;
    uint64_t late = 0, dups = 0;

    void add(uint64_t evt, uint64_t second, uint64_t size, double now, uint64_t span) {
        events++;
        bytes += size;
        bool current = false;
        switch (gaps.add(evt, now, &current)) {
            case EventGaps::Kind::Restart:
                runs.clear();  // the strip follows the new numbers
                break;
            case EventGaps::Kind::Late:
                late++;
                break;
            case EventGaps::Kind::Duplicate:
                dups++;
                return;
            default:
                break;
        }
        if (current && evt + span > gaps.current().max)
            addRun(evt, second);
    }
    uint64_t first() const { return gaps.empty() ? 0 : gaps.current().first; }
    uint64_t max() const { return gaps.empty() ? 0 : gaps.current().max; }

    void addRun(uint64_t evt, uint64_t second) {
        auto next = runs.upper_bound(evt);
        if (next != runs.begin()) {
            auto prev = std::prev(next);
            if (prev->second.first >= evt)
                return;  // already there
            if (prev->second.first + 1 == evt && prev->second.second == second) {
                prev->second.first = evt;
                if (next != runs.end() && next->first == evt + 1 && next->second.second == second) {
                    prev->second.first = next->second.first;
                    runs.erase(next);
                }
                return;
            }
        }
        if (next != runs.end() && next->first == evt + 1 && next->second.second == second) {
            const auto v = next->second;
            runs.erase(next);
            runs[evt] = v;
            return;
        }
        runs[evt] = {evt, second};
    }

    // Forget runs below lo.
    void prune(uint64_t lo) {
        while (!runs.empty() && runs.begin()->second.first < lo)
            runs.erase(runs.begin());
    }

    /**
        The state of event numbers [lo, lo + bins × width) in `bins` bins of `width`, one
        character per bin, run-length encoded ("d120o30g2p10"): d = in DAOS, o = left DAOS,
        g = gap, p = pending, u = unknown (not committed yet, before the first event the
        monitor saw, or removed before the monitor started). A bin shows the first of these
        that any of its event numbers has: g or p only when none of its events is stored.
    */
    std::string strip(uint64_t lo, uint64_t width, size_t bins, const std::set<uint64_t>& gone, double now,
                      double grace) const {
        std::string b(bins, kUnknown);
        const uint64_t hi = lo + width * bins - 1;  // inclusive
        auto mark = [&](uint64_t a, uint64_t z, char c) {
            a = std::max(a, lo);
            z = std::min(z, hi);
            if (a > z)
                return;
            for (size_t i = (a - lo) / width; i <= (z - lo) / width; i++)
                if (rank(c) > rank(b[i]))
                    b[i] = c;
        };
        auto r = runs.upper_bound(lo);
        if (r != runs.begin())
            --r;
        for (; r != runs.end() && r->first <= hi; ++r)
            mark(r->first, r->second.first, gone.count(r->second.second) ? kOut : kIn);
        if (!gaps.empty()) {
            const auto& cur = gaps.current().gaps;
            auto g = cur.upper_bound(lo);
            if (g != cur.begin())
                --g;
            for (; g != cur.end() && g->first <= hi; ++g)
                mark(g->first, g->second.last, now - g->second.since > grace ? kGap : kPending);
        }
        std::string out;
        for (size_t i = 0; i < bins;) {
            size_t k = i;
            while (k < bins && b[k] == b[i])
                k++;
            out += b[i] + std::to_string(k - i);
            i = k;
        }
        return out;
    }
};

// ------------------------------------------------------------------ per-second state

struct Second {
    uint64_t events = 0, bytes = 0;
    std::set<uint64_t> dkeys;  // index entries already counted (until the second is final)
    bool final = false;        // every writer is past it: no new entry can appear
    bool gone = false;         // removed from DAOS
};

struct Writer {
    bool seen = false;  // writer/<slot> read (it doesn't change)
    std::string source;  // where its data comes from, e.g. "LB 682"; empty from receivers before 2026-11-10
    std::optional<L::Watermark> wm;
    bool done = false;
    uint64_t events = 0, bytes = 0;
};

struct Event {
    uint64_t evtNum, second, size;
    uint16_t dataId;
};

class Monitor {
public:
    explicit Monitor(const Options& o) : o_(o) {}

    // Returns false when the container is gone after the run had started.
    bool open() {
        rt_ = std::make_unique<DaosRuntime>();
        pool_ = std::make_unique<DaosPool>(o_.pool, DAOS_PC_RO);
        bool said = false;
        while (running) {
            try {
                cont_ = std::make_unique<DaosContainer>(*pool_, o_.cont, false, DAOS_COO_RO);
                cat_ = std::make_unique<Catalog>(*cont_, DAOS_OO_RO);
                if (auto r = cat_->run()) {
                    settings_ = *r;
                    break;
                }
            } catch (const DaosError& e) {
                if (e.rc() != -DER_NONEXIST)
                    throw;
                if (started_)
                    return false;
            }
            cat_.reset();
            cont_.reset();
            if (!said)
                logOut("waiting for container " + o_.pool + "/" + o_.cont + " (start the writers)");
            said = true;
            nap(1);
        }
        if (!running)
            return false;
        reader_ = std::make_unique<RunReader>(*cont_, settings_, DAOS_OO_RO, 32, 16 << 20);
        started_ = true;
        logOut("run " + o_.cont + ": " + formatRecord(settings_.toRecord()));
        return true;
    }

    void close() {
        reader_.reset();
        cat_.reset();
        cont_.reset();
    }

    // One look at the container; returns the JSON line.
    std::string tick() {
        const double now = unixNow();
        // writers: done is read before wm, so a done slot's watermark is its final one
        for (uint32_t s : cat_->slots()) {
            Writer& w = writers_[s];
            if (!w.seen) {
                if (auto r = cat_->writer(s)) {
                    w.seen = true;
                    if (r->count("source"))
                        w.source = r->at("source");
                    for (auto& c : w.source)  // the catalog stores spaces as '_'
                        if (c == '_')
                            c = ' ';
                }
            }
            if (!w.done && cat_->done(s))
                w.done = true;
            if (auto wm = cat_->watermark(s))
                w.wm = wm;
        }
        auto isFinal = [&](uint64_t t) {
            if (writers_.empty())
                return false;
            for (const auto& [s, w] : writers_)
                if (!(w.wm && w.wm->index > t))
                    return false;
            return true;
        };

        const auto listed = reader_->listSeconds();
        const std::set<uint64_t> present(listed.begin(), listed.end());
        uint64_t outEvents = 0, outBytes = 0, outSeconds = 0;
        for (auto& [t, sec] : seconds_)
            if (!sec.gone && !present.count(t)) {
                sec.gone = true;
                gone_.insert(t);
                outSeconds++;
                outEvents += sec.events;
                outBytes += sec.bytes;
            }

        std::vector<Event> evs;
        uint64_t inBytes = 0;
        for (uint64_t t : listed) {
            if (!running)
                return {};  // Ctrl-C: the first tick of a long run reads every second's index
            Second& sec = seconds_[t];
            if (sec.final)
                continue;
            const bool fin = isFinal(t);  // decided before reading: entries read now are all there are
            std::vector<IndexedBatch> batches;
            try {
                batches = reader_->readIndex({t});
            } catch (const DaosError&) {
                continue;  // removed while reading, or not readable yet: next tick
            }
            for (const auto& b : batches) {
                if (!sec.dkeys.insert(b.dkey).second)
                    continue;
                Writer& w = writers_[b.table.slot];
                for (const auto& e : b.table.entries) {
                    evs.push_back({e.evtNum, t, e.size, e.dataId});
                    sec.events++;
                    sec.bytes += e.size;
                    w.events++;
                    w.bytes += e.size;
                    inBytes += e.size;
                }
            }
            if (fin) {
                sec.final = true;
                sec.dkeys.clear();
            }
        }
        std::sort(evs.begin(), evs.end(), [](const Event& a, const Event& b) {
            return a.evtNum != b.evtNum ? a.evtNum < b.evtNum : a.dataId < b.dataId;
        });
        for (const auto& e : evs)
            streams_.try_emplace(e.dataId, o_.restartGap).first->second.add(e.evtNum, e.second, e.size, now, span_ ? span_ : ~0ull >> 1);

        inEvents_ += evs.size();
        inBytes_ += inBytes;
        outEvents_ += outEvents;
        outBytes_ += outBytes;
        const double dt = lastTick_ > 0 ? now - lastTick_ : o_.every;
        lastTick_ = now;
        tick_++;

        // ---- the JSON line
        std::ostringstream j;
        j.setf(std::ios::fixed);
        j.precision(3);
        j << "{\"type\":\"tick\",\"tick\":" << tick_ << ",\"t\":" << now << ",\"dt\":" << dt
          << ",\"run\":" << jsonStr(o_.cont) << ",\"start\":" << settings_.start << ",\"grace\":" << o_.grace;
        j << ",\"writers\":[";
        L::Losses lost;        // summed over the writers that report it
        uint64_t noCounts = 0;  // writers without loss counters (built before 2026-11-09)
        bool first = true;
        for (const auto& [s, w] : writers_) {
            j << (first ? "" : ",") << "{\"slot\":" << s << ",\"done\":" << (w.done ? "true" : "false");
            if (!w.source.empty())
                j << ",\"source\":" << jsonStr(w.source);
            if (w.wm)
                j << ",\"wm_index\":" << w.wm->index << ",\"wm_data\":" << w.wm->data << ",\"lag\":" << now - w.wm->index;
            if (w.wm && w.wm->losses) {
                const auto& l = *w.wm->losses;
                j << ",\"lost\":" << l.total() << ",\"reassembly_loss\":" << l.reassembly << ",\"enqueue_loss\":" << l.enqueue
                  << ",\"failed\":" << l.failed;
                lost.reassembly += l.reassembly;
                lost.enqueue += l.enqueue;
                lost.failed += l.failed;
            } else {
                noCounts++;
            }
            j << ",\"events\":" << w.events << ",\"bytes\":" << w.bytes << "}";
            first = false;
        }
        j << "]";
        std::set<std::string> sources;  // where the run's data comes from, over all writers
        for (const auto& [s, w] : writers_)
            if (!w.source.empty())
                sources.insert(w.source);
        j << ",\"sources\":[";
        first = true;
        for (const auto& src : sources) {
            j << (first ? "" : ",") << jsonStr(src);
            first = false;
        }
        j << "]";
        j << ",\"in\":{\"events\":" << inEvents_ << ",\"bytes\":" << inBytes_ << ",\"d_events\":" << evs.size()
          << ",\"d_bytes\":" << inBytes << "}";
        j << ",\"out\":{\"events\":" << outEvents_ << ",\"bytes\":" << outBytes_ << ",\"d_events\":" << outEvents
          << ",\"d_bytes\":" << outBytes << ",\"d_seconds\":" << outSeconds << "}";
        j << ",\"daos\":{\"events\":" << inEvents_ - outEvents_ << ",\"bytes\":" << inBytes_ - outBytes_
          << ",\"seconds\":" << listed.size();
        if (!listed.empty())
            j << ",\"first\":" << listed.front() << ",\"last\":" << listed.back();
        j << "}";
        j << ",\"missing\":{\"total\":" << lost.total() << ",\"reassembly\":" << lost.reassembly << ",\"enqueue\":" << lost.enqueue
          << ",\"failed\":" << lost.failed << ",\"writers_unknown\":" << noCounts << "}";
        uint64_t miss = 0, pend = 0, late = 0, dups = 0, total = 0, restarts = 0;
        // The strip's span: --span, or --strip-seconds of events at the fastest dataId's rate,
        // as a power of two × --bins. It only grows (the first update with data can be partly
        // filled), so the strip moves smoothly once the rate is steady.
        if (o_.span) {
            span_ = o_.span;
        } else {
            uint64_t fastest = 0;
            for (const auto& [id, st] : streams_)
                fastest = std::max(fastest, st.events - st.prevEvents);
            if (fastest && tick_ > 1) {
                const double want = fastest / dt * o_.stripSeconds;
                uint64_t span = o_.bins;
                while (span < want)
                    span *= 2;
                span_ = std::max(span_, span);
            }
        }
        const uint64_t width = span_ ? (span_ + o_.bins - 1) / o_.bins : 1;
        j << ",\"strip\":{\"span\":" << width * o_.bins << ",\"bins\":" << o_.bins << ",\"width\":" << width << "}";
        j << ",\"streams\":[";
        first = true;
        for (auto& [id, st] : streams_) {
            const uint64_t top = st.max(), lo = top + 1 >= width * o_.bins ? top + 1 - width * o_.bins : 0;
            st.prune(lo);
            const auto [m, p] = st.gaps.missing(now, o_.grace);
            miss += m;
            pend += p;
            late += st.late;
            dups += st.dups;
            total += st.gaps.everMissing();
            restarts += st.gaps.restarts();
            j << (first ? "" : ",") << "{\"id\":" << id << ",\"events\":" << st.events << ",\"bytes\":" << st.bytes
              << ",\"d_events\":" << st.events - st.prevEvents << ",\"first\":" << st.first() << ",\"max\":" << top
              << ",\"gaps\":" << m << ",\"gaps_total\":" << st.gaps.everMissing() << ",\"restarts\":" << st.gaps.restarts() << ",\"pending\":" << p << ",\"late\":" << st.late << ",\"dups\":" << st.dups
              << ",\"strip_lo\":" << lo;
            if (span_)
                j << ",\"strip\":\"" << st.strip(lo, width, o_.bins, gone_, now, o_.grace) << "\"";
            j << "}";
            st.prevEvents = st.events;
            first = false;
        }
        j << "]";
        j << ",\"gaps\":{\"total\":" << total << ",\"events\":" << miss << ",\"pending\":" << pend << ",\"late\":" << late
          << ",\"dups\":" << dups << ",\"restarts\":" << restarts << "}}";

        char line[400];
        std::snprintf(line, sizeof(line),
                      "in %llu events (+%zu) | out %llu (+%llu) | in DAOS %llu events, %.1f MiB, %zu s | missing %llu "
                      "(reassembly %llu, enqueue %llu, failed %llu)%s | gaps %llu so far (%llu open), pending %llu%s",
                      static_cast<unsigned long long>(inEvents_), evs.size(),
                      static_cast<unsigned long long>(outEvents_), static_cast<unsigned long long>(outEvents),
                      static_cast<unsigned long long>(inEvents_ - outEvents_),
                      (inBytes_ - outBytes_) / 1048576.0, listed.size(), static_cast<unsigned long long>(lost.total()),
                      static_cast<unsigned long long>(lost.reassembly), static_cast<unsigned long long>(lost.enqueue),
                      static_cast<unsigned long long>(lost.failed),
                      noCounts ? (", " + std::to_string(noCounts) + " writers without counts").c_str() : "",
                      static_cast<unsigned long long>(total),
                      static_cast<unsigned long long>(miss),
                      static_cast<unsigned long long>(pend),
                      restarts ? (" | sender restarts " + std::to_string(restarts)).c_str() : "");
        logOut(line);
        return j.str();
    }

private:
    const Options& o_;
    std::unique_ptr<DaosRuntime> rt_;
    std::unique_ptr<DaosPool> pool_;
    std::unique_ptr<DaosContainer> cont_;
    std::unique_ptr<Catalog> cat_;
    std::unique_ptr<RunReader> reader_;
    L::RunSettings settings_;
    bool started_ = false;

    std::map<uint32_t, Writer> writers_;
    std::map<uint64_t, Second> seconds_;
    std::set<uint64_t> gone_;
    std::map<uint16_t, Stream> streams_;
    uint64_t inEvents_ = 0, inBytes_ = 0, outEvents_ = 0, outBytes_ = 0, tick_ = 0;
    double lastTick_ = 0;
    uint64_t span_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
    Options o;
    po::options_description od("Command-line options");
    auto opts = od.add_options();
    opts("help,h", "show this help message");
    opts("pool", po::value<std::string>(&o.pool), "DAOS pool label (required)");
    opts("cont", po::value<std::string>(&o.cont), "the run's container label (required)");
    opts("out,o", po::value<std::string>(&o.out), "JSON lines file, appended (default: <cont>_monitor.jsonl)");
    opts("every", po::value<double>(&o.every)->default_value(2), "seconds between looks at the container");
    opts("span", po::value<uint64_t>(&o.span)->default_value(0), "event numbers per dataId in the strip (0: --strip-seconds of events at the measured rate)");
    opts("strip-seconds", po::value<double>(&o.stripSeconds)->default_value(32), "with --span 0: about this many seconds of events in the strip");
    opts("bins", po::value<size_t>(&o.bins)->default_value(600), "bins per strip");
    opts("grace", po::value<double>(&o.grace)->default_value(5), "seconds an event number may be absent before it counts as a gap");
    opts("restart-gap", po::value<uint64_t>(&o.restartGap)->default_value(EventGaps::kDefaultRestartGap),
         "a jump of more than this many event numbers is a sender restart (a new segment), not a gap");

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
                  << "Example:\n"
                  << "  e2sar_daos_monitor --pool sc26 --cont e2sar_1793280000 -o /scratch/$USER/mon.jsonl\n"
                  << "  python3 viz/daos_monitor/serve.py /scratch/$USER/mon.jsonl     # the dashboard\n";
        return 0;
    }
    if (o.pool.empty() || o.cont.empty()) {
        std::cerr << "--pool and --cont are required" << std::endl;
        return 1;
    }
    if (o.every <= 0 || o.bins == 0 || o.stripSeconds <= 0) {
        std::cerr << "--every, --bins and --strip-seconds must be > 0" << std::endl;
        return 1;
    }
    if (o.out.empty())
        o.out = o.cont + "_monitor.jsonl";
    // A half line left by a crash or a full disk would glue onto the next one: drop it.
    const long long cut = textfile::trimPartialLine(o.out);
    const int fd = cut < 0 ? -1 : ::open(o.out.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd < 0) {
        std::cerr << "cannot write " << o.out << ": " << std::strerror(errno) << std::endl;
        return 1;
    }
    ::close(fd);
    if (cut > 0)
        logErr(o.out + ": dropped a cut-off last line (" + std::to_string(cut) + " bytes)");
    // One line per tick, whole or not at all. While the disk is full the lines are skipped
    // (logged once), and the monitor goes on: the dashboard shows a gap, not a broken file.
    uint64_t skipped = 0;
    auto put = [&](const std::string& line) {
        if (textfile::appendLine(o.out, line, false)) {
            if (skipped) {
                logOut("writing " + o.out + " again after skipping " + std::to_string(skipped) + " lines");
                skipped = 0;
            }
            return;
        }
        if (skipped++ == 0)
            logErr("cannot write " + o.out + ": " + std::strerror(errno) + "; skipping lines until it works");
    };

    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    try {
        Monitor m(o);
        if (!m.open())
            return 0;
        logOut("writing one line per " + std::to_string(o.every) + " s to " + o.out);
        int errors = 0;
        while (running) {
            const auto t0 = std::chrono::steady_clock::now();
            try {
                const std::string line = m.tick();
                if (line.empty())
                    break;  // stopped in the middle of a tick: no partial line
                put(line);
                errors = 0;
            } catch (const DaosError& e) {
                // The container was destroyed (end of a --live-rm run) or a read failed:
                // reopen; a container that no longer exists ends the monitor.
                logErr(std::string("DAOS error: ") + e.what() + "; reopening");
                m.close();
                if (++errors > 1 || e.rc() == -DER_NONEXIST || e.rc() == -DER_NO_HDL) {
                    if (!m.open()) {
                        std::ostringstream end;
                        end << "{\"type\":\"end\",\"t\":" << std::fixed << unixNow()
                            << ",\"why\":\"container destroyed\"}";
                        put(end.str());
                        logOut("container " + o.cont + " is gone: end of the run");
                        return 0;
                    }
                } else if (!m.open()) {
                    return 0;
                }
            }
            nap(o.every - std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        }
    } catch (const DaosError& e) {
        logErr(std::string("DAOS error: ") + e.what());
        return 1;
    }
    return 0;
}
