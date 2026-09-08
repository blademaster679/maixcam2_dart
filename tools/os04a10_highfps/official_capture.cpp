// Independent RAW benchmark: no display, encoding or downstream application.
#include "ax_middleware.hpp"
// Runtime settings for the exact official driver; no synthesized register table.
#define OS04A10_HFR_AVAILABLE 1
#define OS04A10_KEEP_RECEIVER 1
#define OS04A10_HFR_BAYER AX_BP_RGGB
static int OS04A10_HFR_WIDTH=1344, OS04A10_HFR_HEIGHT=760, OS04A10_HFR_FPS=60;

#include <chrono>
#include <csignal>
#include <fstream>
#include <iomanip>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <dlfcn.h>
#include <sstream>
#include <sched.h>

using namespace maix::middleware::maixcam2;
static volatile sig_atomic_t stopped = 0;
static void stop_handler(int) { stopped = 1; }
static uint64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct Record { uint64_t seq, pts, mono; uint32_t width, height, stride, size; int format;
    uint64_t get_us=0, hold_us=0, loop_gap_us=0; int cpu=-1; };
struct Health { uint64_t elapsed_us; double temperature; long mipi_errors, rss_kb; uint64_t read_us=0; };
static Health health(AX_SENSOR_REGISTER_FUNC_T *sns, uint64_t elapsed) {
    Health h{elapsed, -999, -1, -1};
    AX_U32 hi=0,lo=0;
    if (sns->pfn_sensor_read_register && !sns->pfn_sensor_read_register(0,0x4f06,&hi)
        && !sns->pfn_sensor_read_register(0,0x4f07,&lo)) {
        unsigned raw=(hi<<8)|lo;
        if (raw) {
            unsigned magnitude=raw>0xc000 ? raw-0xc000 : raw;
            h.temperature=((magnitude>>8)+(magnitude&255)/255.0)*(raw>0xc000 ? -1 : 1);
        }
    }
    std::ifstream status("/proc/ax_proc/mipi_rx/status");
    std::string line;
    while (std::getline(status,line)) {
        if (line.find("PhyStatus")!=std::string::npos && std::getline(status,line)) {
            std::istringstream values(line);
            std::string phy; long resets,errors;
            if (values>>phy>>resets>>errors) h.mipi_errors=errors;
        }
    }
    std::ifstream process("/proc/self/status");
    while (std::getline(process,line)) {
        if (line.rfind("VmRSS:",0)==0) {
            std::istringstream values(line.substr(6)); values>>h.rss_kb;
        }
    }
    return h;
}
static void save_proc(const char *suffix) {
    for (const char *name : {"sensor/info", "vin/attr", "vin/statistics", "mipi_rx/attr", "mipi_rx/status"}) {
        std::ifstream in(std::string("/proc/ax_proc/") + name);
        std::string filename(name);
        std::replace(filename.begin(), filename.end(), '/', '_');
        std::ofstream out(filename + suffix);
        if (in) out << in.rdbuf();
    }
}
static void save_registers(AX_SENSOR_REGISTER_FUNC_T *sns, const char *filename) {
    std::ofstream out(filename);
    out << "address,value,result\n";
    std::vector<unsigned> addresses = {0x0303,0x0304,0x0305,0x0306,0x0307,0x0308,0x0309,0x030c,
        0x3714,0x37cf,0x4009,0x4051,0x4601,0x4603,0x460c,0x3426,0x3427,0x3428,0x4800,0x4837,0x0322,0x0323,0x0324,0x0325,0x0328,0x032a,0x032f,0x3501,0x3502,0x384c,0x384d,0x4f06,0x4f07};
    for (unsigned a=0x3800; a<=0x3822; ++a) addresses.push_back(a);
    for (auto address : addresses) {
        AX_U32 value=0;
        const int rc = sns->pfn_sensor_read_register ? sns->pfn_sensor_read_register(0,address,&value) : -1;
        out << address << ',' << value << ',' << rc << '\n';
    }
}
int main(int argc, char **argv) {
    int seconds = 10;
    bool hfr = true;
    bool sample_raw = false;
    bool nv21 = false;
    bool burst = false;
    bool realtime = false;
    unsigned itp_depth = 0;
    int queue_depth = 0;
    const char *sensor_library = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--mode") && i+1<argc) {
            std::string mode=argv[++i];
            if (mode=="full60") { OS04A10_HFR_WIDTH=1344; OS04A10_HFR_HEIGHT=760; OS04A10_HFR_FPS=60; }
            else if (mode=="full120") { OS04A10_HFR_WIDTH=1344; OS04A10_HFR_HEIGHT=760; OS04A10_HFR_FPS=120; }
            else if (mode=="full180") { OS04A10_HFR_WIDTH=1344; OS04A10_HFR_HEIGHT=760; OS04A10_HFR_FPS=180; }
            else if (mode=="crop240") { OS04A10_HFR_WIDTH=640; OS04A10_HFR_HEIGHT=360; OS04A10_HFR_FPS=240; }
            else if (mode=="crop360") { OS04A10_HFR_WIDTH=640; OS04A10_HFR_HEIGHT=360; OS04A10_HFR_FPS=360; }
            else return 2;
        }
        else if (!std::strcmp(argv[i], "--hfr")) hfr = true;
        else if (!std::strcmp(argv[i], "--sample-raw")) sample_raw = true;
        else if (!std::strcmp(argv[i], "--sample-frame")) sample_raw = true;
        else if (!std::strcmp(argv[i], "--nv21")) nv21 = true;
        else if (!std::strcmp(argv[i], "--burst")) burst = true;
        else if (!std::strcmp(argv[i], "--realtime")) realtime = true;
        else if (!std::strcmp(argv[i], "--itp-depth") && i+1<argc) {
            std::string value=argv[++i];if(value!="1" && value!="4" && value!="8") return 2;
            itp_depth=std::stoul(value);
        }
        else if (!std::strcmp(argv[i], "--queue-depth") && i+1<argc) {
            char *end=nullptr;
            long value=std::strtol(argv[++i],&end,10);
            if (*end || value<4 || value>32) return 2;
            queue_depth=static_cast<int>(value);
        }
        else if (!std::strcmp(argv[i], "--sensor-lib") && i+1 < argc) sensor_library = argv[++i];
        else if (!std::strcmp(argv[i], "--seconds") && i+1 < argc) {
            char *end = nullptr;
            long value = std::strtol(argv[++i], &end, 10);
            if (*end || value < 1 || value > 1800) return 2;
            seconds = static_cast<int>(value);
        } else { std::cerr << "usage: capture [--hfr] [--seconds 1..1800] [--sensor-lib path] [--sample-frame] [--nv21]\n"; return 2; }
    }
    if (!queue_depth) queue_depth = nv21 ? 4 : 16;
    if (burst && (!nv21 || seconds!=1 || OS04A10_HFR_WIDTH!=640 || OS04A10_HFR_HEIGHT!=360)) {
        std::cerr << "Burst requires one second of 640x360 NV21\n"; return 2;
    }
    if (hfr && OS04A10_HFR_WIDTH > 640 && queue_depth != 4) {
        std::cerr << "Full-array probes require --queue-depth 4 with vendor-sized pools\n";
        return 2;
    }
