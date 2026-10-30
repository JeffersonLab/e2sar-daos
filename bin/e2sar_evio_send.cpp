/**
    e2sar_evio_send: read an EVIO v6 file and send its frames with E2SAR Segmenters, for
    end-to-end throughput and latency tests with e2sar_daos_recv.

    The file (--file) is read into memory. Every record in it is one frame (e.g. a built time
    frame of the CODA streaming data in frames_thread0_*.evio). Each E2SAR event is
    --frames-per-event F consecutive frames (default 1): the records' bytes as they are in the
    file, headers included, back to back. The file header and the trailer aren't sent. With
    F > 1 the file's K frames make ceil(K / F) distinct events; the last one has fewer frames
    when F doesn't divide K. CODA streaming frames are ~400 bytes, so F of 50 or so gives the
    ~20 KB events coda-fb is expected to send, well under the Reassembler's ~85k events/s.

    Threading: --segmenters P independent Segmenters, each driven by its own thread with
    dataId = --dataid + i and eventSrcId = --srcid + i, so the receiver keeps their events
    apart (DAOS key <prefix>/<evtNum>/<dataId>). Each Segmenter sends on --sockets threads.
    Each one sends the file's E distinct events in order as events 1..N (--events, default E:
    the file once); event n is distinct event (n - 1) % E, so it starts over when N > E.
    --digests writes "<i> <sha256>" for every distinct event i (with F = 1: every frame), so
    every received event can be checked.

    Paths: with --withcp the events go to the LB, which spreads them over the receiver's ports
    by their random entropy. Without it (node-to-node) everything goes to the data= address in
    the URI; add --port-per-segmenter so Segmenter i sends to data port + i, which spreads
    them over the receiver's threads (--threads >= --segmenters).

    Timing: --timing-csv writes one row per event with system-clock (epoch) times:
    queue_ns when it was handed to the Segmenter and sent_ns when its last packet was sent.
    Joined with e2sar_daos_recv --timing-csv on (evt_num, data_id) this splits the latency
    into sender, network + LB + reassembly, and DAOS commit.

    The defaults (16 Segmenters x 1 socket, 20 Gbps) are the highest setting that was lossless
    in repeated runs on the testbed (with synthetic ~95 KB events), with e2sar_daos_recv
    --threads 32 --deq 16 --inflight 128 (docs/howto/e2e-bench.md).

    --rate is the total for all Segmenters, paced by the producing threads (the Segmenter's
    own rate limit works in whole microseconds per event, too coarse for small events).
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
#include <iterator>
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

// ------------------------------------------------------------------ EVIO file

// Bytes of the file: one frame (a whole record), or several consecutive ones.
struct Frame {
    const uint8_t* data;
    size_t size;
};

// An EVIO v6 file in memory, and its frames (records), in file order.
struct EvioFile {
    std::vector<uint8_t> bytes;
    bool bigEndian = true;
    std::vector<Frame> frames;

    uint32_t word(size_t byteOff) const {
        if (byteOff + 4 > bytes.size())
            throw std::runtime_error("truncated EVIO file");
        const uint8_t* p = &bytes[byteOff];
        return bigEndian ? (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]
                         : (uint32_t(p[3]) << 24) | (uint32_t(p[2]) << 16) | (uint32_t(p[1]) << 8) | p[0];
    }
};

constexpr uint32_t kMagic = 0xc0da0100u;
size_t pad4(size_t n) { return (n + 3) & ~size_t(3); }

// Read the file and find its frames: every record up to the trailer.
void readEvio(const std::string& path, EvioFile& f) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("can't open " + path);
    f.bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (f.bytes.size() < 56)
        throw std::runtime_error(path + ": too short for an EVIO v6 file header");
    if (f.word(28) != kMagic) {
        f.bigEndian = false;
        if (f.word(28) != kMagic)
            throw std::runtime_error(path + ": no EVIO magic number in the file header");
    }
    if ((f.word(20) & 0xff) != 6)
        throw std::runtime_error(path + ": not EVIO version 6");

    // file header: length (words), index array (bytes), user header (bytes, padded)
    size_t pos = f.word(8) * 4ull + f.word(16) + pad4(f.word(24));
    while (pos + 56 <= f.bytes.size()) {
        const size_t recBytes = f.word(pos) * 4ull;
        if (f.word(pos + 28) != kMagic || recBytes < 56 || pos + recBytes > f.bytes.size())
            throw std::runtime_error(path + ": bad record header at byte " + std::to_string(pos));
        if (((f.word(pos + 20) >> 28) & 0xf) == 3)   // trailer
            break;
        f.frames.push_back({&f.bytes[pos], recBytes});
        pos += recBytes;
    }
    if (f.frames.empty())
        throw std::runtime_error(path + ": no records");
}

std::string sha256(const Frame& b) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_Digest(b.data, b.size, md, &len, EVP_sha256(), nullptr);
    std::string hex;
    char x[3];
    for (unsigned i = 0; i < len; i++) {
        std::snprintf(x, sizeof(x), "%02x", md[i]);
        hex += x;
    }
    return hex;
}

// Group the file's frames into events of `perEvent` consecutive frames. The records are back
// to back in the file, so each event is one span of its bytes; the last one may be shorter.
std::vector<Frame> groupFrames(const std::vector<Frame>& frames, size_t perEvent) {
    std::vector<Frame> events;
    for (size_t i = 0; i < frames.size(); i += perEvent) {
        const Frame& last = frames[std::min(i + perEvent, frames.size()) - 1];
        events.push_back({frames[i].data, static_cast<size_t>(last.data + last.size - frames[i].data)});
    }
    return events;
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
    const std::vector<Frame>* frames = nullptr;   // the distinct events, shared by all workers
    std::vector<EventTimes> times;
    std::atomic<uint64_t> queued{0}, done{0};
};

// Pace to `gbps` (total over all workers, so each gets gbps / nWorkers); < 0 = unlimited.
void produce(Worker& w, uint64_t events, double gbps) {
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t sentBytes = 0;
    for (uint64_t e = 0; e < events && running; e++) {
        const uint64_t n = e + 1;   // event number; always passed explicitly (0 = "continue")
        const Frame& ev = (*w.frames)[e % w.frames->size()];
        if (gbps > 0) {
            const double due = sentBytes * 8.0 / gbps;   // ns since t0
            while (std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() < due)
                ;
        }
        w.times[e].queueNs = nowNs();
        while (running) {
            // random entropy per event (0), which spreads events over the receiver's ports
            auto r = w.seg->addToSendQueue(const_cast<uint8_t*>(ev.data), ev.size, n, w.dataId, 0, &onSent,
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
        sentBytes += ev.size;
    }
}

}  // namespace

int main(int argc, char** argv) {
    po::options_description od("e2sar_evio_send: read an EVIO v6 file and send its frames (records) with E2SAR");
    auto opts = od.add_options();
    std::string uriStr, senderIP, timingCsv, digests, evioPath;
    bool withCP = false, validate = true, multiPort = false;
    uint16_t mtu, dataId;
    uint32_t srcId;
    size_t sockets, segmenters, perEvent;
    uint64_t events;
    double rate;
    int sndBuf;

    opts("help,h", "show this help message");
    opts("file,F", po::value<std::string>(&evioPath), "EVIO v6 file whose frames (records) are sent (required)");
    opts("frames-per-event,f", po::value<size_t>(&perEvent)->default_value(1), "consecutive frames of the file per event");
    opts("uri,u", po::value<std::string>(&uriStr), "EJFAT URI (default: $EJFAT_URI)");
    opts("withcp,c", po::bool_switch(&withCP), "use the control plane: add --ip to the LB sender list, send sync packets");
    opts("ip", po::value<std::string>(&senderIP), "sender IP to add to the LB (required with --withcp)");
    opts("novalidate,v", po::bool_switch()->notifier([&](bool b) { validate = !b; }), "don't validate the LB TLS certificate");
    opts("port-per-segmenter", po::bool_switch(&multiPort), "without --withcp: Segmenter i sends to the URI data port + i, one receive thread each");
    opts("mtu,m", po::value<uint16_t>(&mtu)->default_value(9000), "MTU");
    opts("segmenters,P", po::value<size_t>(&segmenters)->default_value(16), "number of Segmenters, each with its own producing thread");
    opts("sockets,s", po::value<size_t>(&sockets)->default_value(1), "send sockets/threads per Segmenter");
    opts("bufsize,b", po::value<int>(&sndBuf)->default_value(3 * 1024 * 1024), "send socket buffer size");
    opts("rate,r", po::value<double>(&rate)->default_value(20), "total send rate in Gbps over all Segmenters (< 0: unlimited)");
    opts("events,n", po::value<uint64_t>(&events)->default_value(0), "events per Segmenter (0: the file once; more starts over)");
    opts("dataid", po::value<uint16_t>(&dataId)->default_value(4321), "dataId of the first Segmenter (+i for Segmenter i)");
    opts("srcid", po::value<uint32_t>(&srcId)->default_value(1234), "eventSrcId of the first Segmenter (+i for Segmenter i)");
    opts("timing-csv", po::value<std::string>(&timingCsv), "write per-event queue/sent times here");
    opts("digests", po::value<std::string>(&digests), "write '<i> <sha256>' for every distinct event i here (event n is (n - 1) % distinct events)");

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
    if (evioPath.empty() || uriStr.empty() || (withCP && senderIP.empty()) || segmenters == 0 || perEvent == 0) {
        std::cerr << "Need --file, --uri (or $EJFAT_URI), --ip with --withcp, and non-zero --segmenters and --frames-per-event"
                  << std::endl;
        return 1;
    }

    // the file's frames (records), F per event
    EvioFile file;
    try {
        readEvio(evioPath, file);
    } catch (std::exception& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
    const std::vector<Frame> frames = groupFrames(file.frames, perEvent);   // the distinct events
    if (events == 0)
        events = frames.size();
    if (!digests.empty()) {
        std::ofstream dig(digests, std::ios::trunc);
        for (size_t r = 0; r < frames.size(); r++)
            dig << r << ' ' << sha256(frames[r]) << '\n';
    }
    size_t frameBytes = 0;
    for (auto& fr : frames)
        frameBytes += fr.size;
    const double meanFrame = static_cast<double>(frameBytes) / file.frames.size();
    const double meanEvent = static_cast<double>(frameBytes) / frames.size();
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

    std::vector<std::unique_ptr<Worker>> workers;
    for (size_t i = 0; i < segmenters; i++) {
        auto w = std::make_unique<Worker>();
        w->dataId = static_cast<uint16_t>(dataId + i);
        w->frames = &frames;
        w->times.resize(events);
        workers.push_back(std::move(w));
    }

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
              << "File:                 " << evioPath << " (" << (file.bigEndian ? "big" : "little") << "-endian)" << std::endl
              << "Frames:               " << file.frames.size() << " (records), mean "
              << std::fixed << std::setprecision(0) << meanFrame << " bytes" << std::endl
              << "Event:                " << (perEvent == 1 ? std::string("one frame") : std::to_string(perEvent) + " frames")
              << ", mean " << meanEvent << " bytes" << std::endl
              << "Distinct events:      " << frames.size() << " (event n is (n - 1) % " << frames.size() << ")" << std::endl
              << "Events:               " << events << " per Segmenter, " << events * segmenters << " total" << std::endl
              << "Rate:                 " << (rate > 0 ? std::to_string(rate) + " Gbps total" : "unlimited") << std::endl;

    const uint64_t t0 = nowNs();
    std::vector<std::thread> producers;
    for (auto& w : workers)
        producers.emplace_back(produce, std::ref(*w), events, rate > 0 ? rate / segmenters : -1.0);
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
            bytes += frames[e % frames.size()].size;
            nev++;
        }

    uint64_t packets = 0, errors = 0;
    for (auto& w : workers) {
        w->seg->stopThreads();
        auto st = w->seg->getSendStats();
        packets += st.msgCnt;
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
                csv << e + 1 << ',' << w->dataId << ',' << frames[e % frames.size()].size << ','
                    << w->times[e].queueNs << ',' << w->times[e].sentNs << '\n';
    }

    const double secs = (t1 - first) / 1e9;
    std::cout << std::setprecision(3)
              << "Sent:                 " << nev << " events, " << bytes / 1e9 << " GB, " << packets
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
