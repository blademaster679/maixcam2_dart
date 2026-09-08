/* Included in the Axera OS04A10 driver after its SDK headers. */
#include "hfr_generated.h"

static AX_S32 os04a10_hfr_check_fps(AX_F32 fps, AX_F32 line_us)
{
#if OS04A10_HFR_AVAILABLE
    if (!isfinite(fps) || !isfinite(line_us) || fps < 1.0f || fps > OS04A10_HFR_FPS || line_us <= 0.0f)
        return AX_SNS_ERR_NOT_SUPPORT;
    // The nominal mode uses its exact table VTS, as the vendor set_fps does.
    // Avoid rejecting a boundary frame length due to float line-time rounding.
    const double vts = IS_SNS_FPS_EQUAL(fps, OS04A10_HFR_FPS) ? OS04A10_HFR_VTS :
        floor(1000000.0 / ((double)line_us * fps));
    if (vts < OS04A10_HFR_MIN_VTS || vts > 65535.0)
        return AX_SNS_ERR_NOT_SUPPORT;
    return AX_SNS_SUCCESS;
#else
    (void)fps;
    (void)line_us;
    return AX_SNS_ERR_NOT_SUPPORT;
#endif
}
