/**
    e2sar_daos_recv: receive events from an E2SAR sender (e.g. `e2sar_ft -s`) and store
    them in a DAOS key-value object using asynchronous puts on DAOS event queues.

    The pool must exist; the container is created on first use if it does not exist.

    Each reassembled event (one file when the sender is e2sar_ft) is split into chunks
    (default 1 MiB), one KV value per chunk, plus a metadata key written after all
    chunks succeed. See e2sar_daos/event_layout.hpp; e2sar_daos_read reads it back.

    Threading: each dequeue thread (--deq) pulls events from the Reassembler and owns its
    own AsyncKVWriter (one DAOS event queue, --inflight concurrent puts). All threads
    share one pool/container/KV handle.

    Timing: the exit summary reports throughput over the receive window (first to last
    event received) and the commit window (first event received to last event committed),
    so idle time before the sender starts or after it stops doesn't dilute the numbers,
    plus latency distributions per event (reassembled -> committed) and per daos_kv_put.
    --timing-csv writes one row per committed event.

    dfuse mode (--dfuse DIR): instead of the KV object, write each event as one file
    DIR/<prefix>/<prefix>_<evt>_<dataId>.evio (the names e2sar_daos_read --fetch -o uses),
    with blocking open/write/close on each dequeue thread. Meant for a dfuse mount of a
    POSIX container, to compare against the native KV path; the pool/container options
    are then ignored. "Committed" means close() (and fsync() with --fsync) returned.
*/

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include <boost/program_options.hpp>

#include "e2sar.hpp"
#include "e2sar_daos/async_kv_writer.hpp"
#include "e2sar_daos/daos_handles.hpp"
#include "e2sar_daos/event_layout.hpp"
#include "e2sar_daos/latency_histogram.hpp"

namespace po = boost::program_options;
using namespace e2sar;
using namespace e2sar_daos;

namespace {

std::atomic<bool> running{true};

struct Stats {
    std::atomic<uint64_t> eventsReceived{0};
    std::atomic<uint64_t> bytesReceived{0};
    std::atomic<uint64_t> eventsCommitted{0};
    std::atomic<uint64_t> bytesCommitted{0};
    std::atomic<uint64_t> eventsFailed{0};
    std::atomic<uint64_t> putErrors{0};
    std::atomic<uint64_t> recvErrors{0};
    std::atomic<int> lastDaosErr{0};
    std::atomic<int> lastErrno{0};  // dfuse mode
} stats;

// All timestamps are nanoseconds since program start (steady clock).
using Clock = std::chrono::steady_clock;
const Clock::time_point tStart = Clock::now();
// The same instant on the system clock, so CSV times can be matched with other hosts.
const int64_t tStartEpochNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();

int64_t sinceStartNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - tStart).count();
}

struct Timing {
    std::atomic<int64_t> firstRecvNs{-1};
    std::atomic<int64_t> lastRecvNs{-1};
    std::atomic<int64_t> lastCommitNs{-1};
    std::mutex mtx;                 // guards the merged histograms and the CSV
    LatencyHistogram eventLatency;  // reassembled event handed to us -> metadata committed
    LatencyHistogram putLatency;    // each daos_kv_put, launch -> completion (dfuse mode: open -> close)
    std::ofstream csv;
} timing;

void updateMin(std::atomic<int64_t>& a, int64_t v) {
    int64_t cur = a.load();
    while ((cur < 0 || v < cur) && !a.compare_exchange_weak(cur, v)) {}
}

void updateMax(std::atomic<int64_t>& a, int64_t v) {
    int64_t cur = a.load();
    while (v > cur && !a.compare_exchange_weak(cur, v)) {}
}

void onSignal(int) { running = false; }

struct WriterConfig {
    std::string prefix;
    size_t chunkSize;    // 0 = whole event in one value
    size_t maxInflight;  // per thread
    bool verbose;
    std::string dir;     // dfuse mode: <mount>/<prefix>; empty = KV mode
    bool fsync;          // dfuse mode: fsync before close
};

// One reassembled event while its chunks are being written.
struct PendingEvent {
    uint8_t* buf = nullptr;
    size_t size = 0;
    EventNum_t evtNum = 0;
    uint16_t dataId = 0;
    std::string baseKey;
    std::string meta;  // metadata value; must outlive its async put
    uint64_t nchunks = 0;
    int64_t recvNs = 0;  // when recvEvent handed it to us
    size_t remaining = 0;
    int err = 0;

