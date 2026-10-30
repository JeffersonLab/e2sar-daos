/**
    e2sar_daos_recv: receive events from an E2SAR sender and store them in DAOS with the
    run layout of docs/concepts/daos-layout.md, using asynchronous writes on DAOS event queues.

    The pool must exist. --cont-name is the run: one run per container. The first
    receiver creates the container and the run record; every receiver claims a writer
    slot (0, 1, 2, …). No host information is stored.

    Each dequeue thread (--deq) pulls events from the Reassembler:
      - events up to --batch-threshold × --chunk are copied into the thread's open batch
        (one chunk), flushed when the next event doesn't fit, after --flush-us, when the
        UTC second changes, or at stop;
      - larger events are written on their own, without a copy, split into writes of at
        most --max-write.
    A batch's bytes go into data[T_open, slot] (a DAOS byte array); when they are written,
    its batch table goes into index[T_commit] (the commit marker). Once a second the
    receiver writes its watermarks to the catalog; at stop, its final statistics.

    Log lines start with the UTC time in seconds, e.g. "1793280042.123 ".

    Synthetic mode (--synthetic N): no network and no Reassembler; N in-memory events of
    --event-size bytes, optionally paced to --rate Gbps, stored like received events.
    Dry run (--dryrun): receive and reassemble, then discard; no DAOS at all.
*/

#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

#include <boost/program_options.hpp>

#include "e2sar.hpp"
#include "e2sar_daos/async_ops.hpp"
#include "e2sar_daos/catalog.hpp"
#include "e2sar_daos/daos_handles.hpp"
#include "e2sar_daos/daos_objects.hpp"
#include "e2sar_daos/latency_histogram.hpp"
#include "e2sar_daos/layout.hpp"

namespace po = boost::program_options;
using namespace e2sar;
using namespace e2sar_daos;
namespace L = e2sar_daos::layout;

namespace {

std::atomic<bool> running{true};
bool dryRun = false;  // --dryrun: receive and discard, no storage

// ------------------------------------------------------------------ time

uint64_t utcSecond() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

// "1793280042.123 ": UTC seconds with milliseconds, at the start of every log line
std::string ts() {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld.%03lld ", static_cast<long long>(ms / 1000),
                  static_cast<long long>(ms % 1000));
    return buf;
}

std::mutex logMtx;
// One whole line, prefixed with the time, so lines from several threads don't mix.
void logLine(std::ostream& os, const std::string& s) {
    std::lock_guard<std::mutex> lk(logMtx);
    os << ts() << s << std::endl;
}
void logOut(const std::string& s) { logLine(std::cout, s); }
void logErr(const std::string& s) { logLine(std::cerr, s); }

// All timing below is nanoseconds since program start (steady clock).
using Clock = std::chrono::steady_clock;
const Clock::time_point tStart = Clock::now();
const int64_t tStartEpochNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();

int64_t sinceStartNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - tStart).count();
}

// ------------------------------------------------------------------ statistics

struct Stats {
    std::atomic<uint64_t> eventsReceived{0};
    std::atomic<uint64_t> bytesReceived{0};
    std::atomic<uint64_t> eventsCommitted{0};
    std::atomic<uint64_t> bytesCommitted{0};
    std::atomic<uint64_t> eventsFailed{0};
    std::atomic<uint64_t> batchesCommitted{0};
    std::atomic<uint64_t> batchesFailed{0};
    std::atomic<uint64_t> opErrors{0};  // failed DAOS operations, including retried ones
    std::atomic<uint64_t> recvErrors{0};
    std::atomic<int> lastDaosErr{0};
} stats;

struct Timing {
    std::atomic<int64_t> firstRecvNs{-1};
    std::atomic<int64_t> lastRecvNs{-1};
    std::atomic<int64_t> lastCommitNs{-1};
    std::mutex mtx;                 // guards the merged histograms and the CSV
    LatencyHistogram eventLatency;  // event handed to us -> its batch's index entry written
    LatencyHistogram dataLatency;   // each daos_array_write
    LatencyHistogram indexLatency;  // each index daos_obj_update
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

// ------------------------------------------------------------------ event sources

// In-memory events for --synthetic: N events of one size, numbered 0..N-1, optionally
// paced. Each event is a fresh new[] buffer (like the Reassembler's).
class SyntheticSource {
public:
    SyntheticSource(uint64_t total, size_t size, double gbps)
        : total_(total), size_(size), template_(size),
          intervalNs_(gbps > 0 ? size * 8.0 / gbps : 0.0) {
        uint64_t x = 0x9E3779B97F4A7C15ull;  // xorshift64: not compressible, not all zeros
        for (auto& b : template_) {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            b = static_cast<uint8_t>(x);
        }
    }

    void start() { t0Ns_ = sinceStartNs(); }
    size_t size() const { return size_; }

    enum Result { Got, NotYet, Done };

    Result next(uint8_t** buf, EventNum_t* evtNum, int waitMs) {
        for (;;) {
            uint64_t idx = next_.load();
            if (idx >= total_)
                return Done;
            if (intervalNs_ > 0) {
                const int64_t wait = t0Ns_ + static_cast<int64_t>(idx * intervalNs_) - sinceStartNs();
                if (wait > 0) {
                    if (waitMs <= 0)
                        return NotYet;
                    std::this_thread::sleep_for(std::chrono::nanoseconds(
                        std::min<int64_t>(wait, waitMs * 1000000ll)));
                    waitMs = 0;
                    continue;
                }
            }
            if (!next_.compare_exchange_weak(idx, idx + 1))
                continue;
            *buf = new uint8_t[size_];
            std::memcpy(*buf, template_.data(), size_);
            std::memcpy(*buf, &idx, std::min(size_, sizeof(idx)));
            *evtNum = idx;
            return Got;
        }
    }

private:
    const uint64_t total_;
    const size_t size_;
    std::vector<uint8_t> template_;
    const double intervalNs_;
    int64_t t0Ns_ = 0;
    std::atomic<uint64_t> next_{0};
};

// The Reassembler or --synthetic. No events are handed out until main sets `go`, after
// every dequeue thread has called ready(): creating a DAOS event queue while another
// thread's operations are in flight can crash in libfabric.
struct EventSource {
    Reassembler* reas = nullptr;
    SyntheticSource* synth = nullptr;
    std::atomic<size_t> readyThreads{0};
    std::atomic<bool> go{false};

    enum Result { Got, NotYet, Error, Done };

