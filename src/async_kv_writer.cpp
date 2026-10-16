#include "e2sar_daos/async_kv_writer.hpp"

#include <iostream>

namespace e2sar_daos {

AsyncKVWriter::AsyncKVWriter(const DaosKV& kv, size_t maxInflight)
    : kv_(kv), evs_(maxInflight == 0 ? 1 : maxInflight), slots_(evs_.size()),
      polled_(evs_.size()) {
    check(daos_eq_create(&eqh_), "daos_eq_create");
    free_.reserve(slots_.size());
    for (size_t i = slots_.size(); i-- > 0;)
        free_.push_back(i);
}

AsyncKVWriter::~AsyncKVWriter() {
    try {
        drain();
    } catch (const std::exception& e) {
        std::cerr << "AsyncKVWriter: drain failed on destruction: " << e.what() << std::endl;
    }
    if (daos_handle_is_valid(eqh_)) {
        int rc = daos_eq_destroy(eqh_, 0);
        if (rc != 0)
            daos_eq_destroy(eqh_, DAOS_EQ_DESTROY_FORCE);
    }
}

void AsyncKVWriter::submit(std::string key, const void* buf, size_t size, Callback cb) {
    while (free_.empty())
        progress(DAOS_EQ_WAIT);

    const size_t idx = free_.back();
    free_.pop_back();
    daos_event_t* ev = &evs_[idx];
    Slot& s = slots_[idx];

    int rc = daos_event_init(ev, eqh_, nullptr);
    if (rc != 0) {
        free_.push_back(idx);
        cb(rc);
        return;
    }

    s.key = std::move(key);
    s.cb = std::move(cb);
    s.launchRc = 0;
    inflight_++;

    // The key string lives in the slot until completion, as DAOS requires.
    rc = daos_kv_put(kv_.handle(), DAOS_TX_NONE, 0, s.key.c_str(), size, buf, ev);
    if (rc != 0) {
        // If the operation never got launched the event can be finalized right away;
        // otherwise it has been completed into the EQ and progress() will reap it.
        if (daos_event_fini(ev) == 0)
            complete(idx, rc);
        else
            s.launchRc = rc;
    }
}

size_t AsyncKVWriter::progress(int64_t timeoutUs) {
    if (inflight_ == 0)
        return 0;

    int n = daos_eq_poll(eqh_, 1, timeoutUs, static_cast<unsigned int>(polled_.size()),
                         polled_.data());
    check(n < 0 ? n : 0, "daos_eq_poll");

    for (int i = 0; i < n; i++) {
        daos_event_t* ev = polled_[i];
        const size_t idx = static_cast<size_t>(ev - evs_.data());
        int rc = ev->ev_error;
        if (rc == 0)
            rc = slots_[idx].launchRc;
        daos_event_fini(ev);
        complete(idx, rc);
    }
    return static_cast<size_t>(n);
}

void AsyncKVWriter::drain() {
    while (inflight_ > 0)
        progress(DAOS_EQ_WAIT);
}

void AsyncKVWriter::complete(size_t idx, int rc) {
    Slot& s = slots_[idx];
    Callback cb = std::move(s.cb);
    s.cb = nullptr;
    s.key.clear();
    inflight_--;
    free_.push_back(idx);  // free the slot first so the callback sees capacity
    if (cb)
        cb(rc);
}

}  // namespace e2sar_daos