    ~PendingEvent() { delete[] buf; }  // E2SAR allocates events with new[]

    void releasePayload() {
        delete[] buf;
        buf = nullptr;
    }
};

// Receive -> chunk -> async put -> commit metadata. Runs on one thread.
void dequeueLoop(Reassembler* reas, const DaosKV* kv, const WriterConfig cfg, int tid) {
    try {
        // Events whose chunk puts all completed; handled outside the callbacks,
        // since callbacks must not submit. Declared before the writer so it outlives
        // the writer's destructor, which may still run callbacks while draining.
        std::vector<std::shared_ptr<PendingEvent>> done;
        LatencyHistogram eventLatency;  // per thread; merged into timing at the end
        AsyncKVWriter writer(*kv, cfg.maxInflight);

        auto commitDone = [&]() {
            while (!done.empty()) {
                std::vector<std::shared_ptr<PendingEvent>> batch;
                batch.swap(done);
                for (auto& ev : batch) {
                    ev->releasePayload();
                    if (ev->err != 0) {
                        stats.eventsFailed++;
                        std::cerr << "[t" << tid << "] event " << ev->evtNum << "/" << ev->dataId
                                  << " not committed: " << d_errstr(ev->err) << std::endl;
                        continue;
                    }
                    writer.submit(ev->baseKey, ev->meta.data(), ev->meta.size(),
                                  [ev, &cfg, tid, &eventLatency](int rc) {
                        if (rc != 0) {
                            stats.putErrors++;
                            stats.eventsFailed++;
                            stats.lastDaosErr = rc;
                            return;
                        }
                        const int64_t now = sinceStartNs();
                        updateMax(timing.lastCommitNs, now);
                        eventLatency.record(static_cast<uint64_t>(now - ev->recvNs));
                        stats.eventsCommitted++;
                        stats.bytesCommitted += ev->size;
                        if (timing.csv.is_open()) {
                            std::lock_guard<std::mutex> lk(timing.mtx);
                            timing.csv << ev->evtNum << ',' << ev->dataId << ',' << ev->size << ','
                                       << ev->nchunks << ',' << tid << ',' << ev->recvNs / 1e9 << ','
                                       << now / 1e9 << ',' << (now - ev->recvNs) / 1e6 << '\n';
                        }
                        if (cfg.verbose)
                            std::cout << "[t" << tid << "] committed " << ev->baseKey << " ("
                                      << ev->size << " bytes)" << std::endl;
                    });
                }
            }
        };

        while (running) {
            writer.progress(0);
            commitDone();

            uint8_t* buf = nullptr;
            size_t size = 0;
            EventNum_t evtNum = 0;
            uint16_t dataId = 0;
            // With puts in flight, don't sleep in recvEvent: poll the Reassembler and wait
            // on the DAOS event queue instead, so completions are reaped (and timed) promptly.
            const bool busy = writer.inflight() > 0;
            auto res = busy ? reas->getEvent(&buf, &size, &evtNum, &dataId)
                            : reas->recvEvent(&buf, &size, &evtNum, &dataId, 100);
            if (res.has_error()) {
                stats.recvErrors++;
                continue;
            }
            if (res.value() == -1) {  // nothing queued (or stopping)
                if (busy)
                    writer.progress(1000);  // up to 1 ms
                continue;
            }

            const int64_t recvNs = sinceStartNs();
            updateMin(timing.firstRecvNs, recvNs);
            updateMax(timing.lastRecvNs, recvNs);
            stats.eventsReceived++;
            stats.bytesReceived += size;

            auto ev = std::make_shared<PendingEvent>();
            ev->buf = buf;
            ev->size = size;
            ev->evtNum = evtNum;
            ev->dataId = dataId;
            ev->recvNs = recvNs;
            ev->baseKey = eventKey(cfg.prefix, evtNum, dataId);

            const EventMeta meta = EventMeta::forSize(size, cfg.chunkSize);
            ev->meta = meta.toString();
            ev->nchunks = meta.nchunks;
            ev->remaining = meta.nchunks;

            if (meta.nchunks == 0) {
                done.push_back(ev);
                continue;
            }

            for (size_t i = 0; i < meta.nchunks; i++) {
                const size_t off = i * meta.chunk;
                const size_t len = std::min<size_t>(meta.chunk, size - off);
                writer.submit(chunkKey(ev->baseKey, i), ev->buf + off, len,
                              [ev, &done](int rc) {
                                  if (rc != 0) {
                                      stats.putErrors++;
                                      stats.lastDaosErr = rc;
                                      if (ev->err == 0)
                                          ev->err = rc;
                                  }
                                  if (--ev->remaining == 0)
                                      done.push_back(ev);
                              });
            }
        }

        // Flush: wait for outstanding chunks, commit their metadata, wait again.
        writer.drain();
        commitDone();
        writer.drain();

        std::lock_guard<std::mutex> lk(timing.mtx);
        timing.eventLatency.merge(eventLatency);
        timing.putLatency.merge(writer.putLatency());
    } catch (const std::exception& e) {
        std::cerr << "[t" << tid << "] fatal: " << e.what() << std::endl;
        running = false;
    }
}