    void ready() { readyThreads++; }

    Result next(uint8_t** buf, size_t* size, EventNum_t* evtNum, uint16_t* dataId, int waitMs) {
        if (!go) {
            if (waitMs > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return NotYet;
        }
        if (synth) {
            *size = synth->size();
            *dataId = 0;
            switch (synth->next(buf, evtNum, waitMs)) {
                case SyntheticSource::Got: return Got;
                case SyntheticSource::NotYet: return NotYet;
                default: return Done;
            }
        }
        auto res = waitMs > 0 ? reas->recvEvent(buf, size, evtNum, dataId, waitMs)
                              : reas->getEvent(buf, size, evtNum, dataId);
        if (res.has_error())
            return Error;
        return res.value() == -1 ? NotYet : Got;
    }
};

// ------------------------------------------------------------------ watermarks

// What one dequeue thread still has pending, by second, for the watermarks:
//   data:  batches opened in second T (written into data[T, slot]) and not yet committed
//   index: index writes started in second T and not yet completed
// The second is read from the clock under the lock, so the watermark thread, which reads
// the clock first and then each thread's oldest entry, never misses a write that starts
// in an earlier second.
class Pending {
public:
    uint64_t openData() { return add(data_); }
    uint64_t startIndex() { return add(index_); }
    void addIndex(uint64_t t) {
        std::lock_guard<std::mutex> lk(m_);
        index_[t]++;
    }
    void endData(uint64_t t) { remove(data_, t); }
    void endIndex(uint64_t t) { remove(index_, t); }

    /** Lower *index / *data to this thread's oldest pending second. */
    void oldest(uint64_t* index, uint64_t* data) {
        std::lock_guard<std::mutex> lk(m_);
        if (!index_.empty())
            *index = std::min(*index, index_.begin()->first);
        if (!data_.empty())
            *data = std::min(*data, data_.begin()->first);
    }

private:
    uint64_t add(std::map<uint64_t, uint32_t>& m) {
        std::lock_guard<std::mutex> lk(m_);
        const uint64_t t = utcSecond();
        m[t]++;
        return t;
    }
    void remove(std::map<uint64_t, uint32_t>& m, uint64_t t) {
        std::lock_guard<std::mutex> lk(m_);
        auto it = m.find(t);
        if (it != m.end() && --it->second == 0)
            m.erase(it);
    }

    std::mutex m_;
    std::map<uint64_t, uint32_t> index_, data_;
};

// ------------------------------------------------------------------ writer

struct WriterConfig {
    uint32_t slot = 0;
    size_t chunk = 0;      // bytes: array chunk size = batch size
    size_t threshold = 0;  // events up to this size are batched
    int64_t flushNs = 0;
    size_t batchBuffers = 0;
    size_t maxWrite = 0;   // multiple of chunk
    size_t inflight = 0;
    std::string oclass, indexOclass;
    bool verbose = false;
};

// State shared by the dequeue threads of this receiver.
struct Shared {
    const DaosContainer* cont = nullptr;
    DaosIntObject* seconds = nullptr;
    std::atomic<uint64_t> nextChunk{0};  // never resets
    std::atomic<uint64_t> nextBatch{0};  // batch_seq of this slot
};

// Open object handles of one thread, by second, closed once unused and old.
template <class H>
class HandleCache {
public:
    template <class Make>
    H* acquire(uint64_t t, Make make) {
        auto& e = m_[t];
        if (!e.h)
            e.h = make();
        e.refs++;
        return e.h.get();
    }
    void release(uint64_t t) {
        auto it = m_.find(t);
        if (it != m_.end() && it->second.refs > 0)
            it->second.refs--;
    }
    void prune(uint64_t now) {
        for (auto it = m_.begin(); it != m_.end();)
            it = (it->second.refs == 0 && it->first + 2 < now) ? m_.erase(it) : std::next(it);
    }

private:
    struct Entry {
        std::unique_ptr<H> h;
        size_t refs = 0;
    };
    std::map<uint64_t, Entry> m_;
};

struct Batch {
    uint8_t* buf = nullptr;
    bool ownsEvent = false;  // a large event: buf is the Reassembler's, delete[] it
    int pool = -1;           // otherwise the index of the thread's batch buffer
    size_t len = 0;
    uint64_t tOpen = 0;
    int64_t openNs = 0;
    std::vector<L::BatchEntry> entries;
    std::vector<int64_t> recvNs;

    uint64_t firstChunk = 0, nChunks = 0, seq = 0;
    DaosArray* arr = nullptr;
    std::vector<ArrayWrite> writes;  // sized once: DAOS holds pointers into them
    std::vector<uint8_t> tries;
    size_t piecesLeft = 0;

    uint64_t tCommit = 0;
    DaosIntObject* idxObj = nullptr;
    std::vector<uint8_t> table;
    IntKeyUpdate idx;
    uint8_t idxTries = 0;

    int err = 0;
};

struct SecondsMark {
    uint64_t t = 0;
    uint8_t one = 1;
    IntKeyUpdate u;
};

class DequeueThread {
public:
    DequeueThread(const WriterConfig& cfg, Shared& shared, Pending& pending, int tid)
        : cfg_(cfg), sh_(shared), pend_(pending), tid_(tid), ops_(cfg.inflight) {
        pool_.resize(cfg.batchBuffers);
        for (size_t i = 0; i < pool_.size(); i++) {
            pool_[i].reset(new uint8_t[cfg.chunk]);
            freeBufs_.push_back(static_cast<int>(i));
        }
    }