#if !OS04A10_HFR_AVAILABLE
    if (hfr) {
        std::cerr << "360fps unavailable: vendor binning register sequence has not been supplied\n";
        return 3; // Before sensor discovery, SYS init or register access.
    }
#endif
    std::signal(SIGINT, stop_handler);
    std::signal(SIGTERM, stop_handler);
    std::vector<Record> records;
    records.reserve(static_cast<size_t>(seconds + 1) * 400);
    size_t acquire_errors = 0, release_errors = 0;
    int rc = 0;
    try {
        // Pre-touch bounded ordinary RAM before streaming; no disk writes while
        // collecting the burst. 375 packed frames consume 123.6 MiB.
        const size_t burst_frame_bytes=640u*360u*3u/2u;
        std::vector<unsigned char> burst_pixels(burst ? 375*burst_frame_bytes : 0,0);
        size_t burst_frames=0;
        if (!sensor_library) return 2;
        void *official_handle=dlopen(sensor_library,RTLD_NOW|RTLD_GLOBAL);
        using CropFn=int(*)(int,unsigned,unsigned,unsigned,unsigned,float);
        auto crop=official_handle ? reinterpret_cast<CropFn>(dlsym(official_handle,"os04a10_set_crop")) : nullptr;
        if (!crop) { std::cerr << "Official crop API unavailable\n"; return 4; }
        unsigned x=OS04A10_HFR_WIDTH==640 ? 704 : 0, y=OS04A10_HFR_WIDTH==640 ? 404 : 4;
        if (crop(0,x,y,OS04A10_HFR_WIDTH,OS04A10_HFR_HEIGHT,OS04A10_HFR_FPS)) return 4;
        if (hfr && OS04A10_HFR_WIDTH == 640) {
            // Size the dedicated process pools for the actual sensor output.
            // Leave 8 buffers beyond the dump/consumer queue for ISP work.
            for (auto &pool : gtPrivatePoolSingleOs04a10Sdr) {
                pool.nWidth=pool.nWidthStride=640; pool.nHeight=360;
                pool.nBlkCnt=queue_depth+8;
            }
            for (auto &pool : gtSysCommPoolSingleOs04a10Sdr) {
                pool.nWidth=pool.nWidthStride=640; pool.nHeight=360;
                if (pool.enCompressMode==AX_COMPRESS_MODE_NONE) pool.nBlkCnt=queue_depth+8;
            }
        } else {
            queue_depth=4; // Full-resolution baseline retains vendor pool sizing.
        }
        SYS sys(true);
        if (sys.init() != maix::err::ERR_NONE) return 4;
        VI vi;
        const auto sensor = vi.get_sensor_name();
        if (!sensor.first || sensor.second != "os04a10") {
            std::cerr << "OS04A10 required, detected " << sensor.second << '\n'; return 4;
        }
        COMMON_SYS_ARGS_T common = {}, priv = {};
        SAMPLE_VIN_PARAM_T param = {};
        param.eSysCase = SAMPLE_VIN_SINGLE_OS04A10;
        param.eSysMode = COMMON_VIN_SENSOR;
        param.eHdrMode = AX_SNS_LINEAR_MODE;
        param.eLoadRawNode = LOAD_RAW_IFE;
        param.bAiispEnable = AX_FALSE;
        param.bSensorCrop=AX_TRUE;
        param.nSensorCropX=x; param.nSensorCropY=y;
        param.nSensorCropW=param.nSensorWidth=OS04A10_HFR_WIDTH;
        param.nSensorCropH=param.nSensorHeight=OS04A10_HFR_HEIGHT;
        param.nSensorFps=OS04A10_HFR_FPS;
        if (vi.config_sample_case(&param, &common, &priv)) return 4;
        auto &module = AxModuleParam::getInstance();
        module.lock(AX_MOD_VI);
        auto *vp = static_cast<ax_vi_mod_t *>(module.get_param(AX_MOD_VI));
        auto &cam = vp->cams[0];
        if (sensor_library) {
            // Keep the handle alive until process exit; callbacks outlive this block.
            void *handle = dlopen(sensor_library, RTLD_NOW | RTLD_LOCAL);
            auto *object = handle ? static_cast<AX_SENSOR_REGISTER_FUNC_T *>(dlsym(handle, "gSnsos04a10Obj")) : nullptr;
            if (!object) {
                module.unlock(AX_MOD_VI);
                std::cerr << "Cannot load experimental sensor object: " << dlerror() << '\n';
                return 4;
            }
            cam.ptSnsHdl[0] = object;
            std::cout << "sensor_object=" << sensor_library << '\n';
        }
        cam.tPipeAttr[0].tCompressInfo = {AX_COMPRESS_MODE_NONE, 0};
        if (nv21) {
            cam.tChnAttr[0].eImgFormat = AX_FORMAT_YUV420_SEMIPLANAR_VU;
            cam.tChnAttr[0].tCompressInfo = {AX_COMPRESS_MODE_NONE, 0};
            cam.tChnAttr[0].nDepth = queue_depth;
        }
#if OS04A10_HFR_AVAILABLE
        if (hfr) {
            cam.tSnsAttr.nWidth = OS04A10_HFR_WIDTH; cam.tSnsAttr.nHeight = OS04A10_HFR_HEIGHT;
            cam.tSnsAttr.fFrameRate = OS04A10_HFR_FPS; cam.tSnsAttr.eBayerPattern = OS04A10_HFR_BAYER;
#if !OS04A10_KEEP_RECEIVER
            cam.tMipiAttr.nDataRate = OS04A10_HFR_LANE_MBPS;
#endif
            for (auto &r : cam.tDevAttr.tDevImgRgn) r = {0, 0, OS04A10_HFR_WIDTH, OS04A10_HFR_HEIGHT};
            cam.tDevAttr.eBayerPattern = OS04A10_HFR_BAYER;
            cam.tPipeAttr[0].tPipeImgRgn = {0, 0, OS04A10_HFR_WIDTH, OS04A10_HFR_HEIGHT};
            cam.tPipeAttr[0].nWidthStride = OS04A10_HFR_WIDTH;
            cam.tPipeAttr[0].eBayerPattern = OS04A10_HFR_BAYER;
            cam.tChnAttr[0].nWidth = OS04A10_HFR_WIDTH; cam.tChnAttr[0].nHeight = OS04A10_HFR_HEIGHT;
            cam.tChnAttr[0].nWidthStride = OS04A10_HFR_WIDTH;
        }
#endif
        const unsigned expected_w = cam.tSnsAttr.nWidth, expected_h = cam.tSnsAttr.nHeight;
        module.unlock(AX_MOD_VI);
        if (vi.init() != maix::err::ERR_NONE) return 4;
        if(itp_depth) {
            AX_U32 before=0,after=0;
            const int get_before=AX_VIN_GetPipeSourceDepth(0,AX_VIN_FRAME_SOURCE_ID_ITP,&before);
            const int set=AX_VIN_SetPipeSourceDepth(0,AX_VIN_FRAME_SOURCE_ID_ITP,itp_depth);
            const int get_after=AX_VIN_GetPipeSourceDepth(0,AX_VIN_FRAME_SOURCE_ID_ITP,&after);
            std::ofstream("itp_depth.json")<<"{\"before\":"<<before<<",\"after\":"<<after<<",\"get_before\":"<<get_before<<",\"set\":"<<set<<",\"get_after\":"<<get_after<<"}\n";
            if(get_before || set || get_after || after!=itp_depth)throw std::runtime_error("ITP source depth configuration rejected");
        }
        Dl_info selected = {};
        if (!cam.ptSnsHdl[0] || !dladdr(reinterpret_cast<void *>(cam.ptSnsHdl[0]->pfn_sensor_chipid), &selected)
            || !selected.dli_fname || (sensor_library && std::strcmp(sensor_library, selected.dli_fname))) {
            std::cerr << "Selected sensor callback library does not match request\n"; return 4;
        }
        std::cout << "active_sensor_callback=" << selected.dli_fname << '\n';
        AX_S32 chip_id = 0;
        if (!cam.ptSnsHdl[0] || !cam.ptSnsHdl[0]->pfn_sensor_chipid ||
            cam.ptSnsHdl[0]->pfn_sensor_chipid(0, &chip_id) != 0 || chip_id != 0x530441) {
            std::cerr << "Chip-ID verification failed\n"; return 4;
        }
        std::cout << "chip_id=0x" << std::hex << chip_id << std::dec
                  << " expected_raw=" << expected_w << 'x' << expected_h << '\n';
        std::ifstream maps("/proc/self/maps");
        std::ofstream("loaded_maps.txt") << maps.rdbuf();
        AX_VIN_DUMP_ATTR_T dump = {};
        dump.bEnable = AX_TRUE; dump.nDepth = queue_depth;
        if (!nv21 && AX_VIN_SetPipeDumpAttr(0, AX_VIN_PIPE_DUMP_NODE_IFE, AX_VIN_DUMP_QUEUE_TYPE_DEV, &dump)) return 4;
        save_proc("_before.txt");
        save_registers(cam.ptSnsHdl[0], "registers_before.csv");
        // Only this acquisition thread changes policy; vendor threads were
        // created already. GetFrame blocks and the benchmark is time-bounded.
        if(realtime) { sched_param priority={};priority.sched_priority=10;
            if(sched_setscheduler(0,SCHED_FIFO,&priority)) throw std::runtime_error("Cannot set acquisition SCHED_FIFO"); }
        std::ofstream("scheduler.txt") << "policy=" << sched_getscheduler(0) << " realtime_requested=" << realtime << '\n';
        const auto start = now_us();
        const auto measurement_start = start + 2000000;
        const auto end = measurement_start + static_cast<uint64_t>(seconds) * 1000000;
        unsigned consecutive_errors = 0;
        bool sampled = false;
        std::vector<Health> health_records;
        health_records.reserve(seconds+2);
        auto next_health = measurement_start;
        uint64_t previous_release=0;
        while (!stopped && !maix::app::need_exit() && now_us() < end) {
            AX_IMG_INFO_T img = {};
            const auto get_start=now_us();
            const auto result = nv21 ? AX_VIN_GetYuvFrame(0, AX_VIN_CHN_ID_MAIN, &img, 200) :
                AX_VIN_GetRawFrame(0, AX_VIN_PIPE_DUMP_NODE_IFE, AX_SNS_HDR_FRAME_L, &img, 200);
            if (result) {
                ++acquire_errors;
                if (++consecutive_errors >= 5) { rc = 5; break; }
                continue;
            }
            consecutive_errors = 0;
            const auto stamp = now_us();
            const auto &f = img.tFrameInfo.stVFrame;
            Record record{f.u64SeqNum, f.u64PTS, stamp, f.u32Width, f.u32Height,
                                f.u32PicStride[0], f.u32FrameSize, static_cast<int>(f.enImgFormat)};
            record.get_us=stamp-get_start;record.loop_gap_us=previous_release ? get_start-previous_release : 0;
            record.cpu=sched_getcpu();
            // One diagnostic image during warmup; no storage I/O in measurement.
            if (sample_raw && !sampled && stamp >= start + 1000000 && stamp < measurement_start) {
                if (f.u32FrameSize && f.u32FrameSize <= 16*1024*1024) {
                    void *pixels = AX_SYS_Mmap(f.u64PhyAddr[0], f.u32FrameSize);
                    if (pixels) {
                        std::ofstream raw(nv21 ? "sample.nv21" : "sample.raw", std::ios::binary);
                        raw.write(static_cast<const char *>(pixels), f.u32FrameSize);
                        sampled = raw.good();
                        AX_SYS_Munmap(pixels, f.u32FrameSize);
                        std::ofstream meta("sample.json");
                        meta << "{\"width\":" << record.width << ",\"height\":" << record.height
                             << ",\"stride\":" << record.stride << ",\"frame_bytes\":" << record.size
                             << ",\"format\":" << record.format << ",\"sequence\":" << record.seq << "}\n";
                    }
                }
            }
            if (burst && stamp>=measurement_start && stamp<end) {
                if (f.u32Width!=640 || f.u32Height!=360 || f.u32PicStride[0]<640 ||
                    f.enImgFormat!=AX_FORMAT_YUV420_SEMIPLANAR_VU ||
                    f.u32FrameSize<f.u32PicStride[0]*540 || burst_frames>=375) rc=6;
                else {
                    void *pixels=AX_SYS_MmapCache(f.u64PhyAddr[0],f.u32FrameSize);
                    if (!pixels) rc=11;
                    else {
                        // VIN writes via DMA; invalidate before reading through CPU cache.
                        if(AX_SYS_MinvalidateCache(f.u64PhyAddr[0],pixels,f.u32FrameSize)) rc=11;
                        auto *dest=burst_pixels.data()+burst_frames*burst_frame_bytes;
                        for(unsigned row=0;row<540;++row)
                            std::memcpy(dest+row*640,static_cast<unsigned char *>(pixels)+row*f.u32PicStride[0],640);
                        if (AX_SYS_Munmap(pixels,f.u32FrameSize)) rc=11;
                        ++burst_frames;
                    }
                }
            }
            const auto released = nv21 ? AX_VIN_ReleaseYuvFrame(0, AX_VIN_CHN_ID_MAIN, &img) :
                AX_VIN_ReleaseRawFrame(0, AX_VIN_PIPE_DUMP_NODE_IFE, AX_SNS_HDR_FRAME_L, &img);
            previous_release=now_us();record.hold_us=previous_release-stamp;
            if (released) { ++release_errors; rc = 5; break; }
            if (rc) break;
            if (record.width != expected_w || record.height != expected_h) { rc = 6; break; }
            if (nv21 && record.format != AX_FORMAT_YUV420_SEMIPLANAR_VU) { rc = 6; break; }
            if (stamp >= measurement_start && stamp < end) {
                if (records.size() == records.capacity()) { rc = 7; break; }
                if (!records.empty() && (record.seq != records.back().seq+1 || record.pts<=records.back().pts)) {
                    records.push_back(record); rc=10; break;
                }
                records.push_back(record);
                if (stamp>=next_health) {
                    const auto health_start=now_us();
                    auto state=health(cam.ptSnsHdl[0],stamp-measurement_start);
                    state.read_us=now_us()-health_start;
                    health_records.push_back(state);
                    next_health=stamp+1000000;
                    // Datasheet operating junction ceiling is 85 C; stop with 5 C headroom.
                    if (state.temperature>=80 || state.mipi_errors>0) { rc=9; break; }
                    if (health_records.size()%10==0) {
                        std::ofstream progress("progress.json");
                        progress << "{\"elapsed_s\":" << state.elapsed_us/1e6
                            << ",\"frames\":" << records.size() << ",\"temperature_c\":" << state.temperature
                            << ",\"mipi_errors\":" << state.mipi_errors << ",\"rss_kb\":" << state.rss_kb << "}\n";
                    }
                }
            }
        }
        if(realtime) { sched_param priority={};if(sched_setscheduler(0,SCHED_OTHER,&priority)) rc=11; }
        save_proc("_after.txt");
        save_registers(cam.ptSnsHdl[0], "registers_after.csv");
        dump.bEnable = AX_FALSE;
        if (!nv21 && AX_VIN_SetPipeDumpAttr(0, AX_VIN_PIPE_DUMP_NODE_IFE, AX_VIN_DUMP_QUEUE_TYPE_DEV, &dump)) rc = 5;
        if (vi.deinit() != maix::err::ERR_NONE) rc = 5;
        if (burst) {
            std::ofstream video("record.nv21",std::ios::binary);
            video.write(reinterpret_cast<const char *>(burst_pixels.data()),burst_frames*burst_frame_bytes);
            video.flush();
            if(!video || burst_frames!=records.size()) rc=11;
        }
        std::ofstream csv("frames.csv");
        csv << "sequence,pts_raw,monotonic_us,width,height,stride,frame_bytes,format,get_us,hold_us,loop_gap_us,cpu\n";
        for (const auto &r : records)
            csv << r.seq << ',' << r.pts << ',' << r.mono << ',' << r.width << ',' << r.height
                << ',' << r.stride << ',' << r.size << ',' << r.format << ',' << r.get_us << ',' << r.hold_us << ',' << r.loop_gap_us << ',' << r.cpu << '\n';
        std::ofstream health_csv("health.csv");
        health_csv << "elapsed_us,temperature_c,mipi_errors,rss_kb,read_us\n";
        for (const auto &h : health_records)
            health_csv << h.elapsed_us << ',' << h.temperature << ',' << h.mipi_errors << ',' << h.rss_kb << ',' << h.read_us << '\n';
        csv.flush(); health_csv.flush();
        if (!csv || !health_csv) rc=11;
        if (records.size() < 2 && !rc) rc = 5;
        const double fps = records.size() > 1 ?
            (records.size()-1) * 1000000.0 / (records.back().mono - records.front().mono) : 0;
        std::ofstream summary("capture.json");
        summary << "{\"requested_fps\":" << (hfr ? OS04A10_HFR_FPS : 30)
                << ",\"measurement\":\"" << (nv21 ? "VIN CH0 NV21" : "VIN IFE RAW") << "\",\"frames\":" << records.size()
                << ",\"monotonic_fps\":" << std::setprecision(10) << fps
                << ",\"acquire_errors\":" << acquire_errors << ",\"release_errors\":" << release_errors
                << ",\"queue_depth\":" << queue_depth
                << ",\"realtime\":" << (realtime ? "true" : "false")
                << ",\"itp_depth_request\":" << itp_depth
                << ",\"burst_recording\":" << (burst ? "true" : "false")
                << ",\"burst_frames\":" << burst_frames
                << ",\"burst_frame_bytes\":" << (burst ? burst_frame_bytes : 0)
                << ",\"sample_saved\":" << (sampled ? "true" : "false")
                << ",\"exit_code\":" << rc << ",\"interrupted\":" << (stopped ? "true" : "false") << "}\n";
        std::cout << (nv21 ? "nv21_frames=" : "raw_frames=") << records.size() << " monotonic_fps=" << fps << " result=" << rc << '\n';
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 8; }
    return rc;
}