// Write the whole buffer to a new file; returns 0 or an errno.
int writeFile(const std::string& path, const uint8_t* buf, size_t size, bool doFsync) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return errno;
    int err = 0;
    for (size_t off = 0; off < size && err == 0;) {
        const ssize_t n = ::write(fd, buf + off, size - off);
        if (n < 0 && errno != EINTR)
            err = errno;
        else if (n > 0)
            off += static_cast<size_t>(n);
    }
    if (err == 0 && doFsync && ::fsync(fd) != 0)
        err = errno;
    if (::close(fd) != 0 && err == 0)
        err = errno;
    return err;
}

// dfuse mode: receive -> write one file -> commit. Runs on one thread.
void dfuseLoop(Reassembler* reas, const WriterConfig cfg, int tid) {
    LatencyHistogram eventLatency, writeLatency;  // per thread; merged into timing at the end
    while (running) {
        uint8_t* raw = nullptr;
        size_t size = 0;
        EventNum_t evtNum = 0;
        uint16_t dataId = 0;
        auto res = reas->recvEvent(&raw, &size, &evtNum, &dataId, 100);
        if (res.has_error()) {
            stats.recvErrors++;
            continue;
        }
        if (res.value() == -1)  // nothing queued (or stopping)
            continue;
        std::unique_ptr<uint8_t[]> buf(raw);  // E2SAR allocates events with new[]

        const int64_t recvNs = sinceStartNs();
        updateMin(timing.firstRecvNs, recvNs);
        updateMax(timing.lastRecvNs, recvNs);
        stats.eventsReceived++;
        stats.bytesReceived += size;

        const std::string name = cfg.prefix + "_" + std::to_string(evtNum) + "_" + std::to_string(dataId) + ".evio";
        const int64_t t0 = sinceStartNs();
        const int err = writeFile(cfg.dir + "/" + name, buf.get(), size, cfg.fsync);
        const int64_t now = sinceStartNs();
        if (err != 0) {
            stats.putErrors++;
            stats.eventsFailed++;
            stats.lastErrno = err;
            std::cerr << "[t" << tid << "] event " << evtNum << "/" << dataId << " not written: "
                      << std::strerror(err) << std::endl;
            continue;
        }
        updateMax(timing.lastCommitNs, now);
        eventLatency.record(static_cast<uint64_t>(now - recvNs));
        writeLatency.record(static_cast<uint64_t>(now - t0));
        stats.eventsCommitted++;
        stats.bytesCommitted += size;
        if (timing.csv.is_open()) {
            std::lock_guard<std::mutex> lk(timing.mtx);
            timing.csv << evtNum << ',' << dataId << ',' << size << ",1," << tid << ',' << recvNs / 1e9 << ','
                       << now / 1e9 << ',' << (now - recvNs) / 1e6 << '\n';
        }
        if (cfg.verbose)
            std::cout << "[t" << tid << "] committed " << name << " (" << size << " bytes)" << std::endl;
    }
    std::lock_guard<std::mutex> lk(timing.mtx);
    timing.eventLatency.merge(eventLatency);
    timing.putLatency.merge(writeLatency);
}

