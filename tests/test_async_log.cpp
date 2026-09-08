#include "dart/async_log.hpp"
#include <future>
#include <iostream>
#define CHECK(x) do {if(!(x)) throw std::runtime_error(#x);} while(0)
int main() {
    std::string actual,expected;
    std::mutex m;std::condition_variable ready;size_t bytes=0;
    dart::AsyncLogBuffer buffer(8192,[&](const char*p,size_t n){
        std::lock_guard<std::mutex> lock(m);actual.append(p,n);bytes+=n;ready.notify_one();
    });
    std::ostream out(&buffer);
    for(int i=0;i<100;++i) {
        std::string row(2000,static_cast<char>('A'+i%26));expected+=row;out<<row;out.flush();CHECK(out.good());
        std::unique_lock<std::mutex> lock(m);ready.wait(lock,[&]{return bytes==expected.size();});
    }
    CHECK(buffer.finish());CHECK(buffer.finish());CHECK(actual==expected);
    CHECK(buffer.stats().high_water<=8192);CHECK(buffer.stats().bytes==expected.size());
    // A blocked disk must not block the producer. Deliberate overflow must fail.
    std::promise<void> entered,release;auto released=release.get_future();
    dart::AsyncLogBuffer stalled(4096,[&](const char*,size_t){entered.set_value();released.wait();});
    std::ostream producer(&stalled);producer<<std::string(4096,'x');producer.flush();entered.get_future().wait();
    producer<<std::string(4096,'y');producer.flush();CHECK(producer.good());
    producer<<'z';producer.flush();CHECK(!producer.good());release.set_value();
    CHECK(!stalled.finish());CHECK(stalled.stats().failed);
    dart::AsyncLogBuffer bad(8192,[](const char*,size_t){throw std::runtime_error("disk error");});
    std::ostream bad_out(&bad);bad_out<<"retained error";bad_out.flush();CHECK(!bad.finish());
    dart::AsyncLog missing("/nonexistent-dart-log-parent/test");missing<<"x";CHECK(!missing.finish());
    std::cout<<"bounded asynchronous logging: wrap, drain, blocked sink, overflow and disk failures passed\n";
}
