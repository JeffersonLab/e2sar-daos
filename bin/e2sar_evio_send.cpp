/**
    e2sar_evio_send: send synthetic EVIO v6 streaming time frames with E2SAR Segmenters,
    for end-to-end throughput and latency tests with e2sar_daos_recv.

    Each E2SAR event is one EVIO v6 record (14-word record header, index array when it
    holds more than one frame) carrying --frames built time-frame banks. A frame has the
    layout of the CODA streaming data in frames_thread0_*.evio and EC5_EC6_cosmics_*.evio:

      bank 0xFF60 (bank of banks)               built time frame
        bank 0xFF31 (bank of segments)          stream info: TSS 0x32 (frame, timestamp), AIS 0x42 (ROC ids)
        bank <ROC id> (bank of banks) x --rocs
          bank 0xFF30                           ROC stream info: TSS 0x31, AIS 0x41 (port info)
          bank <port 1,3,...,15> x 8            hit words; ~74% empty, as in the real data

    With the defaults (2 ROCs, --hits 23) a frame is ~350 bytes, like the real ones.
    Frame n has timestamp n * 65536 ns.

    Threading: --segmenters P independent Segmenters, each driven by its own thread with
    dataId = --dataid + i and eventSrcId = --srcid + i, so the receiver keeps their events
    apart (DAOS key <prefix>/<evtNum>/<dataId>). Each Segmenter sends on --sockets threads.
    Each one sends --events events, numbered 1..N. Event n of Segmenter i is copy
    n % --pool of a pool of distinct events built up front, so its SHA-256 is known:
    --digests writes "<dataId> <slot> <sha256>" for every pool slot.

    Paths: with --withcp the events go to the LB, which spreads them over the receiver's ports
    by their random entropy. Without it (node-to-node) everything goes to the data= address in
    the URI; add --port-per-segmenter so Segmenter i sends to data port + i, which spreads
    them over the receiver's threads (--threads >= --segmenters).

    Timing: --timing-csv writes one row per event with system-clock (epoch) times:
    queue_ns when it was handed to the Segmenter and sent_ns when its last packet was sent.
    Joined with e2sar_daos_recv --timing-csv on (evt_num, data_id) this splits the latency
    into sender, network + LB + reassembly, and DAOS commit.

    The defaults (16 Segmenters x 1 socket, 256 frames = ~95 KB per event, 20 Gbps) are the
    highest setting that was lossless in repeated runs on the ebpf testbed, with
    e2sar_daos_recv --threads 32 --deq 16 --inflight 128 (docs/E2E_BENCHMARK.md).

    --rate is the total for all Segmenters, paced by the producing threads (the Segmenter's
    own rate limit works in whole microseconds per event, too coarse for small events).
    Segmenter i starts i/P of an event interval after Segmenter 0, so the Segmenters' events
    interleave instead of leaving at the same instant (--no-stagger: all start together).
*/

#include <signal.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <boost/program_options.hpp>
#include <openssl/evp.h>

#include "e2sar.hpp"

namespace po = boost::program_options;
using namespace e2sar;

