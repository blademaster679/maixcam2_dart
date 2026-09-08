#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <ostream>
#include <streambuf>
#include <thread>
#include <vector>

namespace dart {
// Single producer stream, bounded pre-touched byte ring, one disk writer.
// The producer never waits for disk. Overflow is an explicit stream failure;
// no telemetry is silently discarded. finish() drains and joins the writer.
class AsyncLogBuffer final : public std::streambuf {
public:
    using Sink=std::function<void(const char*,size_t)>;
    struct Stats { size_t capacity=0, high_water=0; uint64_t bytes=0,max_write_us=0; bool failed=false; };
    AsyncLogBuffer(size_t capacity,Sink sink):ring_(std::max<size_t>(capacity,1),0),sink_(std::move(sink)) {
        stats_.capacity=ring_.size(); setp(staging_.data(),staging_.data()+staging_.size());
        worker_=std::thread([this]{run();});
    }
    ~AsyncLogBuffer() override {finish();}
    bool finish() {
        if(!finished_) {
            sync();
            {std::lock_guard<std::mutex> lock(mutex_); closed_=true;}
            ready_.notify_one(); worker_.join(); finished_=true;
        }
        return !stats().failed;
    }
    Stats stats() const {std::lock_guard<std::mutex> lock(mutex_);return stats_;}
protected:
    int sync() override {
        const size_t n=pptr()-pbase();
        {std::lock_guard<std::mutex> lock(mutex_);
         if(closed_ || stats_.failed || n>ring_.size()-used_) {stats_.failed=true;return -1;}
         const size_t first=std::min(n,ring_.size()-tail_);
         std::memcpy(ring_.data()+tail_,pbase(),first);
         std::memcpy(ring_.data(),pbase()+first,n-first);
         tail_=(tail_+n)%ring_.size();used_+=n;stats_.high_water=std::max(stats_.high_water,used_);}
        setp(staging_.data(),staging_.data()+staging_.size());ready_.notify_one();return 0;
    }
    int_type overflow(int_type ch) override {
        if(sync()) return traits_type::eof();
        if(!traits_type::eq_int_type(ch,traits_type::eof())) {*pptr()=traits_type::to_char_type(ch);pbump(1);}
        return traits_type::not_eof(ch);
    }
private:
    void run() noexcept {
        std::array<char,65536> batch{};
        for(;;) {
            size_t n;
            {std::unique_lock<std::mutex> lock(mutex_);
             ready_.wait(lock,[this]{return used_ || closed_ || stats_.failed;});
             if(stats_.failed || (!used_ && closed_)) return;
             n=std::min(used_,batch.size());const size_t first=std::min(n,ring_.size()-head_);
             std::memcpy(batch.data(),ring_.data()+head_,first);
             std::memcpy(batch.data()+first,ring_.data(),n-first);
             head_=(head_+n)%ring_.size();used_-=n;}
            const auto start=std::chrono::steady_clock::now();
            try {sink_(batch.data(),n);} catch(...) {
                std::lock_guard<std::mutex> lock(mutex_);stats_.failed=true;return;
            }
            const auto us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
            {std::lock_guard<std::mutex> lock(mutex_);stats_.bytes+=n;stats_.max_write_us=std::max(stats_.max_write_us,static_cast<uint64_t>(us));}
        }
    }
    std::array<char,4096> staging_{};
    std::vector<char> ring_;
    Sink sink_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    size_t head_=0,tail_=0,used_=0;
    bool closed_=false,finished_=false;
    Stats stats_;
    std::thread worker_;
};
class AsyncLog final : public std::ostream {
public:
    explicit AsyncLog(const std::string &path,size_t capacity=1024*1024):std::ostream(nullptr),path_(path),file_(path),buffer_(capacity,[this](const char*p,size_t n){
        file_.write(p,n);if(!file_) throw std::runtime_error("async disk write failed");
    }) {rdbuf(&buffer_);if(!file_) setstate(std::ios::badbit);}
    ~AsyncLog() override {finish();}
    bool finish() {
        if(finished_) return good();
        flush();const bool ok=buffer_.finish();file_.flush();
        const auto s=buffer_.stats();
        std::ofstream audit(path_+".io.json");
        audit<<"{\"capacity_bytes\":"<<s.capacity<<",\"high_water_bytes\":"<<s.high_water<<",\"written_bytes\":"<<s.bytes<<",\"max_write_us\":"<<s.max_write_us<<",\"failed\":"<<(s.failed?"true":"false")<<"}\n";
        audit.flush();if(!ok || !file_ || !audit) setstate(std::ios::badbit);
        finished_=true;return good();
    }
private:
    std::string path_;
    std::ofstream file_;
    AsyncLogBuffer buffer_;
    bool finished_=false;
};
}
