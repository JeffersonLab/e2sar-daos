#pragma once
/**
    Event number gaps of one dataId, for the gap counts of e2sar_daos_monitor and
    e2sar_daos_read. A gap is only a hint of loss: a sender need not number its events one
    after another. The receivers' own loss counters are in the catalog (layout::Losses).

    Events mostly come in order. A number above the highest seen opens a gap below it; a
    number inside a gap fills it (late); any other number already passed is a duplicate.

    A sender that restarts numbers its events again from somewhere else (LB 682: from 67.5M
    down to 5.2M). One "highest seen" would then call every new event a duplicate and never
    see a gap again. So the numbers are kept in segments: a number more than restartGap away
    from every segment starts a new one (a restart), and is not a gap. Events of an older
    segment that arrive after the restart (another writer still catching up) still go to
    their own segment, so its gaps can still be filled.

    Not thread-safe.
*/

#include <cstdint>
#include <deque>
#include <map>
#include <utility>

namespace e2sar_daos {

class EventGaps {
public:
    static constexpr uint64_t kDefaultRestartGap = 1000000;
    static constexpr size_t kMaxSegments = 16;  // older ones are dropped with their open gaps

    struct Gap {
        uint64_t last = 0;
        double since = 0;      // when it opened (unix s)
        bool counted = false;  // already added to everMissing()
    };
    struct Segment {
        uint64_t first = 0, max = 0;
        std::map<uint64_t, Gap> gaps;  // first -> gap
    };
    enum class Kind {
        Next,       // above the segment's highest number (maybe after a gap)
        Late,       // filled a gap
        Earlier,    // below the segment's first number (its first events came late)
        Duplicate,  // already seen
        Restart,    // started a new segment (the first event is one too)
    };

    explicit EventGaps(uint64_t restartGap = kDefaultRestartGap) : restartGap_(restartGap ? restartGap : 1) {}

    /** Add event number evt, seen at unix time now. inCurrent: it went to the newest segment. */
    Kind add(uint64_t evt, double now, bool* inCurrent = nullptr) {
        Segment* s = nullptr;
        for (auto it = segs_.rbegin(); it != segs_.rend(); ++it)
            if (evt + restartGap_ >= it->first && evt <= it->max + restartGap_) {
                s = &*it;
                break;
            }
        if (!s) {
            if (segs_.size() == kMaxSegments) {
                segs_.pop_front();
                dropped_++;
            }
            segs_.push_back({evt, evt, {}});
            if (inCurrent)
                *inCurrent = true;
            return Kind::Restart;
        }
        if (inCurrent)
            *inCurrent = s == &segs_.back();
        if (evt > s->max) {
            if (evt > s->max + 1)
                s->gaps[s->max + 1] = {evt - 1, now, false};
            s->max = evt;
            return Kind::Next;
        }
        if (fill(*s, evt))
            return Kind::Late;
        if (evt < s->first) {
            s->first = evt;  // the segment started earlier than its first event seen
            return Kind::Earlier;
        }
        return Kind::Duplicate;
    }

    bool empty() const { return segs_.empty(); }
    /** The newest segment. Only when !empty(). */
    const Segment& current() const { return segs_.back(); }
    const std::deque<Segment>& segments() const { return segs_; }
    /** Segments started after the first: the sender's restarts seen so far. */
    uint64_t restarts() const { return segs_.empty() ? 0 : segs_.size() - 1 + dropped_; }

    /** Event numbers in open gaps, every segment. */
    uint64_t open() const {
        uint64_t n = 0;
        for (const auto& s : segs_)
            for (const auto& [lo, g] : s.gaps)
                n += g.last - lo + 1;
        return n;
    }

    /**
        Open gaps older than grace (missing) and younger (pending), every segment. Gaps that
        just passed grace are added to everMissing(), once: it never goes down, even when a
        late event fills its gap.
    */
    std::pair<uint64_t, uint64_t> missing(double now, double grace) {
        uint64_t m = 0, p = 0;
        for (auto& s : segs_)
            for (auto& [lo, g] : s.gaps) {
                if (now - g.since > grace) {
                    m += g.last - lo + 1;
                    if (!g.counted) {
                        everMissing_ += g.last - lo + 1;
                        g.counted = true;
                    }
                } else {
                    p += g.last - lo + 1;
                }
            }
        return {m, p};
    }
    uint64_t everMissing() const { return everMissing_; }

private:
    // Take n out of its gap, if it is in one.
    static bool fill(Segment& s, uint64_t n) {
        auto it = s.gaps.upper_bound(n);
        if (it == s.gaps.begin())
            return false;
        --it;
        const uint64_t lo = it->first;
        const Gap g = it->second;
        if (n > g.last)
            return false;
        s.gaps.erase(it);
        if (lo < n)
            s.gaps[lo] = {n - 1, g.since, g.counted};
        if (n < g.last)
            s.gaps[n + 1] = {g.last, g.since, g.counted};
        return true;
    }

    uint64_t restartGap_;
    std::deque<Segment> segs_;
    uint64_t dropped_ = 0;
    uint64_t everMissing_ = 0;
};

}  // namespace e2sar_daos
