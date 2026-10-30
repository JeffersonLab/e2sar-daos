#pragma once
/**
    Asynchronous KV writer built on a DAOS event queue.

    Legacy: layout v1, before one container per run (#31). The applications no longer use it;
    e2sar_daos_recv now writes arrays through AsyncOps (async_ops.hpp).

    Each writer owns one event queue (EQ) and a fixed pool of in-flight slots. submit()
    launches a non-blocking daos_kv_put(); if every slot is busy it first polls the EQ
    until one frees up, which is how back-pressure reaches the caller. Completion
    callbacks run on the calling thread, inside submit()/progress()/drain().

    A writer is NOT thread-safe: DAOS event queues must be polled by a single thread.
    Use one writer per thread; they can all share the same DaosKV.

    The caller's value buffer must stay valid until its callback has run.
    Do not call submit() from inside a completion callback.
*/

#include "e2sar_daos/daos_handles.hpp"
#include "e2sar_daos/latency_histogram.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace e2sar_daos {

class AsyncKVWriter {
public:
    /** rc is 0 on success, otherwise a negative DER_* code. */
    using Callback = std::function<void(int rc)>;

    AsyncKVWriter(const DaosKV& kv, size_t maxInflight);
    /** Drains outstanding operations, then destroys the EQ. */
    ~AsyncKVWriter();
    AsyncKVWriter(const AsyncKVWriter&) = delete;
    AsyncKVWriter& operator=(const AsyncKVWriter&) = delete;

    /**
        Launch a put of [buf, buf+size) under key. Blocks only while all slots are busy.
        If the launch itself fails, cb is invoked synchronously with the error.
    */
    void submit(std::string key, const void* buf, size_t size, Callback cb);

    /**
        Reap completed operations, running their callbacks.
        timeoutUs: 0 = don't wait, <0 = wait for at least one, >0 = wait up to that long.
        Returns the number of operations reaped.
    */
    size_t progress(int64_t timeoutUs = 0);

    /** Block until every in-flight operation has completed. */
    void drain();

    size_t inflight() const noexcept { return inflight_; }
    size_t capacity() const noexcept { return slots_.size(); }

    /** Launch-to-completion time of every successful put (excludes waiting for a slot). */
    const LatencyHistogram& putLatency() const noexcept { return putLatency_; }

private:
    struct Slot {
        std::string key;  // must outlive the in-flight put
        Callback cb;
        int launchRc = 0;
        std::chrono::steady_clock::time_point launched;
    };

    const DaosKV& kv_;
    daos_handle_t eqh_ = DAOS_HDL_INVAL;
    // evs_[i] belongs to slots_[i]; a polled event maps back by pointer offset.
    // Both are sized once and never reallocated, since DAOS holds the event pointers.
    std::vector<daos_event_t> evs_;
    std::vector<Slot> slots_;
    std::vector<size_t> free_;
    std::vector<daos_event_t*> polled_;
    size_t inflight_ = 0;
    LatencyHistogram putLatency_;

    void complete(size_t idx, int rc);
};

}  // namespace e2sar_daos
