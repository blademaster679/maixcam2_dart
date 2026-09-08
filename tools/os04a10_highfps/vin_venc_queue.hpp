#pragma once
// Ownership queue: a successful push transfers one borrowed VIN image to this
// worker. Every accepted image is released exactly once, including on failure.
#include <array>
#include <mutex>
#include <condition_variable>
#include <memory>
#include "ax_vin_api.h"
#include "direct_venc.hpp"
#include "venc_input_copy.hpp"
class VinVencQueue {
    DirectVenc &encoder;
    std::array<AX_IMG_INFO_T,32> images{};
    std::mutex mutex;
    std::condition_variable ready;
    size_t head=0,tail=0,count=0;
    bool closed=false;
    std::thread worker;
    std::unique_ptr<VencInputCopy> copy_pool;
public:
    std::atomic<bool> failed{false};
    std::atomic<unsigned long> accepted{0},released{0},release_errors{0};
    size_t high_water=0;
    explicit VinVencQueue(DirectVenc &e,bool copy=false):encoder(e) {
        if(copy)copy_pool=std::make_unique<VencInputCopy>();
        worker=std::thread([this] {
        for(;;) {
            AX_IMG_INFO_T image={};
            {
                std::unique_lock<std::mutex> lock(mutex);
                ready.wait(lock,[this]{return closed || count;});
                if(!count) break;
                image=images[head];head=(head+1)%images.size();--count;
            }
            AX_VIDEO_FRAME_INFO_T copied={};bool has_copy=false;
            if(!failed && !encoder.failed && copy_pool) {
                int rc=copy_pool->copy(image.tFrameInfo,copied);
                if(rc) {std::cerr<<"VENC input copy failed="<<rc<<'\n';failed=true;}
                else has_copy=true;
            }
            if(!copy_pool && !failed && !encoder.failed && encoder.send(image.tFrameInfo))failed=true;
            if(AX_VIN_ReleaseYuvFrame(0,AX_VIN_CHN_ID_MAIN,&image)) {++release_errors;failed=true;}
            ++released;
            if(has_copy) {
                if(!failed && !encoder.failed && encoder.send(copied))failed=true;
                if(AX_POOL_ReleaseBlock(copied.stVFrame.u32BlkId[0])) {++release_errors;failed=true;}
            }
        }
        });
    }
    bool push(const AX_IMG_INFO_T &image) {
        std::lock_guard<std::mutex> lock(mutex);
        if(closed || failed || encoder.failed || count==images.size()) return false;
        images[tail]=image;tail=(tail+1)%images.size();++count;++accepted;
        high_water=std::max(high_water,count);ready.notify_one();return true;
    }
    void finish() {
        {std::lock_guard<std::mutex> lock(mutex);closed=true;}
        ready.notify_all();if(worker.joinable())worker.join();
    }
    int close_pool() {const int rc=copy_pool ? copy_pool->close() : 0;if(rc)failed=true;return rc;}
    ~VinVencQueue(){finish();encoder.finish();if(close_pool())std::cerr<<"VENC input pool destroy failed\n";}
};
