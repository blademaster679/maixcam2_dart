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

using namespace maix::middleware::maixcam2;
static volatile sig_atomic_t stopped = 0;
static void stop_handler(int) { stopped = 1; }
static uint64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct Record { uint64_t seq, pts, mono; uint32_t width, height, stride, size; int format; };
struct Health { uint64_t elapsed_us; double temperature; long mipi_errors, rss_kb; };
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
#include "maix_basic.hpp"
#include "maix_camera.hpp"
#include "maix_pipeline.hpp"
#include "maix_video.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <execinfo.h>
#include <unistd.h>
static void crash_trace(int sig) { void *stack[32]; int n=backtrace(stack,32); backtrace_symbols_fd(stack,n,2); _exit(128+sig); }
// Camera/IVPS measurement and bounded asynchronous recording are separate from RAW validation.
int main(int argc,char **argv) {
    setvbuf(stdout,nullptr,_IONBF,0); std::signal(SIGSEGV,crash_trace);
    maix::util::init_before_main();
    struct ExitHooks { ~ExitHooks() { maix::util::do_exit_function(); } } exit_hooks;
    std::cerr<<"stage=main\n";
    int seconds=10, width=1344,height=760,fps=180, camera_depth=4;
    bool record_video=false, exercise=false, observe_gaps=false, no_health=false;
    const char *library=nullptr;
    for(int i=1;i<argc;++i) {
        std::string arg=argv[i];
        if(arg=="--seconds" && i+1<argc) seconds=std::atoi(argv[++i]);
        else if(arg=="--sensor-lib" && i+1<argc) library=argv[++i];
        else if(arg=="--mode" && i+1<argc) {
            std::string mode=argv[++i];
            if(mode=="full60") fps=60;
            else if(mode=="full120") fps=120;
            else if(mode=="full180") fps=180;
            else if(mode=="crop240") {width=640;height=360;fps=240;}
            else if(mode=="crop360") {width=640;height=360;fps=360;}
            else return 2;
        } else if(arg=="--record") record_video=true;
        else if(arg=="--exercise-switch") exercise=true;
        else if(arg=="--observe-gaps") observe_gaps=true;
        else if(arg=="--no-health") no_health=true;
        else if(arg=="--queue-depth" && i+1<argc) camera_depth=std::atoi(argv[++i]);
        else if(arg!="--hfr" && arg!="--sample-frame" && arg!="--nv21") return 2;
    }
    if(exercise) {width=640;height=360;} // Small application output retains full-FOV sensor input until ROI is explicit.
    if(!library || seconds<1 || seconds>1800 || (record_video && seconds>30)) return 2;
    std::signal(SIGTERM,stop_handler);std::signal(SIGINT,stop_handler);
    int rc=0;
    try {
        std::cerr<<"stage=load_sensor\n";
        void *handle=dlopen(library,RTLD_NOW|RTLD_GLOBAL);
        auto *sns=handle ? reinterpret_cast<AX_SENSOR_REGISTER_FUNC_T *>(dlsym(handle,"gSnsos04a10Obj")) : nullptr;
        if(!sns) return 4;
        // Initialize encoder before Camera, matching the official high-fps recording example.
        std::unique_ptr<maix::video::Encoder> encoder;
        if(record_video) encoder.reset(new maix::video::Encoder("record.h264",width,height,
            maix::image::FMT_YVU420SP,maix::video::VIDEO_H264,std::min(fps,180),fps,12000000,1000,false,true));
        std::cerr<<"stage=construct_camera\n";
        maix::camera::Camera cam(width,height,maix::image::FMT_YVU420SP,nullptr,fps,camera_depth,false);
        if(fps>180) maix::err::check_raise(cam.set_windowing({640,360}));
        std::cerr<<"stage=open_camera\n";
        maix::err::check_raise(cam.open());
        std::cerr<<"stage=opened\n";
        auto &module=AxModuleParam::getInstance();module.lock(AX_MOD_VI);
        auto *vp=static_cast<ax_vi_mod_t *>(module.get_param(AX_MOD_VI));
        auto *active=vp->cams[0].ptSnsHdl[0];module.unlock(AX_MOD_VI);
        Dl_info location={};
        if(active!=sns || !dladdr(reinterpret_cast<void *>(active->pfn_sensor_chipid),&location) ||
           !location.dli_fname || std::strcmp(location.dli_fname,library)) return 4;
        AX_S32 chip=0;if(sns->pfn_sensor_chipid(0,&chip)||chip!=0x530441) return 4;
        std::cout << "active_sensor_callback=" << location.dli_fname << " chip=0x" << std::hex <<chip<<std::dec<<'\n';
        std::ifstream maps("/proc/self/maps"); std::ofstream("loaded_maps.txt")<<maps.rdbuf();
        if(exercise) {
            std::ofstream events("api_checks.csv"); events<<"action,return_code,opened\n";
            auto check=[&](const char *name,maix::err::Err e) { events<<name<<','<<static_cast<int>(e)<<','<<cam.is_opened()<<'\n';events.flush();maix::err::check_raise(e,name); };
            if(fps!=180) return 2;
            check("fps60",cam.set_fps(60));check("fps120",cam.set_fps(120));check("fps180",cam.set_fps(180));
            auto denied=cam.set_fps(360);events<<"reject_full360,"<<static_cast<int>(denied)<<','<<cam.is_opened()<<'\n';
            if(denied==maix::err::ERR_NONE || !cam.is_opened()) return 12;
            check("crop640",cam.set_windowing({640,360}));check("crop360fps",cam.set_fps(360));
            check("return180",cam.set_fps(180));check("restore_full",cam.set_windowing({0,4,1344,760}));
            for(int n=0;n<3;++n) {cam.close();check("reopen",cam.open());std::unique_ptr<maix::image::Image> img(cam.read(true,1000));if(!img)return 12;}
        }
        AX_IVPS_PIPELINE_ATTR_T pipe_attr={};
        module.lock(AX_MOD_VI);int group=static_cast<ax_vi_mod_t *>(module.get_param(AX_MOD_VI))->nGrpId;module.unlock(AX_MOD_VI);
        int pipe_rc=AX_IVPS_GetPipelineAttr(group,&pipe_attr);
        std::ofstream ivps("ivps_config.csv");ivps<<"group,frc_mode,channel,filter,engaged,source_fps,destination_fps,return_code\n";
        for(int c=0;c<=AX_IVPS_MAX_OUTCHN_NUM;++c)for(int f=0;f<AX_IVPS_MAX_FILTER_NUM_PER_OUTCHN;++f) {
            const auto &a=pipe_attr.tFilter[c][f];
            ivps<<group<<','<<int(pipe_attr.eFRCMode)<<','<<c<<','<<f<<','<<a.bEngage<<','<<a.tFRC.fSrcFrameRate<<','<<a.tFRC.fDstFrameRate<<','<<pipe_rc<<'\n';
        }
        ivps.close();
        save_proc("_before.txt");save_registers(sns,"registers_before.csv");
        const auto warm_start=now_us();
        bool sampled=false;
        while(now_us()-warm_start<3000000 && !stopped) {
            std::unique_ptr<maix::pipeline::Frame> f(cam.pop(1000));
            if(!f) return 5;
            if(!sampled && now_us()-warm_start>2000000) {
                std::unique_ptr<maix::image::Image> img(f->to_image());
                std::ofstream file("sample.nv21",std::ios::binary);file.write(static_cast<const char *>(img->data()),img->data_size());
                std::ofstream("sample.json")<<"{\"width\":"<<img->width()<<",\"height\":"<<img->height()<<",\"stride\":"<<img->width()<<",\"frame_bytes\":"<<img->data_size()<<",\"format\":4}\n";
                sampled=file.good();
            }
        }
        struct Pending {std::unique_ptr<maix::image::Image> img;Record meta;};
        std::deque<Pending> queue;std::mutex mutex;std::condition_variable ready;
        bool done=false;std::atomic<bool> failed(false);std::atomic<size_t> encoded(0);size_t queue_dropped=0;
        std::thread worker;
        if(record_video) worker=std::thread([&] {
            try {
                std::ofstream index("encoded_frames.csv");index<<"index,sequence,pts_raw,monotonic_us\n";
                for(;;) {
                    Pending p;
                    {std::unique_lock<std::mutex> lock(mutex);ready.wait(lock,[&]{return done||!queue.empty();});if(queue.empty()&&done)break;p=std::move(queue.front());queue.pop_front();}
                    std::unique_ptr<maix::video::Frame> output(encoder->encode(p.img.get()));
                    index<<encoded<<','<<p.meta.seq<<','<<p.meta.pts<<','<<p.meta.mono<<'\n';++encoded;
                }
                if(!index) failed=true;
            } catch(const std::exception &e) {std::cerr<<"encoder: "<<e.what()<<'\n';failed=true;}
        });
        std::vector<Record> records;records.reserve((seconds+1)*400);
        std::vector<Health> states;states.reserve(seconds+1);
        size_t errors=0,gaps=0,duplicates=0,backwards=0;
        const auto begin=now_us(),end=begin+uint64_t(seconds)*1000000;auto next_health=begin;
        try {
            while(!stopped && !maix::app::need_exit() && !failed && now_us()<end) {
                std::unique_ptr<maix::pipeline::Frame> frame(cam.pop(200));
                if(!frame) {++errors;rc=5;break;}
                auto *native=static_cast<maix::middleware::maixcam2::Frame *>(frame->frame());
                AX_VIDEO_FRAME_T v={};maix::err::check_raise(native->get_video_frame(&v));
                Record m{v.u64SeqNum,v.u64PTS,now_us(),v.u32Width,v.u32Height,v.u32PicStride[0],v.u32FrameSize,int(v.enImgFormat)};
                if(m.width!=unsigned(width)||m.height!=unsigned(height)) {rc=6;break;}
                if(!records.empty()) {auto &prev=records.back();if(m.seq==prev.seq)++duplicates;else if(m.seq<prev.seq)++backwards;else if(m.seq>prev.seq+1)gaps+=m.seq-prev.seq-1;}
                if(records.size()==records.capacity()) {rc=7;break;}
                records.push_back(m);
                if(record_video) {
                    std::unique_ptr<maix::image::Image> img(frame->to_image());
                    Pending p{std::unique_ptr<maix::image::Image>(new maix::image::Image(width,height,maix::image::FMT_YVU420SP,
                        static_cast<uint8_t *>(img->data()),img->data_size(),true)),m};
                    img.reset();frame.reset();
                    {std::lock_guard<std::mutex> lock(mutex);if(queue.size()>=32){queue.pop_front();++queue_dropped;}queue.push_back(std::move(p));}ready.notify_one();
                } else frame.reset();
                if(!no_health && m.mono>=next_health) {
                    auto h=health(sns,m.mono-begin);states.push_back(h);next_health=m.mono+1000000;
                    if(h.temperature>=80||h.mipi_errors>0) {rc=9;break;}
                    if(states.size()%10==0) std::ofstream("progress.json")<<"{\"elapsed_s\":"<<h.elapsed_us/1e6<<",\"frames\":"<<records.size()<<",\"temperature_c\":"<<h.temperature<<",\"mipi_errors\":"<<h.mipi_errors<<",\"rss_kb\":"<<h.rss_kb<<"}\n";
                }
                if(!record_video && ((!observe_gaps && gaps)||duplicates||backwards)) {rc=10;break;}
            }
        } catch(const std::exception &e) {std::cerr<<"capture: "<<e.what()<<'\n';rc=8;}
        {std::lock_guard<std::mutex> lock(mutex);done=true;}ready.notify_all();if(worker.joinable())worker.join();
        encoder.reset();if(failed)rc=8;
        if(no_health) states.push_back(health(sns,now_us()-begin));
        save_proc("_after.txt");save_registers(sns,"registers_after.csv");cam.close();
        std::ofstream csv("frames.csv");csv<<"sequence,pts_raw,monotonic_us,width,height,stride,frame_bytes,format\n";
        for(auto &m:records)csv<<m.seq<<','<<m.pts<<','<<m.mono<<','<<m.width<<','<<m.height<<','<<m.stride<<','<<m.size<<','<<m.format<<'\n';
        std::ofstream hs("health.csv");hs<<"elapsed_us,temperature_c,mipi_errors,rss_kb\n";
        for(auto &h:states)hs<<h.elapsed_us<<','<<h.temperature<<','<<h.mipi_errors<<','<<h.rss_kb<<'\n';
        double actual=records.size()>1 ? (records.size()-1)*1e6/(records.back().mono-records.front().mono):0;
        if(records.size()<2&&!rc)rc=5;
        if(!rc && !record_video && gaps)rc=10;
        std::ofstream json("capture.json");json<<"{\"requested_fps\":"<<fps<<",\"measurement\":\"Camera::pop IVPS NV21\",\"frames\":"<<records.size()<<",\"monotonic_fps\":"<<std::setprecision(10)<<actual
            <<",\"acquire_errors\":"<<errors<<",\"release_errors\":null,\"release_note\":\"RAII release return unavailable\",\"sequence_gaps\":"<<gaps<<",\"duplicates\":"<<duplicates<<",\"backwards\":"<<backwards
            <<",\"recording\":"<<(record_video?"true":"false")<<",\"encoded_submissions\":"<<encoded<<",\"encoder_queue_dropped\":"<<queue_dropped<<",\"exit_code\":"<<rc<<",\"interrupted\":"<<(stopped?"true":"false")<<"}\n";
        std::cout<<"frames="<<records.size()<<" fps="<<actual<<" gaps="<<gaps<<" encoded="<<encoded<<" queue_dropped="<<queue_dropped<<" result="<<rc<<'\n';
    }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 8;}
    return rc;
}
