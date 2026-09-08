#pragma once
#include "dart/nv21_pipeline.hpp"
#include "ax_vin_api.h"
#include "ax_sys_api.h"
#include <atomic>
// A lease owns one borrowed VIN frame, with CPU mappings valid until destruction.
// Acquire, map, invalidate, unmap and release failures remain separately visible.
struct VinLeaseStats {
    std::atomic<uint64_t> acquired{0}, released{0}, release_errors{0}, map_errors{0};
};
class VinNv21Frame final : public dart::Nv21Frame {
    std::shared_ptr<VinLeaseStats> stats_;
    AX_IMG_INFO_T image_{};
    bool owned_=false;
    void *y_=nullptr,*vu_=nullptr;
    uint32_t y_bytes_=0,vu_bytes_=0;
public:
    explicit VinNv21Frame(std::shared_ptr<VinLeaseStats> stats):stats_(std::move(stats)){}
    void adopt(const AX_IMG_INFO_T &image, uint64_t received) noexcept {
        image_=image;owned_=true;++stats_->acquired;
        metadata={image.tFrameInfo.stVFrame.u64SeqNum,image.tFrameInfo.stVFrame.u64PTS,received};
    }
    dart::Nv21View map() override {
        const auto &f=image_.tFrameInfo.stVFrame;
        if(!owned_ || f.enImgFormat!=AX_FORMAT_YUV420_SEMIPLANAR_VU ||
           !f.u32Width || !f.u32Height || f.u32Width%2 || f.u32Height%2 ||
           f.u32PicStride[0]<f.u32Width || f.u32PicStride[0]>16384 || f.u32Height>4096)
            throw std::runtime_error("unexpected VIN NV21 layout");
        const unsigned uv_stride=f.u32PicStride[1] ? f.u32PicStride[1] : f.u32PicStride[0];
        if(uv_stride<f.u32Width || uv_stride>16384) throw std::runtime_error("invalid UV stride");
        y_bytes_=f.u32PicStride[0]*f.u32Height;vu_bytes_=uv_stride*(f.u32Height/2);
        if(f.u32FrameSize<y_bytes_+vu_bytes_) throw std::runtime_error("truncated VIN frame");
        const uint64_t uv_phy=f.u64PhyAddr[1] ? f.u64PhyAddr[1] : f.u64PhyAddr[0]+y_bytes_;
        if(!y_) {
            y_=AX_SYS_MmapCache(f.u64PhyAddr[0],y_bytes_);
            vu_=AX_SYS_MmapCache(uv_phy,vu_bytes_);
            if(!y_ || !vu_ || AX_SYS_MinvalidateCache(f.u64PhyAddr[0],y_,y_bytes_) ||
               AX_SYS_MinvalidateCache(uv_phy,vu_,vu_bytes_)) {
                ++stats_->map_errors; throw std::runtime_error("VIN DMA map/invalidate failed");
            }
        }
        return {static_cast<const uint8_t*>(y_),static_cast<const uint8_t*>(vu_),
                static_cast<int>(f.u32Width),static_cast<int>(f.u32Height),
                static_cast<int>(f.u32PicStride[0]),static_cast<int>(uv_stride)};
    }
    ~VinNv21Frame() override {
        if(y_ && AX_SYS_Munmap(y_,y_bytes_)) ++stats_->map_errors;
        if(vu_ && AX_SYS_Munmap(vu_,vu_bytes_)) ++stats_->map_errors;
        if(owned_) {
            if(AX_VIN_ReleaseYuvFrame(0,AX_VIN_CHN_ID_MAIN,&image_)) ++stats_->release_errors;
            ++stats_->released;
        }
    }
};
