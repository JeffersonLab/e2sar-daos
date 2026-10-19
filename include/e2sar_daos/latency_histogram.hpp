#pragma once
/**
    Fixed-size log-linear latency histogram (nanoseconds), cheap enough to record every
    put. Values below 8 ns are exact; above that each power of two is split into 8
    buckets, so percentiles are within 12.5% (reported as the bucket's upper bound,
    clamped to the observed max). Count, sum, min and max are exact.

    Not thread-safe: keep one per thread and merge().
*/

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace e2sar_daos {

class LatencyHistogram {
public:
    void record(uint64_t ns) {
        buckets_[index(ns)]++;
        count_++;
        sum_ += ns;
        min_ = std::min(min_, ns);
        max_ = std::max(max_, ns);
    }

    void merge(const LatencyHistogram& o) {
        for (size_t i = 0; i < kBuckets; i++)
            buckets_[i] += o.buckets_[i];
        count_ += o.count_;
        sum_ += o.sum_;
        min_ = std::min(min_, o.min_);
        max_ = std::max(max_, o.max_);
    }

    uint64_t count() const noexcept { return count_; }
    uint64_t minNs() const noexcept { return count_ ? min_ : 0; }
    uint64_t maxNs() const noexcept { return max_; }
    double meanNs() const noexcept { return count_ ? static_cast<double>(sum_) / count_ : 0.0; }

    /** p in [0, 100]. Returns 0 when empty. */
    uint64_t percentileNs(double p) const noexcept {
        if (count_ == 0)
            return 0;
        const auto target = std::max<uint64_t>(1, static_cast<uint64_t>(std::ceil(p / 100.0 * count_)));
        uint64_t seen = 0;
        for (size_t i = 0; i < kBuckets; i++) {
            seen += buckets_[i];
            if (seen >= target)
                return std::min(upperBound(i), max_);
        }
        return max_;
    }

private:
    static constexpr unsigned kSubBits = 3;  // 8 sub-buckets per power of two
    static constexpr uint64_t kSub = 1u << kSubBits;
    static constexpr size_t kBuckets = (64 - kSubBits) * kSub + kSub;

    static size_t index(uint64_t v) {
        if (v < kSub)
            return static_cast<size_t>(v);
        const unsigned msb = 63 - static_cast<unsigned>(__builtin_clzll(v));
        const unsigned shift = msb - kSubBits;
        return static_cast<size_t>(shift * kSub + (v >> shift));  // (v >> shift) in [8, 15]
    }

    static uint64_t upperBound(size_t idx) {
        if (idx < kSub)
            return idx;
        const unsigned shift = static_cast<unsigned>(idx / kSub - 1);
        const uint64_t mantissa = idx % kSub + kSub;
        return ((mantissa + 1) << shift) - 1;
    }

    std::array<uint64_t, kBuckets> buckets_{};
    uint64_t count_ = 0;
    uint64_t sum_ = 0;
    uint64_t min_ = std::numeric_limits<uint64_t>::max();
    uint64_t max_ = 0;
};

}  // namespace e2sar_daos
