/**
    e2sar_daos_monitor: watch one run's DAOS container (docs/DAOS_LAYOUT.md) and write its
    state every --every seconds (default 2) as one JSON line, for the live dashboard
    viz/daos_monitor/ (docs/STREAMING_WORKFLOW.md#monitoring).

    Read-only. It reads only the container, never the reader's files, so it runs with or
    without e2sar_daos_read and doesn't change what writers or readers do:

      incoming   events newly committed: index entries not seen before
      outgoing   events that left DAOS: their index second was removed (e2sar_daos_read --live-rm)
      in DAOS    incoming - outgoing
      missing    event numbers absent, per dataId, for more than --grace seconds while a
                 higher number of that dataId is committed. Younger gaps are "pending"
                 (another writer may still commit them). A gap filled later counts as late.
                 Reported both as still open and as a cumulative total that never goes down.

    Each line also holds, per dataId, the state of its event numbers
    (in DAOS / left DAOS / missing / pending) over its newest --span event numbers (default
    about 32 s of events), in --bins bins, run-length encoded: the dashboard draws them as a
    moving strip. Lines go to --out (appended), so a run can be replayed.

    It waits for the container if the run hasn't started, and exits when the container is
    destroyed (end of a --live-rm run) or on Ctrl-C.
*/

#include <signal.h>

#include <algorithm>
#include <atomic>
#include <chrono>
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
#include "e2sar_daos/layout.hpp"
#include "e2sar_daos/run_reader.hpp"

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
    size_t bins = 600;
};

// ------------------------------------------------------------------ per-dataId state

struct Gap {
    uint64_t last = 0;
    double since = 0;      // when it opened (unix s)
    bool counted = false;  // already added to the cumulative missing count
};

// Bin states, by priority: a bin shows the most important state among its event numbers.
enum : char { kUnknown = 'u', kOut = 'o', kIn = 'd', kPending = 'p', kMissing = 'm' };
int rank(char c) { return c == kMissing ? 4 : c == kPending ? 3 : c == kIn ? 2 : c == kOut ? 1 : 0; }

struct Stream {
    uint64_t events = 0, bytes = 0, prevEvents = 0;
    bool any = false;
    uint64_t first = 0, max = 0;
    std::map<uint64_t, Gap> gaps;  // first -> gap
    // Committed event numbers near the top, as runs [start, end] with one commit second:
    // start -> (end, second). Events mostly come in order, so a second is a few runs.
    std::map<uint64_t, std::pair<uint64_t, uint64_t>> runs;
    uint64_t late = 0, dups = 0;
    uint64_t everMissing = 0;  // cumulative: each number counted once, when it first counts as missing

    void add(uint64_t evt, uint64_t second, uint64_t size, double now, uint64_t span) {
        events++;
        bytes += size;
        if (!any) {
            any = true;
            first = max = evt;
        } else if (evt > max) {
            if (evt > max + 1)
                gaps[max + 1] = {evt - 1, now};
            max = evt;
        } else if (fill(evt)) {
            late++;
        } else if (evt >= first) {
            dups++;
            return;
        } else {
            first = evt;  // below the first event seen: the stream started earlier
        }
        if (evt + span > max)
            addRun(evt, second);
    }

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

    // Take n out of its gap, if it is in one.
    bool fill(uint64_t n) {
        auto it = gaps.upper_bound(n);
        if (it == gaps.begin())
            return false;
        --it;
        const uint64_t lo = it->first;
        const Gap g = it->second;
        if (n > g.last)
            return false;
        gaps.erase(it);
        if (lo < n)
            gaps[lo] = {n - 1, g.since, g.counted};
        if (n < g.last)
            gaps[n + 1] = {g.last, g.since, g.counted};
        return true;
    }

    // Missing (older than grace, still open) and pending event numbers. Gaps that just
    // passed grace are added to everMissing, once: it never goes down, even when a late
    // event fills its gap.
    std::pair<uint64_t, uint64_t> missing(double now, double grace) {
        uint64_t m = 0, p = 0;
        for (auto& [lo, g] : gaps) {
            if (now - g.since > grace) {
                m += g.last - lo + 1;
                if (!g.counted) {
                    everMissing += g.last - lo + 1;
                    g.counted = true;
                }
            } else {
                p += g.last - lo + 1;
            }
        }
        return {m, p};
    }