    void run(EventSource* src) {
        src->ready();
        uint64_t lastPrune = utcSecond();
        while (running) {
            pump();
            maybeFlush();
            const uint64_t now = utcSecond();
            if (now != lastPrune) {
                arrays_.prune(now);
                indexes_.prune(now);
                lastPrune = now;
            }

            uint8_t* buf = nullptr;
            size_t size = 0;
            EventNum_t evtNum = 0;
            uint16_t dataId = 0;
            // With operations in flight, poll the source and wait on the event queue
            // instead; with only an open batch, wake up often enough to flush it in time.
            const int waitMs = ops_.inflight() > 0 ? 0 : (open_ ? 1 : 100);
            const auto res = src->next(&buf, &size, &evtNum, &dataId, waitMs);
            if (res == EventSource::Done)
                break;
            if (res == EventSource::Error) {
                stats.recvErrors++;
                continue;
            }
            if (res == EventSource::NotYet) {
                if (ops_.inflight() > 0)
                    ops_.progress(1000);
                continue;
            }

            const int64_t recvNs = sinceStartNs();
            updateMin(timing.firstRecvNs, recvNs);
            updateMax(timing.lastRecvNs, recvNs);
            stats.eventsReceived++;
            stats.bytesReceived += size;
            if (size <= cfg_.threshold)
                addSmall(buf, size, evtNum, dataId, recvNs);
            else
                addLarge(buf, size, evtNum, dataId, recvNs);
        }

        // Stop: flush the open batch and finish everything in flight.
        if (open_)
            flush(std::move(open_));
        while (!inflight_.empty()) {
            pump();
            if (!inflight_.empty() && ops_.inflight() > 0)
                ops_.progress(DAOS_EQ_WAIT);
        }

        std::lock_guard<std::mutex> lk(timing.mtx);
        timing.eventLatency.merge(eventLatency_);
        timing.dataLatency.merge(dataLatency_);
        timing.indexLatency.merge(indexLatency_);
    }

private:
    // ---- events

    void addSmall(uint8_t* buf, size_t size, EventNum_t evtNum, uint16_t dataId, int64_t recvNs) {
        if (open_ && (open_->len + size > cfg_.chunk || open_->tOpen != utcSecond()))
            flush(std::move(open_));
        if (!open_)
            openBatch();
        std::memcpy(open_->buf + open_->len, buf, size);
        delete[] buf;  // E2SAR allocates events with new[]
        open_->entries.push_back({evtNum, open_->len, size, dataId, 0});
        open_->recvNs.push_back(recvNs);
        open_->len += size;
    }

    void addLarge(uint8_t* buf, size_t size, EventNum_t evtNum, uint16_t dataId, int64_t recvNs) {
        auto b = std::make_unique<Batch>();
        b->buf = buf;
        b->ownsEvent = true;
        b->tOpen = pend_.openData();
        b->openNs = recvNs;
        b->entries.push_back({evtNum, 0, size, dataId, 0});
        b->recvNs.push_back(recvNs);
        b->len = size;
        flush(std::move(b));
    }

    void openBatch() {
        while (freeBufs_.empty()) {  // every buffer is in a batch in flight
            pump();
            if (freeBufs_.empty() && ops_.inflight() > 0)
                ops_.progress(DAOS_EQ_WAIT);
        }
        open_ = std::make_unique<Batch>();
        open_->pool = freeBufs_.back();
        freeBufs_.pop_back();
        open_->buf = pool_[open_->pool].get();
        open_->tOpen = pend_.openData();
        open_->openNs = sinceStartNs();
    }

    void maybeFlush() {
        if (open_ && (sinceStartNs() - open_->openNs >= cfg_.flushNs || open_->tOpen != utcSecond()))
            flush(std::move(open_));
    }

    // ---- data writes

    void flush(std::unique_ptr<Batch> b) {
        Batch* p = b.get();
        inflight_[p] = std::move(b);
        p->seq = sh_.nextBatch++;
        p->nChunks = L::chunksFor(p->len, cfg_.chunk);
        p->firstChunk = p->nChunks ? sh_.nextChunk.fetch_add(p->nChunks) : 0;
        const size_t pieces = (p->len + cfg_.maxWrite - 1) / cfg_.maxWrite;
        p->writes.resize(pieces);
        p->tries.assign(pieces, 0);
        p->piecesLeft = pieces;
        if (pieces == 0) {  // only empty events: nothing to write
            readyIndex_.push_back(p);
            return;
        }
        const uint64_t t = p->tOpen;
        p->arr = arrays_.acquire(t, [&] {
            return std::make_unique<DaosArray>(*sh_.cont, L::oidLo(L::Kind::Data, cfg_.slot, t), cfg_.oclass, cfg_.chunk);
        });
        for (size_t i = 0; i < pieces; i++)
            submitPiece(p, i);
    }

    void submitPiece(Batch* b, size_t i) {
        b->tries[i]++;
        const size_t off = i * cfg_.maxWrite;
        const size_t size = std::min(cfg_.maxWrite, b->len - off);
        ArrayWrite& w = b->writes[i];
        w.set(b->firstChunk * cfg_.chunk + off, b->buf + off, size);
        const daos_handle_t oh = b->arr->handle();
        ops_.submit([&w, oh](daos_event_t* ev) { return w.launch(oh, ev); },
                    [this, b, i](int rc, uint64_t ns) {
                        if (rc == 0) {
                            dataLatency_.record(ns);
                        } else {
                            opError(rc);
                            if (b->tries[i] < 2) {  // retry once
                                retryData_.push_back({b, i});
                                return;
                            }
                            if (b->err == 0)
                                b->err = rc;
                        }
                        if (--b->piecesLeft == 0)
                            (b->err ? finished_ : readyIndex_).push_back(b);
                    });
    }

    // ---- index writes (the commit marker)

    void commitIndex(Batch* b) {
        const uint64_t t = pend_.startIndex();
        b->tCommit = t;
        b->idxObj = indexes_.acquire(t, [&] {
            return std::make_unique<DaosIntObject>(*sh_.cont, L::oidLo(L::Kind::Index, 0, t), cfg_.indexOclass);
        });
        L::BatchTable tab;
        tab.slot = cfg_.slot;
        tab.tData = b->tOpen;
        tab.offset = b->firstChunk * cfg_.chunk;
        tab.length = b->len;
        tab.entries = b->entries;
        L::encodeTable(tab, b->table);
        b->idx.set(L::indexDkey(cfg_.slot, b->seq), L::kAkey, b->table.data(), b->table.size());
        if (t != lastMarked_)
            markSecond(t);
        submitIndex(b);
    }

    void submitIndex(Batch* b) {
        b->idxTries++;
        const daos_handle_t oh = b->idxObj->handle();
        ops_.submit([b, oh](daos_event_t* ev) { return b->idx.launch(oh, ev); },
                    [this, b](int rc, uint64_t ns) {
                        if (rc == 0) {
                            indexLatency_.record(ns);
                        } else {
                            opError(rc);
                            if (b->idxTries < 2) {
                                retryIndex_.push_back(b);
                                return;
                            }
                            b->err = rc;
                        }
                        pend_.endIndex(b->tCommit);
                        finished_.push_back(b);
                    });
    }