namespace {

std::atomic<bool> running{true};

void onSignal(int) { running = false; }

uint64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

// ------------------------------------------------------------------ synthetic EVIO

struct FrameShape {
    unsigned rocs = 2;
    double hitsPerFrame = 23;  // mean hit words per frame (real data: ~23.5)
};

class Words {
public:
    void put(uint32_t w) { v.push_back(w); }
    size_t size() const { return v.size(); }
    uint32_t& at(size_t i) { return v[i]; }
    std::vector<uint32_t> v;
};

// One built time frame bank (tag 0xFF60), word for word in the layout of the real data.
void appendFrame(Words& w, uint64_t frame, const FrameShape& s, std::mt19937_64& rng) {
    const uint64_t ts = frame * 65536;
    const unsigned kPorts = 8;
    const double pNonEmpty = 0.26;
    const double meanPerBank = std::max(1.0, s.hitsPerFrame / (s.rocs * kPorts * pNonEmpty));
    std::bernoulli_distribution nonEmpty(pNonEmpty);
    std::uniform_int_distribution<unsigned> extra(0, static_cast<unsigned>(2 * (meanPerBank - 1)));

    const size_t top = w.size();
    w.put(0);                                    // length, filled in below
    w.put(0xFF601000u | (s.rocs & 0xff));        // tag FF60, bank of banks, num = ROCs
    w.put(5 + s.rocs);                           // stream info bank
    w.put(0xFF312000u | (s.rocs & 0xff));
    w.put(0x32010003u);                          // TSS segment: frame, timestamp lo, hi
    w.put(static_cast<uint32_t>(frame));
    w.put(static_cast<uint32_t>(ts));
    w.put(static_cast<uint32_t>(ts >> 32));
    w.put(0x42010000u | s.rocs);                 // AIS segment: one word per ROC
    for (unsigned r = 0; r < s.rocs; r++)
        w.put((0x12u + r) << 16);

    for (unsigned r = 0; r < s.rocs; r++) {
        const size_t roc = w.size();
        w.put(0);
        w.put(((0x12u + r) << 16) | 0x1011u);    // tag = ROC id, bank of banks
        w.put(0xa);                              // ROC stream info bank
        w.put(0xFF302011u);
        w.put(0x31010003u);                      // TSS segment
        w.put(static_cast<uint32_t>(frame));
        w.put(static_cast<uint32_t>(ts));
        w.put(static_cast<uint32_t>(ts >> 32));
        w.put(0x41050004u);                      // AIS segment: 16-bit info per port
        w.put(0x00830081u); w.put(0x00870085u); w.put(0x008b0089u); w.put(0x008f008du);
        for (unsigned p = 0; p < kPorts; p++) {
            const uint32_t port = 2 * p + 1;
            if (!nonEmpty(rng)) {
                w.put(1);
                w.put((port << 16) | 0x0001u);   // empty payload bank, as written by the DAQ
                continue;
            }
            const unsigned n = 1 + extra(rng);
            w.put(n + 1);
            w.put(port << 16);
            for (unsigned h = 0; h < n; h++) {   // hit word: time (16), flags, channel, charge
                const uint64_t x = rng();
                w.put(static_cast<uint32_t>((x & 0xffff) << 16) | ((x >> 16) & 1 ? 0x6000u : 0x4000u) |
                      static_cast<uint32_t>((x >> 20) & 0xff));
            }
        }
        w.at(roc) = static_cast<uint32_t>(w.size() - roc - 1);
    }
    w.at(top) = static_cast<uint32_t>(w.size() - top - 1);
}

// One EVIO v6 record holding `frames` frames, starting at frame number `first`.
std::vector<uint8_t> buildRecord(uint32_t recordNum, uint64_t first, unsigned frames,
                                 const FrameShape& s, std::mt19937_64& rng) {
    Words body;
    std::vector<uint32_t> lens;
    for (unsigned f = 0; f < frames; f++) {
        const size_t before = body.size();
        appendFrame(body, first + f, s, rng);
        lens.push_back(static_cast<uint32_t>((body.size() - before) * 4));
    }
    const uint32_t indexWords = frames > 1 ? frames : 0;   // real files: no index for 1 event
    const uint32_t dataWords = indexWords + static_cast<uint32_t>(body.size());

    Words w;
    w.put(14 + dataWords);          // record length (words)
    w.put(recordNum);               // record number
    w.put(14);                      // header length (words)
    w.put(frames);                  // event count
    w.put(indexWords * 4);          // index array length (bytes)
    w.put(0x80004206u);             // bit info + version 6, as in the real data
    w.put(0);                       // user header length
    w.put(0xc0da0100u);             // magic
    w.put(dataWords);               // uncompressed data length (words, as in the real data)
    for (int i = 0; i < 5; i++)
        w.put(0);                   // compression, user registers
    if (indexWords)
        for (auto l : lens)
            w.put(l);
    for (auto x : body.v)
        w.put(x);

    std::vector<uint8_t> out(w.size() * 4);
    for (size_t i = 0; i < w.size(); i++) {   // big-endian, like the DAQ output
        out[4 * i] = w.v[i] >> 24;
        out[4 * i + 1] = w.v[i] >> 16;
        out[4 * i + 2] = w.v[i] >> 8;
        out[4 * i + 3] = w.v[i];
    }
    return out;
}

std::string sha256(const std::vector<uint8_t>& b) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_Digest(b.data(), b.size(), md, &len, EVP_sha256(), nullptr);
    std::string hex;
    char x[3];
    for (unsigned i = 0; i < len; i++) {
        std::snprintf(x, sizeof(x), "%02x", md[i]);
        hex += x;
    }
    return hex;
}

// ------------------------------------------------------------------ sending