    // Forget runs below lo.
    void prune(uint64_t lo) {
        while (!runs.empty() && runs.begin()->second.first < lo)
            runs.erase(runs.begin());
    }

    /**
        The state of event numbers [lo, lo + bins × width) in `bins` bins of `width`, one
        character per bin, run-length encoded ("d120o30m2p10"): m = missing, p = pending,
        d = in DAOS, o = left DAOS, u = unknown (not committed yet, before the first event the
        monitor saw, or removed before the monitor started). A bin shows its most
        important state, in that order, so one missing event marks its bin.
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
        auto g = gaps.upper_bound(lo);
        if (g != gaps.begin())
            --g;
        for (; g != gaps.end() && g->first <= hi; ++g)
            mark(g->first, g->second.last, now - g->second.since > grace ? kMissing : kPending);
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
            streams_[e.dataId].add(e.evtNum, e.second, e.size, now, span_ ? span_ : ~0ull >> 1);

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
        bool first = true;
        for (const auto& [s, w] : writers_) {
            j << (first ? "" : ",") << "{\"slot\":" << s << ",\"done\":" << (w.done ? "true" : "false");
            if (w.wm)
                j << ",\"wm_index\":" << w.wm->index << ",\"wm_data\":" << w.wm->data << ",\"lag\":" << now - w.wm->index;
            j << ",\"events\":" << w.events << ",\"bytes\":" << w.bytes << "}";
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
        uint64_t miss = 0, pend = 0, late = 0, dups = 0, total = 0;
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
            const uint64_t lo = st.max + 1 >= width * o_.bins ? st.max + 1 - width * o_.bins : 0;
            st.prune(lo);
            const auto [m, p] = st.missing(now, o_.grace);
            miss += m;
            pend += p;
            late += st.late;
            dups += st.dups;
            total += st.everMissing;
            j << (first ? "" : ",") << "{\"id\":" << id << ",\"events\":" << st.events << ",\"bytes\":" << st.bytes
              << ",\"d_events\":" << st.events - st.prevEvents << ",\"first\":" << st.first << ",\"max\":" << st.max
              << ",\"missing\":" << m << ",\"missing_total\":" << st.everMissing << ",\"pending\":" << p << ",\"late\":" << st.late << ",\"dups\":" << st.dups
              << ",\"strip_lo\":" << lo;
            if (span_)
                j << ",\"strip\":\"" << st.strip(lo, width, o_.bins, gone_, now, o_.grace) << "\"";
            j << "}";
            st.prevEvents = st.events;
            first = false;
        }
        j << "]";
        j << ",\"missing\":{\"total\":" << total << ",\"events\":" << miss << ",\"pending\":" << pend << ",\"late\":" << late
          << ",\"dups\":" << dups << "}}";

        char line[256];
        std::snprintf(line, sizeof(line),
                      "in %llu events (+%zu) | out %llu (+%llu) | in DAOS %llu events, %.1f MiB, %zu s | missing %llu so far (%llu open), pending %llu",
                      static_cast<unsigned long long>(inEvents_), evs.size(),
                      static_cast<unsigned long long>(outEvents_), static_cast<unsigned long long>(outEvents),
                      static_cast<unsigned long long>(inEvents_ - outEvents_),
                      (inBytes_ - outBytes_) / 1048576.0, listed.size(), static_cast<unsigned long long>(total),
                      static_cast<unsigned long long>(miss),
                      static_cast<unsigned long long>(pend));
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
    opts("grace", po::value<double>(&o.grace)->default_value(5), "seconds an event number may be absent before it counts as missing");

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
    std::ofstream out(o.out, std::ios::app);
    if (!out) {
        std::cerr << "cannot open " << o.out << std::endl;
        return 1;
    }

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
                out << m.tick() << std::endl;
                errors = 0;
            } catch (const DaosError& e) {
                // The container was destroyed (end of a --live-rm run) or a read failed:
                // reopen; a container that no longer exists ends the monitor.
                logErr(std::string("DAOS error: ") + e.what() + "; reopening");
                m.close();
                if (++errors > 1 || e.rc() == -DER_NONEXIST || e.rc() == -DER_NO_HDL) {
                    if (!m.open()) {
                        out << "{\"type\":\"end\",\"t\":" << std::fixed << unixNow()
                            << ",\"why\":\"container destroyed\"}" << std::endl;
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