    // seconds[T] = 1, the first time this thread commits in second T. Writing it twice
    // (several threads or receivers) writes the same value.
    void markSecond(uint64_t t) {
        lastMarked_ = t;
        pend_.addIndex(t);  // part of the index watermark
        auto m = std::make_unique<SecondsMark>();
        SecondsMark* p = m.get();
        p->t = t;
        p->u.set(t, L::kAkey, &p->one, 1);
        marks_[p] = std::move(m);
        const daos_handle_t oh = sh_.seconds->handle();
        ops_.submit([p, oh](daos_event_t* ev) { return p->u.launch(oh, ev); },
                    [this, p](int rc, uint64_t) {
                        if (rc != 0) {
                            opError(rc);
                            logErr("[t" + std::to_string(tid_) + "] seconds[" + std::to_string(p->t) +
                                   "] not written: " + d_errstr(rc));
                        }
                        pend_.endIndex(p->t);
                        doneMarks_.push_back(p);
                    });
    }

    // ---- completion

    void finish(Batch* b) {
        if (b->err == 0) {
            const int64_t now = sinceStartNs();
            updateMax(timing.lastCommitNs, now);
            stats.batchesCommitted++;
            stats.eventsCommitted += b->entries.size();
            stats.bytesCommitted += b->len;
            for (size_t i = 0; i < b->entries.size(); i++) {
                eventLatency_.record(static_cast<uint64_t>(now - b->recvNs[i]));
                if (timing.csv.is_open()) {
                    const auto& e = b->entries[i];
                    std::lock_guard<std::mutex> lk(timing.mtx);
                    timing.csv << e.evtNum << ',' << e.dataId << ',' << e.size << ',' << b->nChunks << ','
                               << tid_ << ',' << b->recvNs[i] / 1e9 << ',' << now / 1e9 << ','
                               << (now - b->recvNs[i]) / 1e6 << '\n';
                }
            }
            if (cfg_.verbose)
                logOut("[t" + std::to_string(tid_) + "] committed batch " + std::to_string(b->seq) + ": " +
                       std::to_string(b->entries.size()) + " events, " + std::to_string(b->len) +
                       " bytes, data[" + std::to_string(b->tOpen) + "] index[" + std::to_string(b->tCommit) + "]");
        } else {
            stats.batchesFailed++;
            stats.eventsFailed += b->entries.size();
            logErr("[t" + std::to_string(tid_) + "] batch " + std::to_string(b->seq) + " (" +
                   std::to_string(b->entries.size()) + " events, first " + std::to_string(b->entries.front().evtNum) +
                   ") not committed: " + d_errstr(b->err));
        }
        pend_.endData(b->tOpen);
        if (b->arr)
            arrays_.release(b->tOpen);
        if (b->idxObj)
            indexes_.release(b->tCommit);
        if (b->ownsEvent)
            delete[] b->buf;
        else
            freeBufs_.push_back(b->pool);
        inflight_.erase(b);
    }

    void opError(int rc) {
        stats.opErrors++;
        stats.lastDaosErr = rc;
    }

    // Reap completions, then do what they asked for (callbacks must not submit).
    void pump() {
        ops_.progress(0);
        while (!retryData_.empty() || !retryIndex_.empty() || !readyIndex_.empty() || !finished_.empty() ||
               !doneMarks_.empty()) {
            auto rd = std::move(retryData_);
            retryData_.clear();
            for (auto& [b, i] : rd)
                submitPiece(b, i);
            auto ri = std::move(retryIndex_);
            retryIndex_.clear();
            for (Batch* b : ri)
                submitIndex(b);
            auto ready = std::move(readyIndex_);
            readyIndex_.clear();
            for (Batch* b : ready)
                commitIndex(b);
            auto fin = std::move(finished_);
            finished_.clear();
            for (Batch* b : fin)
                finish(b);
            auto dm = std::move(doneMarks_);
            doneMarks_.clear();
            for (SecondsMark* m : dm)
                marks_.erase(m);
        }
    }

