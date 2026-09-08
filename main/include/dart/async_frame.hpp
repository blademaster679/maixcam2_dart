#pragma once
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace dart {
struct FrameMetadata {
    uint64_t sequence = 0, pts_raw = 0, received_us = 0;
    // SDK input sequence and payload PTS; PTS scale/exposure event unverified.
};
struct SequenceStats {
    uint64_t received = 0, missing = 0, duplicate = 0, reversed = 0, pts_reversed = 0;
    uint64_t last_sequence = 0, last_pts = 0;
    void observe(const FrameMetadata &m) {
        if (received) {
            if (m.sequence == last_sequence) ++duplicate;
            else if (m.sequence < last_sequence) ++reversed; // no assumed wrap width
            else missing += m.sequence - last_sequence - 1;
            if (m.pts_raw <= last_pts) ++pts_reversed;
        }
        ++received; last_sequence = m.sequence; last_pts = m.pts_raw;
    }
};
// One pending frame plus consumer-owned leases. Never wait for the consumer.
// Dropped objects are destroyed OUTSIDE the mutex (SDK release may block).
template<class T> class LatestFrameSlot {
public:
    struct Stats { uint64_t published=0, taken=0, replaced=0, shutdown_discarded=0, rejected=0; };
    bool publish(std::shared_ptr<T> item) {
        if (!item) throw std::invalid_argument("null latest frame");
        std::shared_ptr<T> old;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) { ++stats_.rejected; return false; }
            old.swap(pending_); pending_ = std::move(item);
            ++stats_.published; if (old) ++stats_.replaced;
        }
        ready_.notify_one(); return true;
    }
    std::shared_ptr<T> take() {
        std::lock_guard<std::mutex> lock(mutex_);
        return take_locked();
    }
    std::shared_ptr<T> wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [this]{ return closed_ || pending_; });
        return take_locked();
    }
    void close() {
        std::shared_ptr<T> old;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true; old.swap(pending_);
            if (old) ++stats_.shutdown_discarded;
        }
        ready_.notify_all();
    }
    Stats stats() const { std::lock_guard<std::mutex> lock(mutex_); return stats_; }
    ~LatestFrameSlot() { close(); } // owner must join waiters before destruction
private:
    std::shared_ptr<T> take_locked() {
        std::shared_ptr<T> result; result.swap(pending_);
        if (result) ++stats_.taken;
        return result;
    }
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::shared_ptr<T> pending_;
    Stats stats_;
    bool closed_ = false;
};
// Absolute timestamp deadlines, rational rate. A late caller skips missed slots;
// it never runs a burst of stale work to catch up. Regressing clocks are rejected.
class TimestampSchedule {
public:
    explicit TimestampSchedule(unsigned hz): hz_(hz) {
        if (!hz || hz > 1000000) throw std::invalid_argument("invalid rate");
    }
    bool due(uint64_t now) {
        if (!started_) { started_=true; origin_=last_=now; tick_=1; return true; }
        if (now < last_) throw std::invalid_argument("timestamp regression");
        last_=now;
        if (now-origin_ < (tick_*1000000 + hz_-1)/hz_) return false;
        tick_=(now-origin_)*hz_/1000000+1; return true;
    }
private:
    uint64_t origin_=0, last_=0, tick_=0;
    unsigned hz_; bool started_=false;
};
} // namespace dart
