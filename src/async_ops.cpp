#include "e2sar_daos/async_ops.hpp"

#include <iostream>

#include "e2sar_daos/daos_handles.hpp"

namespace e2sar_daos {

AsyncOps::AsyncOps(size_t maxInflight)
    : evs_(maxInflight == 0 ? 1 : maxInflight), slots_(evs_.size()), polled_(evs_.size()) {
    check(daos_eq_create(&eqh_), "daos_eq_create");
    free_.reserve(slots_.size());
    for (size_t i = slots_.size(); i-- > 0;)
        free_.push_back(i);
}

AsyncOps::~AsyncOps() {
    try {
        drain();
    } catch (const std::exception& e) {
        std::cerr << "AsyncOps: drain failed on destruction: " << e.what() << std::endl;
    }
    if (daos_handle_is_valid(eqh_)) {
        if (daos_eq_destroy(eqh_, 0) != 0)
            daos_eq_destroy(eqh_, DAOS_EQ_DESTROY_FORCE);
    }
}

void AsyncOps::submit(const Launcher& launch, Callback cb) {
    while (free_.empty())
        progress(DAOS_EQ_WAIT);

    const size_t idx = free_.back();
    free_.pop_back();
    daos_event_t* ev = &evs_[idx];
    Slot& s = slots_[idx];

    int rc = daos_event_init(ev, eqh_, nullptr);
    if (rc != 0) {
        free_.push_back(idx);
        cb(rc, 0);
        return;
    }
    s.cb = std::move(cb);
    s.launchRc = 0;
    s.launched = std::chrono::steady_clock::now();
    inflight_++;

    rc = launch(ev);
    if (rc != 0) {
        // Never launched: finalize now. Otherwise it was completed into the EQ and
        // progress() reaps it.
        if (daos_event_fini(ev) == 0)
            complete(idx, rc, 0);
        else
            s.launchRc = rc;
    }
}

size_t AsyncOps::progress(int64_t timeoutUs) {
    if (inflight_ == 0)
        return 0;
    const int n = daos_eq_poll(eqh_, 1, timeoutUs, static_cast<unsigned int>(polled_.size()), polled_.data());
    check(n < 0 ? n : 0, "daos_eq_poll");

    const auto now = std::chrono::steady_clock::now();
    for (int i = 0; i < n; i++) {
        daos_event_t* ev = polled_[i];
        const size_t idx = static_cast<size_t>(ev - evs_.data());
        int rc = ev->ev_error;
        if (rc == 0)
            rc = slots_[idx].launchRc;
        const auto ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - slots_[idx].launched).count());
        daos_event_fini(ev);
        complete(idx, rc, ns);
    }
    return static_cast<size_t>(n);
}

void AsyncOps::drain() {
    while (inflight_ > 0)
        progress(DAOS_EQ_WAIT);
}

void AsyncOps::complete(size_t idx, int rc, uint64_t ns) {
    Slot& s = slots_[idx];
    Callback cb = std::move(s.cb);
    s.cb = nullptr;
    inflight_--;
    free_.push_back(idx);  // free the slot first so the callback sees capacity
    if (cb)
        cb(rc, ns);
}

}  // namespace e2sar_daos
