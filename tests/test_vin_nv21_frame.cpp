#include "../tools/os04a10_highfps/vin_nv21_frame.hpp"
#include <vector>
#include <iostream>
#define CHECK(x) do {if(!(x)) throw std::runtime_error(#x);} while(0)
static std::vector<unsigned char> pixels(96);
static int mapped=0,unmapped=0,invalidated=0,released=0,fail_map=0;
static bool fail_invalidate=false,fail_release=false,fail_unmap=false;
extern "C" {
AX_VOID *AX_SYS_MmapCache(AX_U64 address,AX_U32) {
    ++mapped;if(mapped==fail_map) return nullptr;
    return pixels.data()+(address-1000);
}
AX_S32 AX_SYS_MinvalidateCache(AX_U64,AX_VOID*,AX_U32) {++invalidated;return fail_invalidate?-1:0;}
AX_S32 AX_SYS_Munmap(AX_VOID*,AX_U32) {++unmapped;return fail_unmap?-1:0;}
AX_S32 AX_VIN_ReleaseYuvFrame(AX_U8,AX_VIN_CHN_ID_E,const AX_IMG_INFO_T*) {
    CHECK(unmapped==mapped-(fail_map?1:0));++released;return fail_release?-1:0;
}
}
int main() {
    for(int mode=0;mode<5;++mode) {
        mapped=unmapped=invalidated=released=0;
        fail_map=mode==1?2:0;fail_invalidate=mode==2;fail_release=mode==3;fail_unmap=mode==4;
        auto stats=std::make_shared<VinLeaseStats>();
        {
            AX_IMG_INFO_T image{};auto &f=image.tFrameInfo.stVFrame;
            f.enImgFormat=AX_FORMAT_YUV420_SEMIPLANAR_VU;f.u32Width=8;f.u32Height=8;
            f.u32PicStride[0]=f.u32PicStride[1]=8;f.u32FrameSize=96;
            f.u64PhyAddr[0]=1000;f.u64PhyAddr[1]=1064;
            VinNv21Frame lease(stats);lease.adopt(image,100);
            bool threw=false;
            try {auto v=lease.map();CHECK(v.y==pixels.data() && v.vu==pixels.data()+64);CHECK(invalidated==2);}
            catch(const std::runtime_error&) {threw=true;}
            CHECK(threw==(mode==1 || mode==2));CHECK(released==0);
        }
        CHECK(stats->acquired==1 && stats->released==1 && released==1);
        CHECK((stats->release_errors!=0)==(mode==3));
        CHECK((stats->map_errors!=0)==(mode==1 || mode==2 || mode==4));
    }
    std::cout<<"DMA invalidate/unmap/release ordering and injected failures passed\n";
}
