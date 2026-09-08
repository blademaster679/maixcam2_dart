#include "dart/async_frame.hpp"
#include <atomic>
#include <iostream>
#include <thread>
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (0)
struct Lease { std::atomic<int>& released; int value; ~Lease(){++released;} };
int main() {
    std::atomic<int> freed{0};
    dart::LatestFrameSlot<Lease> q;
    auto make=[&](int n){return std::shared_ptr<Lease>(new Lease{freed,n});};
    q.publish(make(1)); q.publish(make(2)); CHECK(freed==1);
    auto owned=q.take(); CHECK(owned->value==2);
    q.publish(make(3)); CHECK(owned->value==2); q.close(); CHECK(freed==2);
    CHECK(!q.publish(make(4))); CHECK(freed==3); owned.reset(); CHECK(freed==4);
    auto s=q.stats(); CHECK(s.published==3 && s.replaced==1 && s.taken==1 && s.shutdown_discarded==1 && s.rejected==1);
    dart::LatestFrameSlot<Lease> empty;
    std::thread waiter([&]{ CHECK(!empty.wait()); }); empty.close(); waiter.join();
    dart::LatestFrameSlot<Lease> race;
    std::thread consumer([&]{ while(auto f=race.wait()) CHECK(f->value>=0); });
    for(int i=0;i<10000;++i) race.publish(make(i));
    race.close(); consumer.join(); CHECK(freed==10004);
    s=race.stats(); CHECK(s.published==s.taken+s.replaced+s.shutdown_discarded);
    dart::SequenceStats seq;
    for(auto n:{10,11,14,14,13}) seq.observe({static_cast<uint64_t>(n),static_cast<uint64_t>(n),0});
    CHECK(seq.missing==2 && seq.duplicate==1 && seq.reversed==1 && seq.pts_reversed==2);
    dart::TimestampSchedule rate(180); int count=0;
    for(uint64_t t=0;t<1000000;++t) if(rate.due(t)) ++count;
    CHECK(count==180); CHECK(rate.due(2000000)); CHECK(!rate.due(2000000));
    bool threw=false; try{rate.due(1);}catch(const std::invalid_argument&){threw=true;} CHECK(threw);
    std::cout<<"async ownership, shutdown, concurrent replacement, sequence and timestamp tests passed\n";
}