void reportLoop(Reassembler* reas, unsigned intervalSec) {
    using clock = std::chrono::steady_clock;
    uint64_t lastBytes = 0, lastEvents = 0, lastRcvdBytes = 0;
    auto lastT = clock::now();
    while (running) {
        for (unsigned i = 0; i < intervalSec * 10 && running; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!running)
            break;

        const auto now = clock::now();
        const double dt = std::chrono::duration<double>(now - lastT).count();
        const uint64_t bytes = stats.bytesCommitted, events = stats.eventsCommitted;
        const uint64_t rcvdBytes = stats.bytesReceived;
        const auto rs = reas->getStats();

        std::cout << std::fixed << std::setprecision(2)
                  << "in " << stats.eventsReceived << " evts, " << (rcvdBytes - lastRcvdBytes) * 8.0 / dt / 1e9
                  << " Gbps | committed " << events << " evts, " << bytes / 1048576.0 << " MiB, "
                  << (bytes - lastBytes) * 8.0 / dt / 1e9 << " Gbps, "
                  << (events - lastEvents) / dt << " evt/s | failed " << stats.eventsFailed
                  << " | reassembly loss " << rs.reassemblyLoss
                  << ", enqueue loss " << rs.enqueueLoss << std::endl;

        lastBytes = bytes;
        lastRcvdBytes = rcvdBytes;
        lastEvents = events;
        lastT = now;
    }
}

result<int> prepareToReceive(Reassembler& r) {
    auto hostname = NetUtil::getHostName();
    if (hostname.has_error())
        return E2SARErrorInfo{hostname.error().code(), hostname.error().message()};

    auto reg = r.registerWorker(hostname.value());  // no-op without control plane
    if (reg.has_error())
        return E2SARErrorInfo{E2SARErrorc::RPCError, "Unable to register worker: " + reg.error().message()};

    // registerWorker before openAndStart: the state thread needs the session token.
    return r.openAndStart();
}

void printFinalStats(Reassembler& r, bool dfuseMode) {
    const auto rs = r.getStats();
    std::cout << "Reassembler:" << std::endl
              << "\tEvents reassembled:        " << rs.eventSuccess << std::endl
              << "\tLost in reassembly:        " << rs.reassemblyLoss << std::endl
              << "\tLost in enqueue:           " << rs.enqueueLoss << std::endl
              << "\tData errors:               " << rs.dataErrCnt << std::endl
              << "\tgRPC errors:               " << rs.grpcErrCnt << std::endl;
    std::cout << (dfuseMode ? "dfuse:" : "DAOS:") << std::endl
              << "\tEvents received:           " << stats.eventsReceived << std::endl
              << "\tEvents committed:          " << stats.eventsCommitted << std::endl
              << "\tBytes committed:           " << stats.bytesCommitted << std::endl
              << "\tEvents failed:             " << stats.eventsFailed << std::endl
              << (dfuseMode ? "\tWrite errors:              " : "\tPut errors:                ") << stats.putErrors << std::endl
              << "\trecvEvent errors:          " << stats.recvErrors << std::endl;
    if (stats.lastDaosErr != 0)
        std::cout << "\tLast DAOS error:           " << d_errstr(stats.lastDaosErr) << std::endl;
    if (stats.lastErrno != 0)
        std::cout << "\tLast write error:          " << std::strerror(stats.lastErrno) << std::endl;
}

std::string fmtRate(uint64_t bytes, uint64_t events, int64_t windowNs) {
    if (windowNs <= 0)
        return "n/a (needs at least 2 events)";
    const double sec = windowNs / 1e9;
    std::ostringstream o;
    o << std::fixed << std::setprecision(3) << bytes * 8.0 / windowNs << " Gbps, "
      << std::setprecision(1) << bytes / 1048576.0 / sec << " MiB/s, " << events / sec << " evt/s";
    return o.str();
}

std::string fmtLatency(const LatencyHistogram& h) {
    if (h.count() == 0)
        return "n/a";
    auto ms = [](double ns) { return ns / 1e6; };
    std::ostringstream o;
    o << std::fixed << std::setprecision(3) << "min " << ms(h.minNs()) << " | mean " << ms(h.meanNs())
      << " | p50 " << ms(h.percentileNs(50)) << " | p90 " << ms(h.percentileNs(90)) << " | p99 "
      << ms(h.percentileNs(99)) << " | max " << ms(h.maxNs()) << " ms  (" << h.count() << ")";
    return o.str();
}

