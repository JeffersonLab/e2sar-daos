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
*/

#include <signal.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

#include <boost/program_options.hpp>

#include "e2sar.hpp"
#include "e2sar_daos/async_kv_writer.hpp"
#include "e2sar_daos/daos_handles.hpp"
#include "e2sar_daos/event_layout.hpp"

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
} stats;

void onSignal(int) { running = false; }

struct WriterConfig {
    std::string prefix;
    size_t chunkSize;    // 0 = whole event in one value
    size_t maxInflight;  // per thread
    bool verbose;
};

// One reassembled event while its chunks are being written.
struct PendingEvent {
    uint8_t* buf = nullptr;
    size_t size = 0;
    EventNum_t evtNum = 0;
    uint16_t dataId = 0;
    std::string baseKey;
    std::string meta;  // metadata value; must outlive its async put
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
                    writer.submit(ev->baseKey, ev->meta.data(), ev->meta.size(), [ev, &cfg, tid](int rc) {
                        if (rc != 0) {
                            stats.putErrors++;
                            stats.eventsFailed++;
                            stats.lastDaosErr = rc;
                            return;
                        }
                        stats.eventsCommitted++;
                        stats.bytesCommitted += ev->size;
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
            auto res = reas->recvEvent(&buf, &size, &evtNum, &dataId, 100);
            if (res.has_error()) {
                stats.recvErrors++;
                continue;
            }
            if (res.value() == -1)  // timed out or stopping
                continue;

            stats.eventsReceived++;
            stats.bytesReceived += size;

            auto ev = std::make_shared<PendingEvent>();
            ev->buf = buf;
            ev->size = size;
            ev->evtNum = evtNum;
            ev->dataId = dataId;
            ev->baseKey = eventKey(cfg.prefix, evtNum, dataId);

            const EventMeta meta = EventMeta::forSize(size, cfg.chunkSize);
            ev->meta = meta.toString();
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
    } catch (const std::exception& e) {
        std::cerr << "[t" << tid << "] fatal: " << e.what() << std::endl;
        running = false;
    }
}

void reportLoop(Reassembler* reas, unsigned intervalSec) {
    using clock = std::chrono::steady_clock;
    uint64_t lastBytes = 0, lastEvents = 0;
    auto lastT = clock::now();
    while (running) {
        for (unsigned i = 0; i < intervalSec * 10 && running; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!running)
            break;

        const auto now = clock::now();
        const double dt = std::chrono::duration<double>(now - lastT).count();
        const uint64_t bytes = stats.bytesCommitted, events = stats.eventsCommitted;
        const auto rs = reas->getStats();

        std::cout << std::fixed << std::setprecision(2)
                  << "rcvd " << stats.eventsReceived << " evts | committed " << events
                  << " evts, " << bytes / 1048576.0 << " MiB | "
                  << (bytes - lastBytes) * 8.0 / dt / 1e9 << " Gbps, "
                  << (events - lastEvents) / dt << " evt/s | failed " << stats.eventsFailed
                  << " | reassembly loss " << rs.reassemblyLoss
                  << ", enqueue loss " << rs.enqueueLoss << std::endl;

        lastBytes = bytes;
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

void printFinalStats(Reassembler& r) {
    const auto rs = r.getStats();
    std::cout << "Reassembler:" << std::endl
              << "\tEvents reassembled:        " << rs.eventSuccess << std::endl
              << "\tLost in reassembly:        " << rs.reassemblyLoss << std::endl
              << "\tLost in enqueue:           " << rs.enqueueLoss << std::endl
              << "\tData errors:               " << rs.dataErrCnt << std::endl
              << "\tgRPC errors:               " << rs.grpcErrCnt << std::endl;
    std::cout << "DAOS:" << std::endl
              << "\tEvents received:           " << stats.eventsReceived << std::endl
              << "\tEvents committed:          " << stats.eventsCommitted << std::endl
              << "\tBytes committed:           " << stats.bytesCommitted << std::endl
              << "\tEvents failed:             " << stats.eventsFailed << std::endl
              << "\tPut errors:                " << stats.putErrors << std::endl
              << "\trecvEvent errors:          " << stats.recvErrors << std::endl;
    if (stats.lastDaosErr != 0)
        std::cout << "\tLast DAOS error:           " << d_errstr(stats.lastDaosErr) << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
    po::options_description od("Command-line options");

    std::string uriStr, recvIP, pool, cont, oclass, prefix;
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
    opts("report", po::value<unsigned>(&reportSec)->default_value(1), "progress report interval in seconds (0 = off)");
    opts("verbose", po::bool_switch()->default_value(false), "print every committed event");

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

    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    std::cout << "E2SAR version:        " << get_Version() << std::endl
              << "DAOS version:         " << DAOS_VERSION_MAJOR << "." << DAOS_VERSION_MINOR << "."
              << DAOS_VERSION_FIX << std::endl;

    try {
        // DAOS first: fail before registering with the load balancer if storage is unavailable.
        DaosRuntime daos;
        DaosPool daosPool(pool);
        DaosContainer daosCont(daosPool, cont, /*createIfMissing=*/true);
        DaosKV kv(daosCont, oidLo, 0, oclass);

        std::cout << "DAOS pool/container:  " << pool << "/" << cont
                  << (daosCont.created() ? " (created)" : " (existing)") << std::endl
                  << "KV object id:         " << std::hex << "0x" << kv.oid().hi << ".0x" << kv.oid().lo
                  << std::dec << std::endl
                  << "Key prefix:           " << prefix << std::endl
                  << "Chunk size:           " << (chunkKiB ? std::to_string(chunkKiB) + " KiB" : "whole event") << std::endl
                  << "Dequeue threads:      " << deqThreads << " x " << inflight << " in-flight puts" << std::endl;

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

        const WriterConfig cfg{prefix, chunkKiB * 1024, inflight, vm["verbose"].as<bool>()};
        std::vector<std::thread> workers;
        for (size_t i = 0; i < deqThreads; i++)
            workers.emplace_back(dequeueLoop, reas.get(), &kv, cfg, static_cast<int>(i));
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
        printFinalStats(*reas);
    } catch (const DaosError& e) {
        std::cerr << "DAOS error: " << e.what() << std::endl;
        return 1;
    } catch (const E2SARException& e) {
        std::cerr << "E2SAR error: " << static_cast<std::string>(e) << std::endl;
        return 1;
    }
    return stats.eventsFailed == 0 ? 0 : 2;
}