// The URI with every data=<host>:<port> port moved up by `by` (node-to-node, no LB).
EjfatURI shiftDataPort(const std::string& uriStr, size_t by) {
    std::string out;
    size_t pos = 0;
    for (size_t d; (d = uriStr.find("data=", pos)) != std::string::npos;) {
        const size_t end = std::min(uriStr.find('&', d), uriStr.size());
        const size_t colon = uriStr.rfind(':', end - 1);
        if (colon == std::string::npos || colon < d || uriStr[colon - 1] == '[')
            throw std::runtime_error("--port-per-segmenter needs data=<host>:<port> in the URI");
        out += uriStr.substr(pos, colon + 1 - pos) + std::to_string(std::stoul(uriStr.substr(colon + 1, end - colon - 1)) + by);
        pos = end;
    }
    out += uriStr.substr(pos);
    auto r = EjfatURI::getFromString(out, EjfatURI::TokenType::instance);
    if (r.has_error())
        throw std::runtime_error("bad URI " + out + ": " + r.error().message());
    return r.value();
}

struct EventTimes {
    uint64_t queueNs = 0;
    uint64_t sentNs = 0;
};

struct SentArg {
    EventTimes* t;
    std::atomic<uint64_t>* done;
};

void onSent(boost::any a) {
    auto s = boost::any_cast<SentArg>(a);
    s.t->sentNs = nowNs();
    s.done->fetch_add(1, std::memory_order_relaxed);
}

struct Worker {
    std::unique_ptr<Segmenter> seg;
    uint16_t dataId = 0;
    std::vector<std::vector<uint8_t>> pool;
    std::vector<EventTimes> times;
    std::atomic<uint64_t> queued{0}, done{0};
    uint64_t bytes = 0;
};

// Pace to `gbps` (total over all workers, so each gets gbps / nWorkers); < 0 = unlimited.
// The first event is due `offsetNs` after the start.
void produce(Worker& w, uint64_t events, double gbps, double offsetNs) {
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t sentBytes = 0;
    for (uint64_t e = 0; e < events && running; e++) {
        const uint64_t n = e + 1;   // event number; always passed explicitly (0 = "continue")
        auto& ev = w.pool[n % w.pool.size()];
        if (gbps > 0) {
            const double due = offsetNs + sentBytes * 8.0 / gbps;   // ns since t0
            while (std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() < due)
                ;
        }
        w.times[e].queueNs = nowNs();
        while (running) {
            // random entropy per event (0), which spreads events over the receiver's ports
            auto r = w.seg->addToSendQueue(ev.data(), ev.size(), n, w.dataId, 0, &onSent,
                                           SentArg{&w.times[e], &w.done});
            if (!r.has_error())
                break;
            if (r.error().code() != E2SARErrorc::MemoryError) {
                std::cerr << "dataId " << w.dataId << ": " << r.error().message() << std::endl;
                running = false;
                return;
            }
            std::this_thread::yield();   // queue full
        }
        w.queued++;
        sentBytes += ev.size();
    }
}

}  // namespace

