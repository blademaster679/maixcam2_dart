#include "dart/nv21_pipeline.hpp"
#include "dart/async_log.hpp"
#include "dart/target_json.hpp"
#include "dart/visual_motion.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>

namespace dart {
namespace {
uint64_t monotonic_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
uint8_t clip(int v) { return static_cast<uint8_t>(std::clamp(v,0,255)); }
void rgb(int y, int v, int u, uint8_t *p) {
    const int c=298*(y-16); u-=128; v-=128;
    p[0]=clip((c+409*v+128)>>8); p[1]=clip((c-100*u-208*v+128)>>8);
    p[2]=clip((c+516*u+128)>>8);
}
}
void Nv21View::validate() const {
    if (!y || !vu || width<=0 || height<=0 || width%2 || height%2 ||
        y_stride<width || vu_stride<width) throw std::invalid_argument("invalid NV21 planes");
}
CandidateRoi source_roi(float x,float y,int size,int w,int h) {
    if(w<=0 || h<=0 || size<=0 || !std::isfinite(x) || !std::isfinite(y))
        throw std::invalid_argument("invalid ROI dimensions/center");
    const int rw=std::min(size,w), rh=std::min(size,h);
    return {static_cast<int>(std::clamp(std::floor(x-rw/2),0.0F,static_cast<float>(w-rw))),
            static_cast<int>(std::clamp(std::floor(y-rh/2),0.0F,static_cast<float>(h-rh))),rw,rh};
}
std::vector<Point2f> nv21_green_proposals(const Nv21View &v) {
    v.validate();
    // 2x2 chroma grid, max luma within each cell protects tiny bright lamps.
    // Bounded tile maxima: no full-frame RGB allocation or connected-component map.
    struct Peak { int score=0,x=0,y=0; };
    const int cols=(v.width+63)/64, rows=(v.height+63)/64;
    std::vector<Peak> tiles(cols*rows);
    for(int y=0;y<v.height;y+=2) for(int x=0;x<v.width;x+=2) {
        const auto *uv=v.vu+(y/2)*v.vu_stride+x;
        if(uv[0]>=128 || uv[1]>=128) continue;
        int best_y=0,bx=x,by=y;
        for(int dy=0;dy<2;++dy) for(int dx=0;dx<2;++dx) {
            int l=v.y[(y+dy)*v.y_stride+x+dx];
            if(l>best_y){best_y=l;bx=x+dx;by=y+dy;}
        }
        uint8_t color[3];rgb(best_y,uv[0],uv[1],color);
        const int response=2*color[1]-color[0]-color[2];
        if(color[1]<40 || response<35 || 100*color[1]<42*(color[0]+color[1]+color[2])) continue;
        auto &p=tiles[(y/64)*cols+x/64];
        if(response>p.score) p={response,bx,by};
    }
    std::sort(tiles.begin(),tiles.end(),[](const Peak&a,const Peak&b){return a.score>b.score;});
    std::vector<Point2f> result;
    for(const auto &p:tiles) {
        if(!p.score || result.size()==5) break;
        bool close=false; for(const auto&q:result) if(std::hypot(q.x-p.x,q.y-p.y)<48) close=true;
        if(!close) result.push_back({static_cast<float>(p.x),static_cast<float>(p.y),true});
    }
    return result;
}
std::unique_ptr<maix::image::Image> nv21_rgb_region(const Nv21View &v,const CandidateRoi &r,int step) {
    v.validate();
    if(step<=0 || r.x<0 || r.y<0 || r.width<=0 || r.height<=0 ||
       r.x>v.width-r.width || r.y>v.height-r.height || r.width%step || r.height%step)
        throw std::invalid_argument("invalid NV21 conversion ROI");
    auto image=std::make_unique<maix::image::Image>(r.width/step,r.height/step,maix::image::Format::FMT_RGB888);
    auto *dest=static_cast<uint8_t*>(image->data());
    for(int y=r.y;y<r.y+r.height;y+=step) for(int x=r.x;x<r.x+r.width;x+=step) {
        const auto *uv=v.vu+(y/2)*v.vu_stride+(x&~1);
        rgb(v.y[y*v.y_stride+x],uv[0],uv[1],dest);dest+=3;
    }
    return image;
}
void invalidate_uncalibrated(TargetEstimate &t) {
    t.angles_valid=false; t.safe_for_control=false; t.pose.valid=false;
    t.yaw_rad=t.pitch_rad=t.green.yaw_rad=t.green.pitch_rad=0;
    t.line_of_sight_camera={0,0,0}; t.line_of_sight_rate_rad_s={0,0}; t.angular_covariance={0,0};
}
HighFpsPipeline::HighFpsPipeline(const ApplicationConfig &c,bool idle,bool stress):config_(c),idle_(idle),stress_(stress) {
    if(c.camera.width!=1344 || c.camera.height!=760 || c.camera.fps!=180 ||
       c.npu.enabled || c.target_geometry.pose_enabled || c.detector.classical_interval_frames!=1)
        throw std::invalid_argument("full180 pipeline requires 1344x760@180, per-observation detector, NPU/pose disabled");
    vision_=std::thread(&HighFpsPipeline::vision_loop,this);
    try {
        control_=std::thread(&HighFpsPipeline::control_loop,this);
        motion_=std::thread(&HighFpsPipeline::motion_loop,this);
    } catch(...) { stopped_=true; frames_.close(); motion_frames_.close();
        vision_.join(); if(control_.joinable()) control_.join(); throw; }
}
HighFpsPipeline::~HighFpsPipeline(){finish();}
void HighFpsPipeline::submit(std::shared_ptr<Nv21Frame> f) {
    { std::lock_guard<std::mutex> lock(stats_mutex_);
      if(!sequence_.received) first_received_=f->metadata.received_us;
      last_received_=f->metadata.received_us; sequence_.observe(f->metadata); }
    frames_.publish(std::move(f));
}
void HighFpsPipeline::finish() {
    if(finished_) return;
    stopped_=true; frames_.close();
    if(vision_.joinable()) vision_.join();
    motion_frames_.close();
    if(motion_.joinable()) motion_.join();
    if(control_.joinable()) control_.join();
    motion_results_.close();
    estimates_.close();
    const auto fs=frames_.stats();
    std::lock_guard<std::mutex> lock(stats_mutex_);
    std::ofstream summary("business.json");
    summary << "{\"received\":"<<sequence_.received<<",\"slot_replaced\":"<<fs.replaced
        <<",\"scheduled_skipped\":"<<scheduled_skipped_<<",\"shutdown_discarded\":"<<fs.shutdown_discarded
        <<",\"slot_taken\":"<<fs.taken<<",\"upstream_missing\":"<<sequence_.missing
        <<",\"upstream_duplicate\":"<<sequence_.duplicate<<",\"upstream_reversed\":"<<sequence_.reversed
        <<",\"pts_nonincreasing\":"<<sequence_.pts_reversed<<",\"vision_frames\":"<<vision_count_
        <<",\"green_detection_calls\":"<<green_count_<<",\"armor_detection_calls\":"<<armor_count_
        <<",\"control_outputs\":"<<output_count_<<",\"first_received_us\":"<<first_received_
        <<",\"last_received_us\":"<<last_received_<<",\"failed\":"<<(failed_?"true":"false")<<"}\n";
    summary.flush(); if(!summary) failed_=true;
    finished_=true;
}
void HighFpsPipeline::vision_loop() {
    try {
        GreenLightDetector detector(config_.detector,config_.armor,config_.target_geometry,config_.npu);
        TimestampSchedule measurement(90), search(30), motion_schedule(30), armor_schedule(60);
        std::vector<Point2f> proposals;
        TargetEstimate last;
        size_t cursor=0;
        AsyncLog log("vision.csv");
        log<<"sequence,pts_raw,received_us,started_us,finished_us,search_ran,green_ran,armor_ran,motion_ran,search_us,roi_convert_us,detect_us,motion_us,direct_green,green_us\n";
        while(auto f=frames_.wait()) {
            if(stopped_) break;
            const auto started=monotonic_us();
            if(idle_ || !measurement.due(f->metadata.received_us)) { ++scheduled_skipped_; continue; }
            const auto v=f->map();
            if(v.width!=1344 || v.height!=760) throw std::runtime_error("unexpected business source dimensions");
            const auto search_start=monotonic_us();
            const bool full=search.due(f->metadata.received_us);
            if(full) proposals=nv21_green_proposals(v);
            const auto search_end=monotonic_us();
            auto prediction=detector.tracker_snapshot(); prediction.predict(f->metadata.received_us);
            Point2f center{v.width/2.0F,v.height/2.0F,true};
            const bool tracked=last.green.valid && !last.green.predicted;
            if(tracked) center={prediction.predicted_x(),prediction.predicted_y(),true};
            else if(!proposals.empty()) center=proposals[cursor++%proposals.size()];
            const int size=tracked ? std::clamp(static_cast<int>(last.green.apparent_size*12),128,384) : 96;
            const auto roi=source_roi(center.x,center.y,size,v.width,v.height);
            const auto convert_start=monotonic_us();
            auto image=nv21_rgb_region(v,roi);
            const auto convert_end=monotonic_us();
            MotionPrior prior;
            if(auto ready=motion_results_.take()) {
                if(ready->timestamp_us<=f->metadata.received_us &&
                   f->metadata.received_us-ready->timestamp_us<=50000) prior=*ready;
            }
            const bool motion_ran=config_.visual_motion.enabled && (tracked || stress_) && motion_schedule.due(f->metadata.received_us);
            const auto motion_start=monotonic_us();
            if(motion_ran) {
                // KLT needs luminance only. Sample Y directly; no full-field color conversion.
                auto job=std::make_shared<MotionFrame>(); job->timestamp=f->metadata.received_us;
                job->image=std::make_shared<maix::image::Image>(v.width/8,v.height/8,maix::image::Format::FMT_RGB888);
                auto *out=static_cast<uint8_t*>(job->image->data());
                for(int y=0;y<v.height;y+=8) for(int x=0;x<v.width;x+=8) {
                    const auto value=clip((298*(v.y[y*v.y_stride+x]-16)+128)>>8);
                    *out++=value;*out++=value;*out++=value;
                }
                motion_frames_.publish(job);
            }
            const auto motion_end=monotonic_us();
            const auto detect_start=monotonic_us();
            last=detector.process_region(*image,roi,v.width,v.height,f->metadata.received_us,prior.valid?&prior:nullptr,stress_,armor_schedule.due(f->metadata.received_us));
            const auto finished=monotonic_us();
            auto snapshot=std::make_shared<Snapshot>(config_.detector);
            snapshot->tracker=detector.tracker_snapshot();
            last.source_metadata_valid=true; last.source_sequence=f->metadata.sequence;
            last.source_pts_raw=f->metadata.pts_raw; last.source_received_us=f->metadata.received_us;
            invalidate_uncalibrated(last); snapshot->target=last; estimates_.publish(snapshot);
            const bool armor_ran=last.armor_detection_ran;
            ++vision_count_; if(last.classical_detection_ran) ++green_count_; if(armor_ran) ++armor_count_;
            log<<f->metadata.sequence<<','<<f->metadata.pts_raw<<','<<f->metadata.received_us<<','<<started<<','<<finished<<','<<full<<",1,"<<armor_ran<<','<<motion_ran<<','<<search_end-search_start<<','<<convert_end-convert_start<<','<<finished-detect_start<<','<<motion_end-motion_start<<','<<(last.green.valid&&!last.green.predicted)<<','<<last.classical_detection_ms*1000<<'\n';
            if(!log) throw std::runtime_error("vision metrics write failed");
        }
        log.finish(); if(!log) throw std::runtime_error("vision metrics flush failed");
    } catch(const std::exception &e) { std::cerr<<"vision: "<<e.what()<<'\n';failed_=true;stopped_=true;frames_.close(); }
}
void HighFpsPipeline::motion_loop() {
    try {
        VisualMotionEstimator estimator(config_.visual_motion);
        AsyncLog log("motion.csv",256*1024);log<<"timestamp_us,started_us,finished_us,valid\n";
        uint64_t previous=0;
        while(auto job=motion_frames_.wait()) {
            if(stopped_) break;
            if(previous && job->timestamp-previous>100000) estimator.reset();
            previous=job->timestamp;
            const auto start=monotonic_us();
            auto prior=std::make_shared<MotionPrior>(estimator.update(*job->image,job->timestamp));
            prior->image_dx_px*=8;prior->image_dy_px*=8;
            motion_results_.publish(prior);
            log<<job->timestamp<<','<<start<<','<<monotonic_us()<<','<<prior->valid<<'\n';
            if(!log) throw std::runtime_error("motion metrics write failed");
        }
        log.finish();if(!log) throw std::runtime_error("motion metrics flush failed");
    } catch(const std::exception &e) {std::cerr<<"motion: "<<e.what()<<'\n';failed_=true;stopped_=true;frames_.close();}
}
void HighFpsPipeline::control_loop() {
    try {
        AsyncLog log("targets.jsonl",8*1024*1024);
        std::shared_ptr<Snapshot> latest;
        const auto origin=std::chrono::steady_clock::now(); uint64_t tick=0;
        while(!stopped_) {
            if(auto next=estimates_.take()) latest=std::move(next);
            // Read after acquiring the snapshot: preemption must not make the
            // output timestamp earlier than a newly published source frame.
            const auto now=monotonic_us();
            TargetEstimate t;
            if(latest) {
                t=latest->target;
                auto predictor=latest->tracker;
                t.green=predictor.update(nullptr,now,config_.detector.camera_model,false);
                t.timestamp_us=now; t.measurement_age_us=t.green.measurement_age_us;
                t.valid=t.green.valid; t.predicted=t.green.predicted;
                t.classical_detection_ran=false; t.armor_detection_ran=false; t.classical_detection_ms=0;
                t.aim_point={t.green.center_x,t.green.center_y,t.green.valid};
                // Cached armor is observational metadata; no synthetic armor prediction.
                t.guidance_mode=GuidanceMode::LampApproach;
                if(t.green.state==TrackState::Candidate) t.state=GuidanceTrackState::Acquiring;
                else if(t.green.valid) t.state=GuidanceTrackState::Coasting;
                else if(t.green.state==TrackState::Tracking) t.state=GuidanceTrackState::Reacquire;
                if(now-t.source_received_us>static_cast<uint64_t>(config_.armor.cache_max_age_ms)*1000)
                    t.armor=ArmorDetection{};
            } else t.timestamp_us=now;
            invalidate_uncalibrated(t);
            {std::lock_guard<std::mutex> lock(stats_mutex_);
             t.upstream_missing=sequence_.missing;t.upstream_duplicate=sequence_.duplicate;t.upstream_reversed=sequence_.reversed;}
            t.application_dropped=frames_.stats().replaced+scheduled_skipped_.load();
            write_target_estimate_json(log,t,nullptr,",\"timestamp_source\":\"host_monotonic_output\",\"measurement_timestamp_source\":\"VIN_receive_host_monotonic\",\"pts_event\":\"SDK_payload_unknown_exposure_phase\",\"exposure_age_valid\":false");
            log<<'\n'; ++output_count_;
            if(!log) throw std::runtime_error("control metrics write failed");
            const auto elapsed=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-origin).count();
            tick=std::max(tick+1,static_cast<uint64_t>(elapsed)*180/1000000+1);
            std::this_thread::sleep_until(origin+std::chrono::microseconds((tick*1000000+179)/180));
        }
        log.finish(); if(!log) throw std::runtime_error("control metrics flush failed");
    } catch(const std::exception&e){std::cerr<<"control: "<<e.what()<<'\n';failed_=true;stopped_=true;frames_.close();}
}
} // namespace dart