    const WriterConfig& cfg_;
    Shared& sh_;
    Pending& pend_;
    const int tid_;
    // Batches and seconds marks in flight; declared before ops_ so they outlive its
    // destructor, which may still run callbacks.
    std::unordered_map<Batch*, std::unique_ptr<Batch>> inflight_;
    std::unordered_map<SecondsMark*, std::unique_ptr<SecondsMark>> marks_;
    std::vector<std::unique_ptr<uint8_t[]>> pool_;
    std::vector<int> freeBufs_;
    HandleCache<DaosArray> arrays_;
    HandleCache<DaosIntObject> indexes_;
    std::vector<std::pair<Batch*, size_t>> retryData_;
    std::vector<Batch*> retryIndex_, readyIndex_, finished_;
    std::vector<SecondsMark*> doneMarks_;
    uint64_t lastMarked_ = 0;
    LatencyHistogram eventLatency_, dataLatency_, indexLatency_;
    AsyncOps ops_;
    std::unique_ptr<Batch> open_;
};

void dequeueLoop(EventSource* src, const WriterConfig* cfg, Shared* shared, Pending* pending, int tid) {
    try {
        DequeueThread t(*cfg, *shared, *pending, tid);
        t.run(src);
    } catch (const std::exception& e) {
        logErr("[t" + std::to_string(tid) + "] fatal: " + e.what());
        src->ready();  // don't keep main waiting for this thread
        running = false;
    }
}

// --dryrun: receive -> count -> discard. No storage.
void dryrunLoop(EventSource* src, bool verbose, int tid) {
    src->ready();
    while (running) {
        uint8_t* raw = nullptr;
        size_t size = 0;
        EventNum_t evtNum = 0;
        uint16_t dataId = 0;
        const auto res = src->next(&raw, &size, &evtNum, &dataId, 100);
        if (res == EventSource::Done)
            break;
        if (res == EventSource::Error) {
            stats.recvErrors++;
            continue;
        }
        if (res == EventSource::NotYet)
            continue;
        delete[] raw;

        const int64_t recvNs = sinceStartNs();
        updateMin(timing.firstRecvNs, recvNs);
        updateMax(timing.lastRecvNs, recvNs);
        stats.eventsReceived++;
        stats.bytesReceived += size;
        if (timing.csv.is_open()) {
            std::lock_guard<std::mutex> lk(timing.mtx);
            timing.csv << evtNum << ',' << dataId << ',' << size << ",0," << tid << ',' << recvNs / 1e9 << ",,\n";
        }
        if (verbose)
            logOut("[t" + std::to_string(tid) + "] received " + std::to_string(evtNum) + "/" +
                   std::to_string(dataId) + " (" + std::to_string(size) + " bytes, discarded)");
    }
}

// Once a second: wm/<slot> = the oldest second each thread still has pending (or now).
L::Watermark currentWatermark(const std::vector<std::unique_ptr<Pending>>& pending) {
    const uint64_t now = utcSecond();  // read the clock first: see Pending
    L::Watermark wm{now, now};
    for (const auto& p : pending)
        p->oldest(&wm.index, &wm.data);
    return wm;
}

void watermarkLoop(Catalog* catalog, uint32_t slot, const std::vector<std::unique_ptr<Pending>>* pending) {
    while (running) {
        for (int i = 0; i < 10 && running; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!running)
            break;
        try {
            catalog->putWatermark(slot, currentWatermark(*pending));
        } catch (const DaosError& e) {
            logErr(std::string("unable to write the watermark: ") + e.what());
        }
    }
}

// ------------------------------------------------------------------ reporting

// Zero suppression (unless logIdle): an interval with nothing new prints no line; the
// first such interval prints one "idle" line, so the log shows when the traffic stopped.
void reportLoop(Reassembler* reas, unsigned intervalSec, bool logIdle) {
    uint64_t lastBytes = 0, lastEvents = 0, lastRcvdBytes = 0, lastRcvd = 0, lastFailed = 0, lastLoss = 0,
             lastBatches = 0;
    bool idle = false;
    auto lastT = Clock::now();
    while (running) {
        for (unsigned i = 0; i < intervalSec * 10 && running; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!running)
            break;

        const auto now = Clock::now();
        const double dt = std::chrono::duration<double>(now - lastT).count();
        const uint64_t bytes = stats.bytesCommitted, events = stats.eventsCommitted, batches = stats.batchesCommitted;
        const uint64_t rcvdBytes = stats.bytesReceived, rcvd = stats.eventsReceived, failed = stats.eventsFailed;
        uint64_t reasLoss = 0, enqLoss = 0;
        if (reas) {
            const auto rs = reas->getStats();
            reasLoss = rs.reassemblyLoss;
            enqLoss = rs.enqueueLoss;
        }
        const bool changed = rcvd != lastRcvd || events != lastEvents || failed != lastFailed ||
                             reasLoss + enqLoss != lastLoss;
        lastT = now;

        if (!changed && !logIdle) {
            if (!idle) {
                logOut("idle: no new events; progress lines resume when events arrive");
                idle = true;
            }
            continue;
        }
        idle = false;

        std::ostringstream o;
        o << std::fixed << std::setprecision(2) << "in " << rcvd << " evts, "
          << (rcvdBytes - lastRcvdBytes) * 8.0 / dt / 1e9 << " Gbps";
        if (dryRun) {
            o << ", " << (rcvd - lastRcvd) / dt << " evt/s | dry run, discarded";
        } else {
            const uint64_t nb = batches - lastBatches;
            o << " | committed " << events << " evts, " << bytes / 1048576.0 << " MiB, "
              << (bytes - lastBytes) * 8.0 / dt / 1e9 << " Gbps, " << (events - lastEvents) / dt << " evt/s, "
              << nb / dt << " batch/s";
            if (nb > 0)
                o << " (" << static_cast<double>(events - lastEvents) / nb << " evt/batch)";
            o << " | failed " << failed;
        }
        if (reas)
            o << " | reassembly loss " << reasLoss << ", enqueue loss " << enqLoss;
        logOut(o.str());

        lastBytes = bytes;
        lastRcvdBytes = rcvdBytes;
        lastEvents = events;
        lastBatches = batches;
        lastRcvd = rcvd;
        lastFailed = failed;
        lastLoss = reasLoss + enqLoss;
    }
}

result<int> prepareToReceive(Reassembler& r) {
    auto hostname = NetUtil::getHostName();
    if (hostname.has_error())
        return E2SARErrorInfo{hostname.error().code(), hostname.error().message()};
    auto reg = r.registerWorker(hostname.value());  // no-op without control plane
    if (reg.has_error())
        return E2SARErrorInfo{E2SARErrorc::RPCError, "Unable to register worker: " + reg.error().message()};
    return r.openAndStart();  // after registerWorker: the state thread needs the session token
}

void printFinalStats(Reassembler* r) {
    if (r) {
        const auto rs = r->getStats();
        logOut("Reassembler:");
        logOut("\tEvents reassembled:        " + std::to_string(rs.eventSuccess));
        logOut("\tLost in reassembly:        " + std::to_string(rs.reassemblyLoss));
        logOut("\tLost in enqueue:           " + std::to_string(rs.enqueueLoss));
        logOut("\tData errors:               " + std::to_string(rs.dataErrCnt));
        logOut("\tgRPC errors:               " + std::to_string(rs.grpcErrCnt));
    }
    if (dryRun) {
        logOut("Dry run (nothing stored):");
        logOut("\tEvents received:           " + std::to_string(stats.eventsReceived));
        logOut("\tBytes received:            " + std::to_string(stats.bytesReceived));
        logOut("\trecvEvent errors:          " + std::to_string(stats.recvErrors));
        return;
    }
    logOut("DAOS:");
    logOut("\tEvents received:           " + std::to_string(stats.eventsReceived));
    logOut("\tEvents committed:          " + std::to_string(stats.eventsCommitted));
    logOut("\tBytes committed:           " + std::to_string(stats.bytesCommitted));
    logOut("\tBatches committed:         " + std::to_string(stats.batchesCommitted));
    logOut("\tEvents failed:             " + std::to_string(stats.eventsFailed));
    logOut("\tFailed DAOS operations:    " + std::to_string(stats.opErrors) + " (including retried ones)");
    logOut("\trecvEvent errors:          " + std::to_string(stats.recvErrors));
    if (stats.lastDaosErr != 0)
        logOut(std::string("\tLast DAOS error:           ") + d_errstr(stats.lastDaosErr));
}

std::string fmtRate(uint64_t bytes, uint64_t events, int64_t windowNs) {
    if (windowNs <= 0)
        return "n/a (needs at least 2 events)";
    const double sec = windowNs / 1e9;
    std::ostringstream o;
    o << std::fixed << std::setprecision(3) << bytes * 8.0 / windowNs << " Gbps, " << std::setprecision(1)
      << bytes / 1048576.0 / sec << " MiB/s, " << events / sec << " evt/s";
    return o.str();
}

std::string fmtLatency(const LatencyHistogram& h) {
    if (h.count() == 0)
        return "n/a";
    auto ms = [](double ns) { return ns / 1e6; };
    std::ostringstream o;
    o << std::fixed << std::setprecision(3) << "min " << ms(h.minNs()) << " | mean " << ms(h.meanNs()) << " | p50 "
      << ms(h.percentileNs(50)) << " | p90 " << ms(h.percentileNs(90)) << " | p99 " << ms(h.percentileNs(99))
      << " | max " << ms(h.maxNs()) << " ms  (" << h.count() << ")";
    return o.str();
}

void printTiming() {
    const int64_t first = timing.firstRecvNs, lastRecv = timing.lastRecvNs, lastCommit = timing.lastCommitNs;
    logOut("Timing:");
    if (first < 0) {
        logOut("\tno events received");
        return;
    }
    std::ostringstream o;
    o << std::fixed << std::setprecision(3);
    o << "\tFirst event received:      " << first / 1e9 << " s after start";
    logOut(o.str());
    o.str("");
    o << "\tReceive window:            " << (lastRecv - first) / 1e9 << " s (first -> last event received)";
    logOut(o.str());
    if (!dryRun) {
        o.str("");
        o << "\tCommit window:             " << (lastCommit >= 0 ? (lastCommit - first) / 1e9 : 0.0)
          << " s (first event received -> last event committed)";
        logOut(o.str());
    }
    logOut("\tIngest throughput:         " + fmtRate(stats.bytesReceived, stats.eventsReceived, lastRecv - first));
    if (dryRun)
        return;
    logOut("\tDAOS commit throughput:    " +
           fmtRate(stats.bytesCommitted, stats.eventsCommitted, lastCommit >= 0 ? lastCommit - first : 0));
    logOut("\tEvent latency (recv->commit): " + fmtLatency(timing.eventLatency));
    logOut("\tdaos_array_write latency:  " + fmtLatency(timing.dataLatency));
    logOut("\tIndex write latency:       " + fmtLatency(timing.indexLatency));
}

// This receiver's final statistics, for done/<slot>.
Record finalStatsRecord(Reassembler* r) {
    Record rec{{"received", std::to_string(stats.eventsReceived.load())},
               {"committed", std::to_string(stats.eventsCommitted.load())},
               {"bytes", std::to_string(stats.bytesCommitted.load())},
               {"batches", std::to_string(stats.batchesCommitted.load())},
               {"failed", std::to_string(stats.eventsFailed.load())},
               {"start", std::to_string(tStartEpochNs / 1000000000)},
               {"stop", std::to_string(utcSecond())}};
    uint64_t lost = 0;
    if (r) {
        const auto rs = r->getStats();
        rec["reassembly_loss"] = std::to_string(rs.reassemblyLoss);
        rec["enqueue_loss"] = std::to_string(rs.enqueueLoss);
        lost = rs.reassemblyLoss + rs.enqueueLoss;
    }
    rec["lost"] = std::to_string(lost);
    return rec;
}

// "95K", "1.5M", "64M": bytes with an optional binary K/M/G suffix; 0 if invalid.
size_t parseSize(const std::string& s) {
    size_t pos = 0;
    double v = 0;
    try {
        v = std::stod(s, &pos);
    } catch (const std::exception&) {
        return 0;
    }
    const std::string unit = s.substr(pos);
    double mult = 1;
    if (unit == "K" || unit == "k")
        mult = 1024.0;
    else if (unit == "M" || unit == "m")
        mult = 1024.0 * 1024;
    else if (unit == "G" || unit == "g")
        mult = 1024.0 * 1024 * 1024;
    else if (!unit.empty())
        return 0;
    return v > 0 ? static_cast<size_t>(v * mult) : 0;
}

}  // namespace

int main(int argc, char** argv) {
    po::options_description od("Command-line options");

    std::string uriStr, recvIP, pool, contName, oclass, indexOclass, timingCsv, eventSizeStr, maxWriteStr;
    uint16_t recvStartPort;
    size_t numRecvThreads, deqThreads, inflight, chunkKiB, batchBuffers;
    std::vector<int> coreList;
    int sockBufSize, eventTimeoutMS, numaNode;
    uint64_t synthEvents;
    int64_t flushUs;
    unsigned reportSec;
    double synthGbps, batchThreshold;

    auto opts = od.add_options();
    opts("help,h", "show this help message");
    // E2SAR receive side, same meaning as in e2sar_ft -r
    opts("uri,u", po::value<std::string>(&uriStr), "EJFAT_URI (defaults to the EJFAT_URI environment variable)");
    opts("withcp,c", po::bool_switch()->default_value(false), "enable control plane interactions");
    opts("ip", po::value<std::string>(&recvIP)->default_value(""), "IP address to listen on (conflicts with --autoip)");
    opts("autoip", po::bool_switch()->default_value(false), "auto-detect the listening IP from the URI data address");
    opts("port", po::value<uint16_t>(&recvStartPort)->default_value(10000), "starting UDP receive port");
    opts("threads", po::value<size_t>(&numRecvThreads)->default_value(1), "number of E2SAR receive (reassembly) threads");
    opts("cores", po::value<std::vector<int>>(&coreList)->multitoken(), "cores to bind E2SAR receive threads to (conflicts with --threads)");
    opts("bufsize,b", po::value<int>(&sockBufSize)->default_value(3 * 1024 * 1024), "receive socket buffer size");
    opts("timeout", po::value<int>(&eventTimeoutMS)->default_value(500), "event reassembly timeout in ms");
    opts("numa", po::value<int>(&numaNode)->default_value(-1), "bind memory allocation to this NUMA node (if >= 0)");
    opts("ipv4,4", "force IPv4 control plane address if the URI has a hostname (disables cert validation)");
    opts("ipv6,6", "force IPv6 control plane address if the URI has a hostname (disables cert validation)");
    opts("novalidate,v", "don't validate the control plane server certificate");
    // DAOS side
    opts("pool", po::value<std::string>(&pool)->default_value("sc26"), "DAOS pool label (must exist)");
    opts("cont-name", po::value<std::string>(&contName), "the run's container label (created if missing); required unless --dryrun. Every receiver of a run uses the same");
    opts("chunk", po::value<size_t>(&chunkKiB)->default_value(1024), "array chunk size = batch size, in KiB");
    opts("oclass", po::value<std::string>(&oclass)->default_value("SX"), "data array object class");
    opts("index-oclass", po::value<std::string>(&indexOclass)->default_value("S8"), "seconds and index object class");
    opts("batch-threshold", po::value<double>(&batchThreshold)->default_value(0.9), "events up to this fraction of a chunk are batched; larger ones are written on their own");
    opts("flush-us", po::value<int64_t>(&flushUs)->default_value(5000), "flush a partly filled batch after this many microseconds");
    opts("batch-buffers", po::value<size_t>(&batchBuffers)->default_value(8), "batch buffers (one chunk each) per dequeue thread");
    opts("max-write", po::value<std::string>(&maxWriteStr)->default_value("64M"), "largest single array write (K/M/G suffix); larger events are split; rounded down to whole chunks");
    opts("inflight", po::value<size_t>(&inflight)->default_value(128), "max DAOS operations in flight per dequeue thread");
    opts("deq", po::value<size_t>(&deqThreads)->default_value(8), "number of dequeue threads, each with its own DAOS event queue");
    opts("dryrun", po::bool_switch()->default_value(false), "receive and reassemble events, then discard them: no DAOS at all. Not with --synthetic");
    // synthetic mode (no network)
    opts("synthetic", po::value<uint64_t>(&synthEvents)->default_value(0), "store N in-memory events instead of receiving (0 = off); the E2SAR options are ignored. Not with --dryrun");
    opts("event-size", po::value<std::string>(&eventSizeStr)->default_value("95K"), "with --synthetic: bytes per event, with an optional K/M/G suffix");
    opts("rate", po::value<double>(&synthGbps)->default_value(0), "with --synthetic: pace events to this many Gbps (0 = as fast as DAOS takes them)");
    opts("report", po::value<unsigned>(&reportSec)->default_value(1), "progress report interval in seconds (0 = off)");
    opts("log-idle", po::bool_switch()->default_value(false), "print a progress line every interval, even with no new events");
    opts("verbose", po::bool_switch()->default_value(false), "print every committed batch");
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
                  << "Example (pairs with `e2sar_perf -s -u <uri> --ip <sender ip> -l 1048576 -n 100 --rate 5`):\n"
                  << "  e2sar_daos_recv -u 'ejfat://...@host:port/lb/1?data=<recv ip>' --ip <recv ip> \\\n"
                  << "      --pool sc26 --cont-name e2sar_1793280000\n"
                  << "Example without DAOS (receive and discard, to check the network side):\n"
                  << "  e2sar_daos_recv -u 'ejfat://...' --ip <recv ip> --dryrun\n"
                  << "Example without a sender (100,000 x 20 KB events, as fast as DAOS takes them):\n"
                  << "  e2sar_daos_recv --synthetic 100000 --event-size 20K --pool sc26 --cont-name synth_1"
                  << std::endl;
        return 0;
    }