int main(int argc, char** argv) {
    po::options_description od("e2sar_evio_send: send synthetic EVIO v6 time frames with E2SAR");
    auto opts = od.add_options();
    std::string uriStr, senderIP, timingCsv, digests;
    bool withCP = false, validate = true, multiPort = false;
    uint16_t mtu, dataId;
    uint32_t srcId;
    size_t sockets, segmenters, poolSize;
    uint64_t events;
    unsigned framesPerEvent;
    FrameShape shape;
    double rate;
    int sndBuf;
    bool noStagger;

    opts("help,h", "show this help message");
    opts("uri,u", po::value<std::string>(&uriStr), "EJFAT URI (default: $EJFAT_URI)");
    opts("withcp,c", po::bool_switch(&withCP), "use the control plane: add --ip to the LB sender list, send sync packets");
    opts("ip", po::value<std::string>(&senderIP), "sender IP to add to the LB (required with --withcp)");
    opts("novalidate,v", po::bool_switch()->notifier([&](bool b) { validate = !b; }), "don't validate the LB TLS certificate");
    opts("port-per-segmenter", po::bool_switch(&multiPort), "without --withcp: Segmenter i sends to the URI data port + i, one receive thread each");
    opts("no-stagger", po::bool_switch(&noStagger), "start all Segmenters at once instead of i/P of an event interval apart");
    opts("mtu,m", po::value<uint16_t>(&mtu)->default_value(9000), "MTU");
    opts("segmenters,P", po::value<size_t>(&segmenters)->default_value(16), "number of Segmenters, each with its own producing thread");
    opts("sockets,s", po::value<size_t>(&sockets)->default_value(1), "send sockets/threads per Segmenter");
    opts("bufsize,b", po::value<int>(&sndBuf)->default_value(3 * 1024 * 1024), "send socket buffer size");
    opts("rate,r", po::value<double>(&rate)->default_value(20), "total send rate in Gbps over all Segmenters (< 0: unlimited)");
    opts("events,n", po::value<uint64_t>(&events)->default_value(6250), "events per Segmenter");
    opts("frames,f", po::value<unsigned>(&framesPerEvent)->default_value(256), "EVIO time frames per event (one EVIO record, ~95 KB at 256)");
    opts("rocs", po::value<unsigned>(&shape.rocs)->default_value(2), "ROC banks per frame");
    opts("hits", po::value<double>(&shape.hitsPerFrame)->default_value(23), "mean hit words per frame");
    opts("pool", po::value<size_t>(&poolSize)->default_value(64), "distinct events per Segmenter (event e is copy e % pool)");
    opts("dataid", po::value<uint16_t>(&dataId)->default_value(4321), "dataId of the first Segmenter (+i for Segmenter i)");
    opts("srcid", po::value<uint32_t>(&srcId)->default_value(1234), "eventSrcId of the first Segmenter (+i for Segmenter i)");
    opts("timing-csv", po::value<std::string>(&timingCsv), "write per-event queue/sent times here");
    opts("digests", po::value<std::string>(&digests), "write '<dataId> <slot> <sha256>' for every pool event here");

    po::variables_map vm;
    try {
        po::store(po::parse_command_line(argc, argv, od), vm);
        po::notify(vm);
    } catch (const po::error& e) {
        std::cerr << "Unable to parse command line: " << e.what() << std::endl;
        return 1;
    }
    if (vm.count("help")) {
        std::cout << od << std::endl;
        return 0;
    }
    if (uriStr.empty() && std::getenv("EJFAT_URI"))
        uriStr = std::getenv("EJFAT_URI");
    if (uriStr.empty() || (withCP && senderIP.empty()) || segmenters == 0 || poolSize == 0 || framesPerEvent == 0) {
        std::cerr << "Need --uri (or $EJFAT_URI), --ip with --withcp, and non-zero --segmenters/--pool/--frames" << std::endl;
        return 1;
    }
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    auto uriRes = EjfatURI::getFromString(uriStr, EjfatURI::TokenType::instance);
    if (uriRes.has_error()) {
        std::cerr << "Bad URI: " << uriRes.error().message() << std::endl;
        return 1;
    }
    EjfatURI uri = uriRes.value();

    std::unique_ptr<LBManager> lbm;
    if (withCP) {
        lbm = std::make_unique<LBManager>(uri, validate);
        auto r = lbm->addSenders({senderIP});
        if (r.has_error()) {
            std::cerr << "Unable to add sender " << senderIP << ": " << r.error().message() << std::endl;
            return 1;
        }
        std::cout << "Added sender " << senderIP << " to the LB" << std::endl;
    }

    // build the event pools and digests
    std::vector<std::unique_ptr<Worker>> workers;
    std::ofstream dig;
    if (!digests.empty())
        dig.open(digests, std::ios::trunc);
    size_t poolBytes = 0;
    for (size_t i = 0; i < segmenters; i++) {
        auto w = std::make_unique<Worker>();
        w->dataId = static_cast<uint16_t>(dataId + i);
        std::mt19937_64 rng(0xE25A0000ull + w->dataId);
        for (size_t s = 0; s < poolSize; s++) {
            w->pool.push_back(buildRecord(static_cast<uint32_t>(s), s * framesPerEvent, framesPerEvent, shape, rng));
            poolBytes += w->pool.back().size();
            if (dig)
                dig << w->dataId << ' ' << s << ' ' << sha256(w->pool.back()) << '\n';
        }
        w->times.resize(events);
        for (uint64_t e = 0; e < events; e++)
            w->bytes += w->pool[(e + 1) % poolSize].size();
        workers.push_back(std::move(w));
    }
    if (dig)
        dig.close();
    const double meanEvent = static_cast<double>(poolBytes) / (segmenters * poolSize);

    Segmenter::SegmenterFlags sflags;
    sflags.useCP = withCP;
    sflags.mtu = mtu;
    sflags.numSendSockets = sockets;
    sflags.sndSocketBufSize = sndBuf;
    sflags.rateGbps = -1;   // paced in produce()
    for (size_t i = 0; i < segmenters; i++) {
        try {
            workers[i]->seg = std::make_unique<Segmenter>(multiPort ? shiftDataPort(uriStr, i) : uri, workers[i]->dataId,
                                                          static_cast<uint32_t>(srcId + i), sflags);
        } catch (E2SARException& e) {
            std::cerr << "Unable to create Segmenter " << i << ": " << static_cast<std::string>(e) << std::endl;
            return 1;
        } catch (std::exception& e) {
            std::cerr << "Unable to create Segmenter " << i << ": " << e.what() << std::endl;
            return 1;
        }
        auto r = workers[i]->seg->openAndStart();
        if (r.has_error()) {
            std::cerr << "Segmenter " << i << " unable to start: " << r.error().message() << std::endl;
            return 1;
        }
    }

    std::cout << "E2SAR version:        " << get_Version() << std::endl
              << "Control plane:        " << (withCP ? "ON" : "OFF") << (multiPort ? ", data port + i per Segmenter" : "") << std::endl
              << "Segmenters:           " << segmenters << " x " << sockets << " sockets, dataId "
              << dataId << ".." << dataId + segmenters - 1 << std::endl
              << "Event:                EVIO v6 record of " << framesPerEvent << " frame(s), mean "
              << std::fixed << std::setprecision(0) << meanEvent << " bytes ("
              << std::setprecision(1) << meanEvent / framesPerEvent << " bytes/frame)" << std::endl
              << "Events:               " << events << " per Segmenter, " << events * segmenters << " total" << std::endl
              << "Rate:                 " << (rate > 0 ? std::to_string(rate) + " Gbps total" : "unlimited") << std::endl;

    const uint64_t t0 = nowNs();
    std::vector<std::thread> producers;
    const double perSeg = rate > 0 ? rate / segmenters : -1.0;
    const double interval = rate > 0 ? meanEvent * 8.0 / perSeg : 0;   // ns between one Segmenter's events
    for (size_t i = 0; i < workers.size(); i++)
        producers.emplace_back(produce, std::ref(*workers[i]), events, perSeg,
                               noStagger ? 0.0 : interval * i / segmenters);
    for (auto& t : producers)
        t.join();

    // wait for every queued event to be sent
    for (auto& w : workers)
        while (w->done < w->queued)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    uint64_t t1 = t0, first = UINT64_MAX, nev = 0, bytes = 0;
    for (auto& w : workers)
        for (uint64_t e = 0; e < w->queued; e++) {
            first = std::min(first, w->times[e].queueNs);
            t1 = std::max(t1, w->times[e].sentNs);
            bytes += w->pool[(e + 1) % poolSize].size();
            nev++;
        }

    uint64_t frames = 0, errors = 0;
    for (auto& w : workers) {
        w->seg->stopThreads();
        auto st = w->seg->getSendStats();
        frames += st.msgCnt;
        errors += st.errCnt;
        if (st.errCnt)
            std::cerr << "dataId " << w->dataId << ": " << st.errCnt << " send errors, last: "
                      << (st.lastE2SARError != E2SARErrorc::NoError ? make_error_code(st.lastE2SARError).message()
                                                                    : std::string(strerror(st.lastErrno)))
                      << std::endl;
    }

    if (!timingCsv.empty()) {
        std::ofstream csv(timingCsv, std::ios::trunc);
        csv << "evt_num,data_id,bytes,queue_ns,sent_ns\n";
        for (auto& w : workers)
            for (uint64_t e = 0; e < w->queued; e++)
                csv << e + 1 << ',' << w->dataId << ',' << w->pool[(e + 1) % poolSize].size() << ','
                    << w->times[e].queueNs << ',' << w->times[e].sentNs << '\n';
    }

    const double secs = (t1 - first) / 1e9;
    std::cout << std::setprecision(3)
              << "Sent:                 " << nev << " events, " << bytes / 1e9 << " GB, " << frames
              << " packets, " << errors << " errors" << std::endl
              << "Send window:          " << secs << " s (first queued -> last sent)" << std::endl
              << "Sender goodput:       " << (secs > 0 ? bytes * 8 / secs / 1e9 : 0) << " Gbps, "
              << std::setprecision(0) << (secs > 0 ? nev / secs : 0) << " evt/s" << std::endl
              << "first_queue_ns=" << first << " last_sent_ns=" << t1 << std::endl;

    workers.clear();   // Segmenters stop sync before the sender is removed
    if (lbm) {
        auto r = lbm->removeSenders({senderIP});
        if (r.has_error())
            std::cerr << "Unable to remove sender " << senderIP << ": " << r.error().message() << std::endl;
        else
            std::cout << "Removed sender " << senderIP << " from the LB" << std::endl;
    }
    return errors == 0 && running ? 0 : 2;
}
