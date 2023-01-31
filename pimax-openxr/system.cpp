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

namespace pimax_openxr {

    using namespace pimax_openxr::log;
    using namespace pimax_openxr::utils;

    // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrGetSystem
    XrResult OpenXrRuntime::xrGetSystem(XrInstance instance, const XrSystemGetInfo* getInfo, XrSystemId* systemId) {
        if (getInfo->type != XR_TYPE_SYSTEM_GET_INFO) {
            return XR_ERROR_VALIDATION_FAILURE;
        }

        TraceLoggingWrite(g_traceProvider,
                          "xrGetSystem",
                          TLXArg(instance, "Instance"),
                          TLArg(xr::ToCString(getInfo->formFactor), "FormFactor"));

        if (!m_instanceCreated || instance != (XrInstance)1) {
            return XR_ERROR_HANDLE_INVALID;
        }

        if (getInfo->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY) {
            return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
        }

        // Create the PVR session.
        if (!m_pvrSession) {
            const auto result = pvr_createSession(m_pvr, &m_pvrSession);

            // This is the error returned when pi_server is not running. We pretend the HMD is not found.
            if (result == pvrResult::pvr_rpc_failed) {
                return XR_ERROR_FORM_FACTOR_UNAVAILABLE;
            }

            CHECK_PVRCMD(result);
        }

        // Check for HMD presence.
        pvrHmdStatus status{};
        CHECK_PVRCMD(pvr_getHmdStatus(m_pvrSession, &status));
        TraceLoggingWrite(g_traceProvider,
                          "PVR_HmdStatus",
                          TLArg(!!status.ServiceReady, "ServiceReady"),
                          TLArg(!!status.HmdPresent, "HmdPresent"),
                          TLArg(!!status.HmdMounted, "HmdMounted"),
                          TLArg(!!status.IsVisible, "IsVisible"),
                          TLArg(!!status.DisplayLost, "DisplayLost"),
                          TLArg(!!status.ShouldQuit, "ShouldQuit"));
        if (!(status.ServiceReady && status.HmdPresent)) {
            return XR_ERROR_FORM_FACTOR_UNAVAILABLE;
        }

        // Query HMD properties.
        CHECK_PVRCMD(pvr_getHmdInfo(m_pvrSession, &m_cachedHmdInfo));
        TraceLoggingWrite(g_traceProvider,
                          "PVR_HmdInfo",
                          TLArg(m_cachedHmdInfo.VendorId, "VendorId"),
                          TLArg(m_cachedHmdInfo.ProductId, "ProductId"),
                          TLArg(m_cachedHmdInfo.Manufacturer, "Manufacturer"),
                          TLArg(m_cachedHmdInfo.ProductName, "ProductName"),
                          TLArg(m_cachedHmdInfo.SerialNumber, "SerialNumber"),
                          TLArg(m_cachedHmdInfo.FirmwareMinor, "FirmwareMinor"),
                          TLArg(m_cachedHmdInfo.FirmwareMajor, "FirmwareMajor"),
                          TLArg(m_cachedHmdInfo.Resolution.w, "ResolutionWidth"),
                          TLArg(m_cachedHmdInfo.Resolution.h, "ResolutionHeight"));
        if (!m_loggedProductName) {
            Log("Device is: %s\n", m_cachedHmdInfo.ProductName);
            m_telemetry.logProduct(m_cachedHmdInfo.ProductName);
            m_loggedProductName = true;
        }

        // Cache common information.
        m_floorHeight = pvr_getFloatConfig(m_pvrSession, CONFIG_KEY_EYE_HEIGHT, 0.f);
        TraceLoggingWrite(g_traceProvider,
                          "PVR_GetConfig",
                          TLArg(CONFIG_KEY_EYE_HEIGHT, "Config"),
                          TLArg(m_floorHeight, "EyeHeight"));

        CHECK_PVRCMD(pvr_getEyeRenderInfo(m_pvrSession, pvrEye_Left, &m_cachedEyeInfo[0]));
        CHECK_PVRCMD(pvr_getEyeRenderInfo(m_pvrSession, pvrEye_Right, &m_cachedEyeInfo[1]));
        updateEyeInfo();
        if (m_useParallelProjection && m_cantingAngle) {
            Log("Parallel projection is enabled\n");
        }

        // Setup common parameters.
        CHECK_PVRCMD(pvr_setTrackingOriginType(m_pvrSession, pvrTrackingOrigin_EyeLevel));

        m_systemCreated = true;
        *systemId = (XrSystemId)1;

        TraceLoggingWrite(g_traceProvider, "xrGetSystem", TLArg((int)*systemId, "SystemId"));

        return XR_SUCCESS;
    }

    // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrGetSystemProperties
    XrResult OpenXrRuntime::xrGetSystemProperties(XrInstance instance,
                                                  XrSystemId systemId,
                                                  XrSystemProperties* properties) {
        if (properties->type != XR_TYPE_SYSTEM_PROPERTIES) {
            return XR_ERROR_VALIDATION_FAILURE;
        }

        TraceLoggingWrite(
            g_traceProvider, "xrGetSystemProperties", TLXArg(instance, "Instance"), TLArg((int)systemId, "SystemId"));

        if (!m_instanceCreated || instance != (XrInstance)1) {
            return XR_ERROR_HANDLE_INVALID;
        }

        if (!m_systemCreated || systemId != (XrSystemId)1) {
            return XR_ERROR_SYSTEM_INVALID;
        }

        XrSystemHandTrackingPropertiesEXT* handTrackingProperties =
            reinterpret_cast<XrSystemHandTrackingPropertiesEXT*>(properties->next);
        if (has_XR_EXT_hand_tracking) {
            while (handTrackingProperties) {
                if (handTrackingProperties->type == XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT) {
                    break;
                }
                handTrackingProperties =
                    reinterpret_cast<XrSystemHandTrackingPropertiesEXT*>(handTrackingProperties->next);
            }
        }

        properties->vendorId = m_cachedHmdInfo.VendorId;

        // We include the "aapvr" string because some applications like OpenXR Toolkit rely on this string to
        // identify Pimax.
        sprintf_s(properties->systemName, sizeof(properties->systemName), "%s (aapvr)", m_cachedHmdInfo.ProductName);
        properties->systemId = systemId;

        properties->trackingProperties.positionTracking = XR_TRUE;
        properties->trackingProperties.orientationTracking = XR_TRUE;

        static_assert(pvrMaxLayerCount >= XR_MIN_COMPOSITION_LAYERS_SUPPORTED);
        properties->graphicsProperties.maxLayerCount = pvrMaxLayerCount;
        properties->graphicsProperties.maxSwapchainImageWidth = 16384;
        properties->graphicsProperties.maxSwapchainImageHeight = 16384;

        TraceLoggingWrite(g_traceProvider,
                          "xrGetSystemProperties",
                          TLArg((int)properties->systemId, "SystemId"),
                          TLArg(properties->vendorId, "VendorId"),
                          TLArg(properties->systemName, "SystemName"),
                          TLArg(!!properties->trackingProperties.positionTracking, "PositionTracking"),
                          TLArg(!!properties->trackingProperties.orientationTracking, "OrientationTracking"),
                          TLArg(properties->graphicsProperties.maxLayerCount, "MaxLayerCount"),
                          TLArg(properties->graphicsProperties.maxSwapchainImageWidth, "MaxSwapchainImageWidth"),
                          TLArg(properties->graphicsProperties.maxSwapchainImageHeight, "MaxSwapchainImageHeight"));

        if (handTrackingProperties) {
            handTrackingProperties->supportsHandTracking = XR_TRUE;

            TraceLoggingWrite(g_traceProvider,
                              "xrGetSystemProperties",
                              TLArg((int)properties->systemId, "SystemId"),
                              TLArg(!!handTrackingProperties->supportsHandTracking, "SupportsHandTracking"));
        }

        return XR_SUCCESS;
    }

    // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrEnumerateEnvironmentBlendModes
    XrResult OpenXrRuntime::xrEnumerateEnvironmentBlendModes(XrInstance instance,
                                                             XrSystemId systemId,
                                                             XrViewConfigurationType viewConfigurationType,
                                                             uint32_t environmentBlendModeCapacityInput,
                                                             uint32_t* environmentBlendModeCountOutput,
                                                             XrEnvironmentBlendMode* environmentBlendModes) {
        // We only support immersive VR mode.
        static const XrEnvironmentBlendMode blendModes[] = {XR_ENVIRONMENT_BLEND_MODE_OPAQUE};

        TraceLoggingWrite(g_traceProvider,
                          "xrEnumerateEnvironmentBlendModes",
                          TLXArg(instance, "Instance"),
                          TLArg((int)systemId, "SystemId"),
                          TLArg(xr::ToCString(viewConfigurationType), "ViewConfigurationType"),
                          TLArg(environmentBlendModeCapacityInput, "EnvironmentBlendModeCapacityInput"));

        if (!m_instanceCreated || instance != (XrInstance)1) {
            return XR_ERROR_HANDLE_INVALID;
        }

        if (!m_systemCreated || systemId != (XrSystemId)1) {
            return XR_ERROR_SYSTEM_INVALID;
        }

        if (viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
            return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
        }

        if (environmentBlendModeCapacityInput && environmentBlendModeCapacityInput < ARRAYSIZE(blendModes)) {
            return XR_ERROR_SIZE_INSUFFICIENT;
        }

        *environmentBlendModeCountOutput = ARRAYSIZE(blendModes);
        TraceLoggingWrite(g_traceProvider,
                          "xrEnumerateEnvironmentBlendModes",
                          TLArg(*environmentBlendModeCountOutput, "EnvironmentBlendModeCountOutput"));

        if (environmentBlendModeCapacityInput && environmentBlendModes) {
            for (uint32_t i = 0; i < *environmentBlendModeCountOutput; i++) {
                environmentBlendModes[i] = blendModes[i];
                TraceLoggingWrite(g_traceProvider,
                                  "xrEnumerateEnvironmentBlendModes",
                                  TLArg(xr::ToCString(environmentBlendModes[i]), "EnvironmentBlendMode"));
            }
        }

        return XR_SUCCESS;
    }

    void OpenXrRuntime::updateEyeInfo() {
        m_cantingAngle = PVR::Quatf{m_cachedEyeInfo[0].HmdToEyePose.Orientation}.Angle(m_cachedEyeInfo[1].HmdToEyePose.Orientation)/2;
        for (uint32_t i = 0; i < xr::StereoView::Count; i++) {
            m_cachedEyeFov[i].angleDown = -atan(m_cachedEyeInfo[i].Fov.DownTan);
            m_cachedEyeFov[i].angleUp = atan(m_cachedEyeInfo[i].Fov.UpTan);
            m_cachedEyeFov[i].angleLeft = -atan(m_cachedEyeInfo[i].Fov.LeftTan);
            m_cachedEyeFov[i].angleRight = atan(m_cachedEyeInfo[i].Fov.RightTan);

            // Apply parallel projection transforms. These are needed in order to calculate the appropriate resolution
            // to recommend for swapchains.
            if (m_useParallelProjection && m_cantingAngle) {
                // Eliminate canting.
                m_cachedEyeInfo[i].HmdToEyePose.Orientation = PVR::Quatf::Identity();

                // Shift FOV by canting angle
                const float angle = i == 0 ? -m_cantingAngle : m_cantingAngle;
                m_cachedEyeFov[i].angleLeft += angle;
                m_cachedEyeFov[i].angleRight += angle;

                // Per https://risa2000.github.io/hmdgdb, PP also increases the vertical FOV by 6 degrees.
                m_cachedEyeFov[i].angleUp += PVR::DegreeToRad(6.f);
                m_cachedEyeFov[i].angleDown -= PVR::DegreeToRad(6.f);
            }
        }
    }

    // Retrieve some information from PVR needed for graphic/frame management.
    void OpenXrRuntime::fillDisplayDeviceInfo() {
        pvrDisplayInfo info{};
        CHECK_PVRCMD(pvr_getEyeDisplayInfo(m_pvrSession, pvrEye_Left, &info));
        TraceLoggingWrite(g_traceProvider,
                          "PVR_EyeDisplayInfo",
                          TraceLoggingCharArray((char*)&info.luid, sizeof(LUID), "Luid"),
                          TLArg(info.edid_vid, "EdidVid"),
                          TLArg(info.edid_pid, "EdidPid"),
                          TLArg(info.pos_x, "PosX"),
                          TLArg(info.pos_y, "PosY"),
                          TLArg(info.width, "Width"),
                          TLArg(info.height, "Height"),
                          TLArg(info.refresh_rate, "RefreshRate"),
                          TLArg((int)info.disp_state, "DispState"),
                          TLArg((int)info.eye_display, "EyeDisplay"),
                          TLArg((int)info.eye_rotate, "EyeRotate"));

        // We also store the expected frame duration.
        m_displayRefreshRate = info.refresh_rate;
        m_frameDuration = 1.0 / info.refresh_rate;

        memcpy(&m_adapterLuid, &info.luid, sizeof(LUID));
    }

} // namespace pimax_openxr
