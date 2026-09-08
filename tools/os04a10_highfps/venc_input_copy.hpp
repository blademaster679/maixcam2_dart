#pragma once
// Diagnostic user-pool input following MSP sample/venc/common/sample_pool.c.
// Copy only pixels and documented frame attributes, retaining original sequence/PTS.
#include <cstring>
#include <stdexcept>
#include "ax_sys_api.h"
class VencInputCopy {
    AX_POOL pool=AX_INVALID_POOLID;
public:
    explicit VencInputCopy() {
        AX_POOL_CONFIG_T config={};config.MetaSize=4096;config.BlkSize=640*360*3/2;config.BlkCnt=16;
        config.CacheMode=AX_POOL_CACHE_MODE_CACHED;
        std::strcpy(reinterpret_cast<char *>(config.PartitionName),"anonymous");
        std::strcpy(reinterpret_cast<char *>(config.PoolName),"os04a10_venc_input");
        pool=AX_POOL_CreatePool(&config);
        if(pool==AX_INVALID_POOLID)throw std::runtime_error("VENC input pool creation failed");
    }
    int copy(const AX_VIDEO_FRAME_INFO_T &input,AX_VIDEO_FRAME_INFO_T &output) {
        const auto &f=input.stVFrame;
        if(f.u32Width!=640 || f.u32Height!=360 || f.enImgFormat!=AX_FORMAT_YUV420_SEMIPLANAR ||
            f.u32PicStride[0]<640 || f.u32FrameSize<f.u32PicStride[0]*540)return -1;
        const auto block=AX_POOL_GetBlock(pool,640*360*3/2,nullptr);
        if(block==AX_INVALID_BLOCKID)return -2;
        auto *dst=AX_POOL_GetBlockVirAddr(block);
        auto *src=AX_SYS_MmapCache(f.u64PhyAddr[0],f.u32FrameSize);
        int result=0;
        if(!src || !dst) result=-3;
        else if(AX_SYS_MinvalidateCache(f.u64PhyAddr[0],src,f.u32FrameSize))result=-4;
        else {
            for(unsigned row=0;row<540;++row)std::memcpy(static_cast<char *>(dst)+row*640,static_cast<char *>(src)+row*f.u32PicStride[0],640);
            if(AX_SYS_MflushCache(AX_POOL_Handle2PhysAddr(block),dst,640*360*3/2))result=-5;
        }
        if(src && AX_SYS_Munmap(src,f.u32FrameSize))result=-6;
        if(result) {AX_POOL_ReleaseBlock(block);return result;}
        output={};auto &o=output.stVFrame;o.u32Width=640;o.u32Height=360;
        o.enImgFormat=f.enImgFormat;o.enVscanFormat=f.enVscanFormat;o.stDynamicRange=f.stDynamicRange;o.stColorGamut=f.stColorGamut;
        o.u32PicStride[0]=o.u32PicStride[1]=640;o.u32FrameSize=640*360*3/2;
        o.u32BlkId[0]=block;o.u64PhyAddr[0]=AX_POOL_Handle2PhysAddr(block);o.u64PhyAddr[1]=o.u64PhyAddr[0]+640*360;
        o.u64VirAddr[0]=reinterpret_cast<AX_U64>(dst);o.u64VirAddr[1]=o.u64VirAddr[0]+640*360;
        o.u64SeqNum=f.u64SeqNum;o.u64PTS=f.u64PTS;return 0;
    }
    int close() {if(pool==AX_INVALID_POOLID)return 0;const auto id=pool;pool=AX_INVALID_POOLID;return AX_POOL_DestroyPool(id);}
    ~VencInputCopy(){close();}
};
