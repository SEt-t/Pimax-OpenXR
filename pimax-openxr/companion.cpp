// MIT License
//
// Copyright(c) 2022-2023 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this softwareand associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "pch.h"

#include "log.h"
#include "runtime.h"
#include "utils.h"

struct RuntimeStatus {
    bool valid;

    float refreshRate;
    uint32_t resolutionWidth;
    uint32_t resolutionHeight;
    uint8_t fovLevel;
    float fov;
    float floorHeight;
    bool useParallelProjection;
    bool useSmartSmoothing;
    bool useLighthouseTracking;
    float fps;
};

extern "C" __declspec(dllexport) void WINAPI getRuntimeStatus(RuntimeStatus* status) {
    pimax_openxr::log::Log("Hello\n");

    pvrEnvHandle pvr;
    CHECK_PVRCMD(pvr_initialise(&pvr));

    pvrSessionHandle pvrSession;
    CHECK_PVRCMD(pvr_createSession(pvr, &pvrSession));

    pvrDisplayInfo displayInfo{};
    CHECK_PVRCMD(pvr_getEyeDisplayInfo(pvrSession, pvrEye_Left, &displayInfo));

    pvrEyeRenderInfo eyeInfo[xr::StereoView::Count];
    CHECK_PVRCMD(pvr_getEyeRenderInfo(pvrSession, pvrEye_Left, &eyeInfo[0]));
    CHECK_PVRCMD(pvr_getEyeRenderInfo(pvrSession, pvrEye_Right, &eyeInfo[1]));
    float cantingAngle = PVR::Quatf{eyeInfo[0].HmdToEyePose.Orientation}.Angle(eyeInfo[1].HmdToEyePose.Orientation)/2;
    // Add the canting angle.
    const auto fov = PVR::RadToDegree(atan(eyeInfo[0].Fov.LeftTan) + atan(eyeInfo[1].Fov.RightTan) + cantingAngle*2);
    const auto useParallelProjection = cantingAngle && !pvr_getIntConfig(pvrSession, "steamvr_use_native_fov", 0);

    pvrFovPort fovForResolution = eyeInfo[0].Fov;
    if (useParallelProjection) {
        fovForResolution.LeftTan = tan(atan(eyeInfo[0].Fov.LeftTan) + cantingAngle);
        fovForResolution.RightTan = tan(atan(eyeInfo[0].Fov.RightTan) - cantingAngle);
        fovForResolution.UpTan = tan(atan(eyeInfo[0].Fov.UpTan) + PVR::DegreeToRad(6.f));
        fovForResolution.DownTan = tan(atan(eyeInfo[0].Fov.DownTan) + PVR::DegreeToRad(6.f));
    }

    pvrSizei viewportSize;
    CHECK_PVRCMD(pvr_getFovTextureSize(pvrSession, pvrEye_Left, fovForResolution, 1.f, &viewportSize));

    status->refreshRate = displayInfo.refresh_rate;
    status->resolutionWidth = viewportSize.w;
    status->resolutionHeight = viewportSize.h;
    status->fovLevel = pvr_getIntConfig(pvrSession, "fov_level", 1);
    status->fov = fov;
    status->floorHeight = pvr_getFloatConfig(pvrSession, CONFIG_KEY_EYE_HEIGHT, 0.f);
    status->useParallelProjection = useParallelProjection;
    status->useSmartSmoothing = pvr_getIntConfig(pvrSession, "dbg_asw_enable", 0);
    status->useLighthouseTracking = pvr_getIntConfig(pvrSession, "enable_lighthouse_tracking", 0);
    status->fps = pvr_getFloatConfig(pvrSession, "client_fps", 0);

    status->valid = true;

    pvr_destroySession(pvrSession);
    pvr_shutdown(pvr);
}
