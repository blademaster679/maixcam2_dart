#pragma once
// Same public ISP operations/units as the pinned official Camera implementation.
// Avoid Camera::exposure's unrelated _fps = 1e6/shutter bookkeeping.
#include "dart/green_detector.hpp"
#include "ax_isp_api.h"
#include <fstream>
#include <thread>
#include <chrono>
static void apply_vin_camera_settings(const dart::CameraSettings &c, AX_SENSOR_REGISTER_FUNC_T *sensor) {
    AX_U32 hi=0,lo=0;
    if(!sensor || !sensor->pfn_sensor_read_register ||
       sensor->pfn_sensor_read_register(0,0x380e,&hi) || sensor->pfn_sensor_read_register(0,0x380f,&lo) || !(hi*256+lo))
        throw std::runtime_error("VTS readback required for shutter quantization check");
    const double row_us=1000000.0/(c.fps*(hi*256+lo));
    if(c.exposure_us>=1000000/c.fps) throw std::runtime_error("exposure exceeds source frame period");
    auto shutter_matches=[&](const AX_ISP_IQ_AE_PARAM_T &p){return !p.nEnable &&
        std::abs(static_cast<double>(p.tExpManual.nShutter)-c.exposure_us)<=std::ceil(row_us);};
    auto gain_matches=[&](const AX_ISP_IQ_AE_PARAM_T &p){return c.gain<=0 ||
        (!p.nEnable && p.tExpManual.nAGain==static_cast<AX_U32>(c.gain));};
    AX_ISP_IQ_AE_PARAM_T ae{};
    if(AX_ISP_IQ_GetAeParam(0,&ae)) throw std::runtime_error("GetAeParam failed");
    if(c.exposure_us>0) {
        ae.nEnable=0;ae.tExpManual.nShutter=c.exposure_us;
        ae.tExpManual.nShortShutter=c.exposure_us/16;ae.tExpManual.nVsShutter=c.exposure_us;
    }
    if(c.gain>0) {ae.nEnable=0;ae.tExpManual.nAGain=c.gain;}
    if(AX_ISP_IQ_SetAeParam(0,&ae) || AX_ISP_IQ_GetAeParam(0,&ae))
        throw std::runtime_error("Set/readback AE failed");
    // ISP updates may be queued for a subsequent frame. Bound readback settling.
    for(int retry=0;retry<20 &&
        ((c.exposure_us>0 && !shutter_matches(ae)) || !gain_matches(ae));++retry) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if(AX_ISP_IQ_GetAeParam(0,&ae)) throw std::runtime_error("AE settling readback failed");
    }
    if((c.exposure_us>0 && !shutter_matches(ae)) || !gain_matches(ae)) {
        std::cerr<<"AE readback enable="<<static_cast<int>(ae.nEnable)
                 <<" shutter="<<ae.tExpManual.nShutter<<" requested_shutter="<<c.exposure_us
                 <<" again="<<ae.tExpManual.nAGain<<" requested_again="<<c.gain<<'\n';
        throw std::runtime_error("AE shutter/gain readback mismatch");
    }
    AX_ISP_IQ_AWB_PARAM_T wb{};
    if(AX_ISP_IQ_GetAwbParam(0,&wb)) throw std::runtime_error("GetAwbParam failed");
    if(c.manual_white_balance) {
        wb.nEnable=0;
        auto gain=[](float g){return static_cast<AX_U16>(256+3839*std::clamp(g,0.0F,1.0F));};
        wb.tManualParam.tGain.nGainR=gain(c.white_balance_gain[0]);
        wb.tManualParam.tGain.nGainGr=gain(c.white_balance_gain[1]);
        wb.tManualParam.tGain.nGainGb=gain(c.white_balance_gain[2]);
        wb.tManualParam.tGain.nGainB=gain(c.white_balance_gain[3]);
    } else wb.nEnable=1;
    const auto expected_gain=wb.tManualParam.tGain;
    if(AX_ISP_IQ_SetAwbParam(0,&wb) || AX_ISP_IQ_GetAwbParam(0,&wb))
        throw std::runtime_error("Set/readback AWB failed");
    auto wb_matches=[&] {
        const auto &g=wb.tManualParam.tGain;
        return c.manual_white_balance ? !wb.nEnable &&
            g.nGainR==expected_gain.nGainR && g.nGainGr==expected_gain.nGainGr &&
            g.nGainGb==expected_gain.nGainGb && g.nGainB==expected_gain.nGainB : wb.nEnable!=0;
    };
    for(int retry=0;retry<20 && !wb_matches();++retry) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if(AX_ISP_IQ_GetAwbParam(0,&wb)) throw std::runtime_error("AWB settling readback failed");
    }
    if(!wb_matches()) throw std::runtime_error("AWB gain readback mismatch");
    std::ofstream("camera_settings.json")<<"{\"ae_enabled\":"<<static_cast<int>(ae.nEnable)
      <<",\"requested_shutter_us\":"<<c.exposure_us<<",\"sensor_row_us_nominal\":"<<row_us<<",\"manual_shutter_us\":"<<ae.tExpManual.nShutter<<",\"requested_again_raw\":"<<c.gain<<",\"manual_again_raw\":"<<ae.tExpManual.nAGain
      <<",\"awb_enabled\":"<<static_cast<int>(wb.nEnable)<<",\"wb_raw\":["<<wb.tManualParam.tGain.nGainR<<','
      <<wb.tManualParam.tGain.nGainGr<<','<<wb.tManualParam.tGain.nGainGb<<','<<wb.tManualParam.tGain.nGainB
      <<"],\"calibrated\":false}\n";
}
