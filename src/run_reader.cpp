#include "e2sar_daos/run_reader.hpp"

#include <algorithm>
#include <cstring>

namespace e2sar_daos {

using namespace layout;

RunReader::RunReader(const DaosContainer& cont, const RunSettings& run, unsigned int mode, size_t inflight,
                     size_t readSize)
    : cont_(cont), run_(run), mode_(mode), readSize_(std::max<size_t>(readSize / run.chunk, 1) * run.chunk),
      seconds_(cont, oidLo(Kind::Seconds), run.indexOclass, mode), ops_(inflight) {}

std::vector<uint64_t> RunReader::listSeconds() {
    std::vector<uint64_t> out;
    seconds_.listDkeys([&](uint64_t t) { out.push_back(t); });
    std::sort(out.begin(), out.end());
    return out;
}

namespace {

// One index value being fetched. Heap-allocated: DAOS holds pointers into it.
struct TableFetch {
    size_t object = 0;
    uint64_t second = 0;
    uint64_t dkey = 0;
    std::vector<uint8_t> buf;
    IntKeyUpdate u;
    int tries = 0;
    int rc = 0;
    bool done = false;

    void prepare() {
        u.set(dkey, kAkey, buf.data(), buf.size());
        u.iod.iod_size = DAOS_REC_ANY;  // DAOS returns the value's size
    }
};

}  // namespace

std::vector<IndexedBatch> RunReader::readIndex(const std::vector<uint64_t>& seconds) {
    std::vector<std::unique_ptr<DaosIntObject>> objects;
    std::vector<std::unique_ptr<TableFetch>> fetches;
    for (uint64_t t : seconds) {
        objects.push_back(std::make_unique<DaosIntObject>(cont_, oidLo(Kind::Index, 0, t), run_.indexOclass, mode_));
        objects.back()->listDkeys([&](uint64_t dk) {
            auto f = std::make_unique<TableFetch>();
            f->object = objects.size() - 1;
            f->second = t;
            f->dkey = dk;
            f->buf.resize(16384);  // a 500-event table; larger ones are fetched again at their size
            fetches.push_back(std::move(f));
        });
    }

    std::vector<TableFetch*> todo;
    for (auto& f : fetches)
        todo.push_back(f.get());
    while (!todo.empty()) {
        std::vector<TableFetch*> again;
        for (TableFetch* f : todo) {
            f->tries++;
            f->prepare();
            const daos_handle_t oh = objects[f->object]->handle();
            ops_.submit(
                [f, oh](daos_event_t* ev) {
                    return daos_obj_fetch(oh, DAOS_TX_NONE, 0, &f->u.dkeyIov, 1, &f->u.iod, &f->u.sgl, nullptr, ev);
                },
                [f, &again](int rc, uint64_t) {
                    if (rc == -DER_REC2BIG && f->u.iod.iod_size > f->buf.size()) {
                        f->buf.resize(f->u.iod.iod_size);
                        again.push_back(f);
                    } else if (rc != 0 && f->tries < 2) {
                        again.push_back(f);
                    } else {
                        f->rc = rc;
                        f->done = true;
                    }
                });
        }
        ops_.drain();
        todo.swap(again);
    }

    std::vector<IndexedBatch> out;
    out.reserve(fetches.size());
    for (auto& f : fetches) {
        if (f->rc != 0)
            throw DaosError("fetching index[" + std::to_string(f->second) + "] dkey " + std::to_string(f->dkey), f->rc);
        const size_t size = f->u.iod.iod_size;
        auto tab = decodeTable(f->buf.data(), std::min(size, f->buf.size()));
        if (!tab || tab->slot != indexDkeySlot(f->dkey))
            throw DaosError("index[" + std::to_string(f->second) + "] dkey " + std::to_string(f->dkey) +
                                ": not a valid batch table",
                            -DER_INVAL);
        out.push_back({f->second, f->dkey, std::move(*tab)});
    }
    return out;
}

DaosArray& RunReader::array(uint32_t slot, uint64_t t) {
    auto& a = arrays_[{slot, t}];
    if (!a)
        a = std::make_unique<DaosArray>(cont_, oidLo(Kind::Data, slot, t), run_.oclass, run_.chunk, mode_);
    return *a;
}

namespace {

struct PieceRead {
    size_t range = 0;
    uint64_t at = 0;  // from the range start
    std::vector<uint8_t> buf;
    ArrayWrite d;  // the same descriptors serve for reading
    int tries = 0;
};

}  // namespace

std::vector<size_t> RunReader::read(const std::vector<DataRange>& ranges,
                                    const std::function<void(const DataPiece&)>& fn) {
    std::vector<size_t> failed;
    std::vector<std::pair<size_t, uint64_t>> todo;  // (range, at)
    for (size_t r = 0; r < ranges.size(); r++)
        for (uint64_t at = 0; at < ranges[r].length; at += readSize_)
            todo.push_back({r, at});

    std::vector<std::unique_ptr<PieceRead>> live;  // pieces in flight or waiting for a retry
    std::vector<PieceRead*> retry, finished;

    auto submit = [&](PieceRead* p) {
        p->tries++;
        const DataRange& r = ranges[p->range];
        p->d.set(r.offset + p->at, p->buf.data(), p->buf.size());
        const daos_handle_t oh = array(r.slot, r.tData).handle();
        ops_.submit([p, oh](daos_event_t* ev) { return daos_array_read(oh, DAOS_TX_NONE, &p->d.iod, &p->d.sgl, ev); },
                    [&, p](int rc, uint64_t) {
                        if (rc == 0) {
                            fn({p->range, p->at, p->buf.data(), p->buf.size()});
                        } else if (p->tries < 2) {
                            retry.push_back(p);
                            return;
                        } else {
                            failed.push_back(p->range);
                        }
                        finished.push_back(p);
                    });
    };
    auto settle = [&]() {
        while (!retry.empty() || !finished.empty()) {
            auto again = std::move(retry);
            retry.clear();
            for (PieceRead* p : again)
                submit(p);
            auto fin = std::move(finished);
            finished.clear();
            for (PieceRead* p : fin)
                live.erase(std::find_if(live.begin(), live.end(), [p](const auto& u) { return u.get() == p; }));
        }
    };

    for (const auto& [r, at] : todo) {
        auto p = std::make_unique<PieceRead>();
        p->range = r;
        p->at = at;
        p->buf.resize(std::min<uint64_t>(readSize_, ranges[r].length - at));
        PieceRead* raw = p.get();
        live.push_back(std::move(p));
        submit(raw);  // waits for a free slot first, which runs completions
        settle();
    }
    while (!live.empty()) {
        ops_.progress(DAOS_EQ_WAIT);
        settle();
    }
    std::sort(failed.begin(), failed.end());
    failed.erase(std::unique(failed.begin(), failed.end()), failed.end());
    return failed;
}

void RunReader::removeSecond(uint64_t t) {
    DaosIntObject index(cont_, oidLo(Kind::Index, 0, t), run_.indexOclass, mode_);
    index.punch();
    uint64_t key = t;
    daos_key_t dkey;
    d_iov_set(&dkey, &key, sizeof(key));
    check(daos_obj_punch_dkeys(seconds_.handle(), DAOS_TX_NONE, 0, 1, &dkey, nullptr),
          "daos_obj_punch_dkeys(seconds " + std::to_string(t) + ")");
}

void RunReader::removeData(uint64_t t, uint32_t slot) {
    DaosArray& a = array(slot, t);
    check(daos_array_destroy(a.handle(), DAOS_TX_NONE, nullptr), "daos_array_destroy");
    arrays_.erase({slot, t});
}

}  // namespace e2sar_daos