    dryRun = vm["dryrun"].as<bool>();
    const bool synthMode = synthEvents > 0;
    if (synthMode && dryRun) {
        std::cerr << "--synthetic and --dryrun are mutually exclusive" << std::endl;
        return 1;
    }
    const size_t synthSize = parseSize(eventSizeStr);
    if (synthMode && (synthSize == 0 || synthGbps < 0)) {
        std::cerr << "--event-size must be a positive size (e.g. 95K, 1.5M) and --rate >= 0" << std::endl;
        return 1;
    }
    const bool withCP = vm["withcp"].as<bool>();
    const bool autoIP = vm["autoip"].as<bool>();
    if (!synthMode && autoIP == !recvIP.empty()) {
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
    if (deqThreads == 0 || inflight == 0 || batchBuffers == 0) {
        std::cerr << "--deq, --inflight and --batch-buffers must be > 0" << std::endl;
        return 1;
    }
    if (chunkKiB == 0 || batchThreshold <= 0 || batchThreshold > 1 || flushUs <= 0) {
        std::cerr << "--chunk must be > 0, --batch-threshold in (0, 1], --flush-us > 0" << std::endl;
        return 1;
    }
    const size_t chunk = chunkKiB * 1024;
    const size_t maxWrite = parseSize(maxWriteStr) / chunk * chunk;
    if (maxWrite == 0) {
        std::cerr << "--max-write must be at least one chunk (" << chunk << " bytes)" << std::endl;
        return 1;
    }
    if (!dryRun && contName.empty()) {
        std::cerr << "--cont-name is required (the run's DAOS container label), unless --dryrun" << std::endl;
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

    std::unique_ptr<EjfatURI> uri;
    if (!synthMode) {
        auto uriRes = vm.count("uri") ? EjfatURI::getFromString(uriStr, EjfatURI::TokenType::instance, preferV6)
                                      : EjfatURI::getFromEnv("EJFAT_URI", EjfatURI::TokenType::instance, preferV6);
        if (uriRes.has_error()) {
            std::cerr << "Unable to parse EJFAT URI: " << uriRes.error().message() << std::endl;
            return 1;
        }
        uri = std::make_unique<EjfatURI>(uriRes.value());
    }

    if (!timingCsv.empty()) {
        timing.csv.open(timingCsv, std::ios::trunc);
        if (!timing.csv) {
            std::cerr << "Unable to open --timing-csv file " << timingCsv << std::endl;
            return 1;
        }
        timing.csv << std::fixed << std::setprecision(6);  // seconds since program start
        timing.csv << "evt_num,data_id,bytes,chunks,thread,recv_s,commit_s,latency_ms\n";
    }

    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    logOut("E2SAR version:        " + get_Version());
    logOut("Clock start:          " + std::to_string(tStartEpochNs) + " ns since epoch (--timing-csv times are relative to it)");
    logOut("DAOS version:         " + std::to_string(DAOS_VERSION_MAJOR) + "." + std::to_string(DAOS_VERSION_MINOR) +
           "." + std::to_string(DAOS_VERSION_FIX));

    try {
        // Storage first: fail before registering with the load balancer if it is unavailable.
        std::unique_ptr<DaosRuntime> daos;
        std::unique_ptr<DaosPool> daosPool;
        std::unique_ptr<DaosContainer> daosCont;
        std::unique_ptr<Catalog> catalog;
        std::unique_ptr<DaosIntObject> seconds;
        WriterConfig cfg;
        Shared shared;
        std::vector<std::unique_ptr<Pending>> pending;
        for (size_t i = 0; i < deqThreads; i++)
            pending.push_back(std::make_unique<Pending>());

        if (dryRun) {
            logOut("Storage:              none (--dryrun: events are discarded after reassembly)");
        } else {
            daos = std::make_unique<DaosRuntime>();
            daosPool = std::make_unique<DaosPool>(pool);
            daosCont = std::make_unique<DaosContainer>(*daosPool, contName, /*createIfMissing=*/true);
            logOut("DAOS pool/container:  " + pool + "/" + contName + (daosCont->created() ? " (created)" : " (existing)"));

            oclassId(oclass);  // check the class names before anything is written
            oclassId(indexOclass);
            catalog = std::make_unique<Catalog>(*daosCont);
            L::RunSettings mine;
            mine.start = utcSecond();
            mine.chunk = chunk;
            mine.oclass = oclass;
            mine.indexOclass = indexOclass;
            mine.batchThreshold = batchThreshold;
            const auto [run, created] = catalog->createOrJoin(mine);
            const uint32_t slot = catalog->claimSlot(utcSecond());
            seconds = std::make_unique<DaosIntObject>(*daosCont, L::oidLo(L::Kind::Seconds), indexOclass);

            cfg.slot = slot;
            cfg.chunk = chunk;
            cfg.threshold = static_cast<size_t>(batchThreshold * chunk);
            cfg.flushNs = flushUs * 1000;
            cfg.batchBuffers = batchBuffers;
            cfg.maxWrite = maxWrite;
            cfg.inflight = inflight;
            cfg.oclass = oclass;
            cfg.indexOclass = indexOclass;
            cfg.verbose = vm["verbose"].as<bool>();
            shared.cont = daosCont.get();
            shared.seconds = seconds.get();

            logOut("Run:                  " + std::string(created ? "new, started " : "joined, started ") +
                   std::to_string(run.start));
            logOut("Writer slot:          " + std::to_string(slot));
            logOut("Classes:              data " + oclass + ", index " + indexOclass + ", catalog " + L::kCatalogClass);
            logOut("Chunk / batch:        " + std::to_string(chunk) + " bytes; events <= " +
                   std::to_string(cfg.threshold) + " bytes batched, flush after " + std::to_string(flushUs) + " us");
            logOut("Large events:         written in pieces of <= " + std::to_string(maxWrite) + " bytes");
            logOut("Dequeue threads:      " + std::to_string(deqThreads) + " x " + std::to_string(inflight) +
                   " in flight, " + std::to_string(batchBuffers) + " batch buffers each");
            catalog->putWatermark(slot, currentWatermark(pending));
        }

        std::unique_ptr<Reassembler> reas;
        std::unique_ptr<SyntheticSource> synth;
        EventSource src;
        if (synthMode) {
            synth = std::make_unique<SyntheticSource>(synthEvents, synthSize, synthGbps);
            src.synth = synth.get();
            logOut("Synthetic events:     " + std::to_string(synthEvents) + " x " + std::to_string(synthSize) +
                   " bytes, " + (synthGbps > 0 ? std::to_string(synthGbps) + " Gbps" : std::string("unpaced")) +
                   " (no network)");
        } else {
            Reassembler::ReassemblerFlags rflags;
            rflags.useCP = withCP;
            rflags.rcvSocketBufSize = sockBufSize;
            rflags.useHostAddress = preferHostAddr;
            rflags.validateCert = !vm.count("novalidate");
            rflags.eventTimeout_ms = eventTimeoutMS;

            if (vm.count("cores")) {
                reas = autoIP ? std::make_unique<Reassembler>(*uri, recvStartPort, coreList, rflags)
                              : std::make_unique<Reassembler>(*uri, ip::make_address(recvIP), recvStartPort, coreList, rflags);
            } else {
                reas = autoIP ? std::make_unique<Reassembler>(*uri, recvStartPort, numRecvThreads, rflags)
                              : std::make_unique<Reassembler>(*uri, ip::make_address(recvIP), recvStartPort, numRecvThreads, rflags);
            }
            src.reas = reas.get();

            std::ostringstream o;
            o << reas->get_dataIP() << " ports " << reas->get_recvPorts().first << ":" << reas->get_recvPorts().second;
            logOut(std::string("Control plane:        ") + (withCP ? "ON" : "OFF"));
            logOut("Listening on:         " + o.str());

            auto prep = prepareToReceive(*reas);
            if (prep.has_error()) {
                logErr("Unable to start reassembler: " + prep.error().message());
                reas->stopThreads();
                return 1;
            }
            logOut("Receiving; Ctrl-C to stop");
        }

        std::vector<std::thread> workers;
        for (size_t i = 0; i < deqThreads; i++) {
            if (dryRun)
                workers.emplace_back(dryrunLoop, &src, vm["verbose"].as<bool>(), static_cast<int>(i));
            else
                workers.emplace_back(dequeueLoop, &src, &cfg, &shared, pending[i].get(), static_cast<int>(i));
        }
        while (running && src.readyThreads < deqThreads)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (synth)
            synth->start();
        src.go = true;

        std::thread reporter, wmThread;
        if (reportSec > 0)
            reporter = std::thread(reportLoop, reas.get(), reportSec, vm["log-idle"].as<bool>());
        if (catalog)
            wmThread = std::thread(watermarkLoop, catalog.get(), cfg.slot, &pending);

        for (auto& t : workers)
            t.join();  // after Ctrl-C (or the last synthetic event), once everything in flight is done
        running = false;
        if (reporter.joinable())
            reporter.join();
        if (wmThread.joinable())
            wmThread.join();

        if (reas) {
            logOut("Stopping reassembler");
            auto dereg = reas->deregisterWorker();
            if (dereg.has_error())
                logErr("Unable to deregister worker: " + dereg.error().message());
            reas->stopThreads();
        }
        printFinalStats(reas.get());
        printTiming();
        if (catalog) {
            try {
                catalog->putWatermark(cfg.slot, currentWatermark(pending));
                catalog->putDone(cfg.slot, finalStatsRecord(reas.get()));
                logOut("Final watermark and statistics written to the catalog as " + L::doneKey(cfg.slot));
            } catch (const DaosError& e) {  // the events are stored; only the end marker is missing
                logErr(std::string("Unable to write the end of this writer to the catalog: ") + e.what());
            }
        }
        if (timing.csv.is_open()) {
            timing.csv.close();
            logOut("Per-event timing written to " + timingCsv);
        }
    } catch (const DaosError& e) {
        logErr(std::string("DAOS error: ") + e.what());
        return 1;
    } catch (const E2SARException& e) {
        logErr("E2SAR error: " + static_cast<std::string>(e));
        return 1;
    }
    return stats.eventsFailed == 0 ? 0 : 2;
}