void printTiming(bool dfuseMode) {
    const int64_t first = timing.firstRecvNs, lastRecv = timing.lastRecvNs, lastCommit = timing.lastCommitNs;
    std::cout << "Timing:" << std::endl;
    if (first < 0) {
        std::cout << "\tno events received" << std::endl;
        return;
    }
    std::cout << std::fixed << std::setprecision(3)
              << "\tFirst event received:      " << first / 1e9 << " s after start" << std::endl
              << "\tReceive window:            " << (lastRecv - first) / 1e9 << " s (first -> last event received)" << std::endl
              << "\tCommit window:             " << (lastCommit >= 0 ? (lastCommit - first) / 1e9 : 0.0)
              << " s (first event received -> last event committed)" << std::endl
              << "\tIngest throughput:         " << fmtRate(stats.bytesReceived, stats.eventsReceived, lastRecv - first) << std::endl
              << (dfuseMode ? "\tdfuse commit throughput:   " : "\tDAOS commit throughput:    ")
              << fmtRate(stats.bytesCommitted, stats.eventsCommitted, lastCommit >= 0 ? lastCommit - first : 0) << std::endl
              << "\tEvent latency (recv->commit): " << fmtLatency(timing.eventLatency) << std::endl
              << (dfuseMode ? "\tFile open->close latency:  " : "\tdaos_kv_put latency:       ")
              << fmtLatency(timing.putLatency) << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
    po::options_description od("Command-line options");

    std::string uriStr, recvIP, pool, cont, oclass, prefix, timingCsv, outDir;
    uint16_t recvStartPort;
    size_t numRecvThreads, deqThreads, inflight, chunkKiB;
    std::vector<int> coreList;
    int sockBufSize, eventTimeoutMS, numaNode;
    uint64_t oidLo;
    unsigned reportSec;

    auto opts = od.add_options();
    opts("help,h", "show this help message");
    // E2SAR receive side, same meaning as in e2sar_ft -r
    opts("uri,u", po::value<std::string>(&uriStr), "EJFAT_URI (defaults to the EJFAT_URI environment variable)");
    opts("withcp,c", po::bool_switch()->default_value(false), "enable control plane interactions");
    opts("ip", po::value<std::string>(&recvIP)->default_value(""), "IP address to listen on (conflicts with --autoip)");
    opts("autoip", po::bool_switch()->default_value(false), "auto-detect the listening IP from the URI data address");
    opts("port", po::value<uint16_t>(&recvStartPort)->default_value(10000), "starting UDP receive port");
    opts("threads", po::value<size_t>(&numRecvThreads)->default_value(1), "number of E2SAR receive threads");
    opts("cores", po::value<std::vector<int>>(&coreList)->multitoken(), "cores to bind E2SAR receive threads to (conflicts with --threads)");
    opts("bufsize,b", po::value<int>(&sockBufSize)->default_value(3 * 1024 * 1024), "receive socket buffer size");
    opts("timeout", po::value<int>(&eventTimeoutMS)->default_value(500), "event reassembly timeout in ms");
    opts("numa", po::value<int>(&numaNode)->default_value(-1), "bind memory allocation to this NUMA node (if >= 0)");
    opts("ipv4,4", "force IPv4 control plane address if the URI has a hostname (disables cert validation)");
    opts("ipv6,6", "force IPv6 control plane address if the URI has a hostname (disables cert validation)");
    opts("novalidate,v", "don't validate the control plane server certificate");
    // DAOS side
    opts("pool", po::value<std::string>(&pool)->default_value("sc26"), "DAOS pool label (must exist)");
    opts("cont", po::value<std::string>(&cont)->default_value("e2sar"), "DAOS container label (created if missing)");
    opts("oid", po::value<uint64_t>(&oidLo)->default_value(0xE25A), "low 64 bits of the KV object id");
    opts("oclass", po::value<std::string>(&oclass)->default_value(""), "DAOS object class, e.g. SX or RP_2G1 (default: container default)");
    opts("prefix", po::value<std::string>(&prefix)->default_value("run"), "key prefix; use a per-run value, since e2sar_ft restarts event numbers at 0");
    opts("chunk", po::value<size_t>(&chunkKiB)->default_value(1024), "chunk size in KiB for splitting events into KV values (0 = one value per event)");
    opts("inflight", po::value<size_t>(&inflight)->default_value(64), "max concurrent async puts per dequeue thread");
    opts("deq", po::value<size_t>(&deqThreads)->default_value(1), "number of dequeue threads, each with its own DAOS event queue");
    // dfuse mode (POSIX container mounted with dfuse)
    opts("dfuse", po::value<std::string>(&outDir), "dfuse mount point: write each event as a file under DIR/<prefix>/ instead of the KV object; --pool/--cont/--oid/--oclass/--chunk/--inflight are ignored");
    opts("fsync", po::bool_switch()->default_value(false), "with --dfuse: fsync each file before closing it");
    opts("report", po::value<unsigned>(&reportSec)->default_value(1), "progress report interval in seconds (0 = off)");
    opts("verbose", po::bool_switch()->default_value(false), "print every committed event");
    opts("timing-csv", po::value<std::string>(&timingCsv), "write one CSV row of timing per committed event to this file");

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
                  << "Example (pairs with `e2sar_ft -s -u <uri> --ip <sender ip> /path/to/files`):\n"
                  << "  e2sar_daos_recv -u 'ejfat://...@host:port/lb/1?data=<recv ip>' --ip <recv ip> \\\n"
                  << "      --pool sc26 --cont e2sar --prefix run42 --deq 2 --inflight 64" << std::endl;
        return 0;
    }

    const bool withCP = vm["withcp"].as<bool>();
    const bool autoIP = vm["autoip"].as<bool>();
    if (autoIP == !recvIP.empty()) {
        std::cerr << "Specify exactly one of --ip or --autoip" << std::endl;
        return 1;
    }
    if (vm.count("cores") && !vm["threads"].defaulted()) {
        std::cerr << "--cores and --threads are mutually exclusive" << std::endl;
        return 1;
    }
    if (vm.count("ipv4") && vm.count("ipv6")) {
        std::cerr << "--ipv4 and --ipv6 are mutually exclusive" << std::endl;
        return 1;
    }
    if (deqThreads == 0 || inflight == 0) {
        std::cerr << "--deq and --inflight must be > 0" << std::endl;
        return 1;
    }
    if (prefix.empty() || prefix.find('/') != std::string::npos) {
        std::cerr << "--prefix must be non-empty and must not contain '/'" << std::endl;
        return 1;
    }

    if (numaNode >= 0) {
        auto numaRes = Affinity::setNUMABind(numaNode);
        if (numaRes.has_error()) {
            std::cerr << "Unable to bind to NUMA node: " << numaRes.error().message() << std::endl;
            return 1;
        }
    }

    const bool preferV6 = vm.count("ipv6") > 0;
    const bool preferHostAddr = vm.count("ipv4") || vm.count("ipv6");

    auto uriRes = vm.count("uri")
        ? EjfatURI::getFromString(uriStr, EjfatURI::TokenType::instance, preferV6)
        : EjfatURI::getFromEnv("EJFAT_URI", EjfatURI::TokenType::instance, preferV6);
    if (uriRes.has_error()) {
        std::cerr << "Unable to parse EJFAT URI: " << uriRes.error().message() << std::endl;
        return 1;
    }

    if (!timingCsv.empty()) {
        timing.csv.open(timingCsv, std::ios::trunc);
        if (!timing.csv) {
            std::cerr << "Unable to open --timing-csv file " << timingCsv << std::endl;
            return 1;
        }
        // times in seconds since program start, microsecond resolution
        timing.csv << std::fixed << std::setprecision(6);
        timing.csv << "evt_num,data_id,bytes,chunks,thread,recv_s,commit_s,latency_ms\n";
    }

    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    std::cout << "E2SAR version:        " << get_Version() << std::endl
              << "Clock start:          " << tStartEpochNs << " ns since epoch (--timing-csv times are relative to it)" << std::endl
              << "DAOS version:         " << DAOS_VERSION_MAJOR << "." << DAOS_VERSION_MINOR << "."
              << DAOS_VERSION_FIX << std::endl;

    const bool dfuseMode = !outDir.empty();
    try {
        // Storage first: fail before registering with the load balancer if it is unavailable.
        std::unique_ptr<DaosRuntime> daos;
        std::unique_ptr<DaosPool> daosPool;
        std::unique_ptr<DaosContainer> daosCont;
        std::unique_ptr<DaosKV> kv;
        if (dfuseMode) {
            outDir += "/" + prefix;
            if (::mkdir(outDir.c_str(), 0755) != 0 && errno != EEXIST) {
                std::cerr << "Unable to create " << outDir << ": " << std::strerror(errno) << std::endl;
                return 1;
            }
            std::cout << "Output directory:     " << outDir << (vm["fsync"].as<bool>() ? " (fsync)" : "") << std::endl
                      << "Dequeue threads:      " << deqThreads << " (blocking file writes)" << std::endl;
        } else {
            daos = std::make_unique<DaosRuntime>();
            daosPool = std::make_unique<DaosPool>(pool);
            daosCont = std::make_unique<DaosContainer>(*daosPool, cont, /*createIfMissing=*/true);
            kv = std::make_unique<DaosKV>(*daosCont, oidLo, 0, oclass);

            std::cout << "DAOS pool/container:  " << pool << "/" << cont
                      << (daosCont->created() ? " (created)" : " (existing)") << std::endl
                      << "KV object id:         " << std::hex << "0x" << kv->oid().hi << ".0x" << kv->oid().lo
                      << std::dec << std::endl
                      << "Key prefix:           " << prefix << std::endl
                      << "Chunk size:           " << (chunkKiB ? std::to_string(chunkKiB) + " KiB" : "whole event") << std::endl
                      << "Dequeue threads:      " << deqThreads << " x " << inflight << " in-flight puts" << std::endl;
        }

        Reassembler::ReassemblerFlags rflags;
        rflags.useCP = withCP;  // E2SAR >= 0.4 expects the LB header iff the control plane is off
        rflags.rcvSocketBufSize = sockBufSize;
        rflags.useHostAddress = preferHostAddr;
        rflags.validateCert = !vm.count("novalidate");
        rflags.eventTimeout_ms = eventTimeoutMS;

        std::unique_ptr<Reassembler> reas;
        const auto& uri = uriRes.value();
        if (vm.count("cores")) {
            reas = autoIP ? std::make_unique<Reassembler>(uri, recvStartPort, coreList, rflags)
                          : std::make_unique<Reassembler>(uri, ip::make_address(recvIP), recvStartPort, coreList, rflags);
        } else {
            reas = autoIP ? std::make_unique<Reassembler>(uri, recvStartPort, numRecvThreads, rflags)
                          : std::make_unique<Reassembler>(uri, ip::make_address(recvIP), recvStartPort, numRecvThreads, rflags);
        }

        std::cout << "Control plane:        " << (withCP ? "ON" : "OFF") << std::endl
                  << "Listening on:         " << reas->get_dataIP() << " ports "
                  << reas->get_recvPorts().first << ":" << reas->get_recvPorts().second << std::endl;

        auto prep = prepareToReceive(*reas);
        if (prep.has_error()) {
            std::cerr << "Unable to start reassembler: " << prep.error().message() << std::endl;
            reas->stopThreads();
            return 1;
        }
        std::cout << "Receiving; Ctrl-C to stop" << std::endl;

        const WriterConfig cfg{prefix, chunkKiB * 1024, inflight, vm["verbose"].as<bool>(), outDir,
                               vm["fsync"].as<bool>()};
        std::vector<std::thread> workers;
        for (size_t i = 0; i < deqThreads; i++) {
            if (dfuseMode)
                workers.emplace_back(dfuseLoop, reas.get(), cfg, static_cast<int>(i));
            else
                workers.emplace_back(dequeueLoop, reas.get(), kv.get(), cfg, static_cast<int>(i));
        }
        std::thread reporter;
        if (reportSec > 0)
            reporter = std::thread(reportLoop, reas.get(), reportSec);

        for (auto& t : workers)
            t.join();  // returns after Ctrl-C, once all in-flight puts are flushed
        running = false;
        if (reporter.joinable())
            reporter.join();

        std::cout << "Stopping reassembler" << std::endl;
        auto dereg = reas->deregisterWorker();
        if (dereg.has_error())
            std::cerr << "Unable to deregister worker: " << dereg.error().message() << std::endl;
        reas->stopThreads();
        printFinalStats(*reas, dfuseMode);
        printTiming(dfuseMode);
        if (timing.csv.is_open()) {
            timing.csv.close();
            std::cout << "Per-event timing written to " << timingCsv << std::endl;
        }
    } catch (const DaosError& e) {
        std::cerr << "DAOS error: " << e.what() << std::endl;
        return 1;
    } catch (const E2SARException& e) {
        std::cerr << "E2SAR error: " << static_cast<std::string>(e) << std::endl;
        return 1;
    }
    return stats.eventsFailed == 0 ? 0 : 2;
}
