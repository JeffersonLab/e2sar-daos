#pragma once
/**
    Any asynchronous DAOS operation on one event queue, with a fixed number in flight.

    submit() takes a launcher, which starts the operation on the event it is given
    (daos_array_write, daos_obj_update, …), and a callback for its completion. If every
    slot is busy, submit() first polls the queue until one frees up: that is how
    back-pressure reaches the caller. Callbacks run on the calling thread, inside
    submit()/progress()/drain(), with the result code and the launch-to-completion time.

    Not thread-safe: an event queue is polled by one thread. Use one AsyncOps per thread.
    Whatever the operation points to (descriptors, buffers) must stay valid until its
    callback has run. Do not call submit() from inside a callback.
*/

#include <daos.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>

namespace e2sar_daos {

class AsyncOps {
public:
    /** rc is 0 on success, otherwise a negative DER_* code; ns is launch to completion. */
    using Callback = std::function<void(int rc, uint64_t ns)>;
    /** Start the operation on ev; return its launch rc. */
    using Launcher = std::function<int(daos_event_t* ev)>;

    explicit AsyncOps(size_t maxInflight);
    /** Drains outstanding operations, then destroys the event queue. */
    ~AsyncOps();
    AsyncOps(const AsyncOps&) = delete;
    AsyncOps& operator=(const AsyncOps&) = delete;

    /** Launch one operation. If the launch fails, cb runs right away with the error. */
    void submit(const Launcher& launch, Callback cb);

    /**
        Reap completed operations, running their callbacks.
        timeoutUs: 0 = don't wait, <0 = wait for at least one, >0 = wait up to that long.
    */
    size_t progress(int64_t timeoutUs = 0);

    /** Block until every operation has completed. */
    void drain();

    size_t inflight() const noexcept { return inflight_; }
    size_t capacity() const noexcept { return slots_.size(); }

private:
    struct Slot {
        Callback cb;
        int launchRc = 0;
        std::chrono::steady_clock::time_point launched;
    };

    daos_handle_t eqh_ = DAOS_HDL_INVAL;
    // evs_[i] belongs to slots_[i]; a polled event maps back by pointer offset.
    // Sized once and never reallocated, since DAOS holds the event pointers.
    std::vector<daos_event_t> evs_;
    std::vector<Slot> slots_;
    std::vector<size_t> free_;
    std::vector<daos_event_t*> polled_;
    size_t inflight_ = 0;

    void complete(size_t idx, int rc, uint64_t ns);
};

}  // namespace e2sar_daos
