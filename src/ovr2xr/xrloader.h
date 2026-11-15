// Minimal OpenXR runtime loader.
//
// We deliberately do not link the Khronos loader. It would be a large build
// dependency for one job we can do in ~100 lines: read the active runtime from
// the registry, load its DLL, and perform the loader/runtime negotiation
// handshake to obtain xrGetInstanceProcAddr.
//
// Trade-off: this bypasses OpenXR API layers (the OpenXR equivalent of
// injectable middleware - OpenXR Toolkit, validation layers, etc). For a mod
// talking to one known runtime that is an acceptable loss, and it keeps the
// translation layer a single self-contained file the way ReviveXR is. If we
// ever need layers, swap this out for the real loader.
#pragma once

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11

#include <windows.h>
#include <d3d11.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace dlvr {

// Loads the active OpenXR runtime and resolves xrGetInstanceProcAddr.
// Returns false if there is no runtime, it can't be loaded, or negotiation
// fails. `whyNot` receives a human-readable reason.
bool LoadOpenXrRuntime(char* whyNot, size_t whyNotBytes);

// Valid once LoadOpenXrRuntime() has returned true.
extern PFN_xrGetInstanceProcAddr xrGetInstanceProcAddr_;

// Path of the runtime DLL we loaded, for logging.
extern char g_runtimePath[];

// The instance-level entry points we use. Resolved by ResolveInstanceFunctions()
// once an XrInstance exists; the handful needed before that (xrCreateInstance,
// xrEnumerateInstanceExtensionProperties) resolve against XR_NULL_HANDLE.
#define DLVR_XR_GLOBAL_FUNCS(X)            \
    X(xrCreateInstance)                    \
    X(xrEnumerateInstanceExtensionProperties)

#define DLVR_XR_INSTANCE_FUNCS(X)          \
    X(xrDestroyInstance)                   \
    X(xrGetInstanceProperties)             \
    X(xrGetSystem)                         \
    X(xrGetSystemProperties)               \
    X(xrGetCurrentInteractionProfile)      \
    X(xrPathToString)                      \
    X(xrEnumerateViewConfigurationViews)   \
    X(xrEnumerateEnvironmentBlendModes)    \
    X(xrGetD3D11GraphicsRequirementsKHR)   \
    X(xrCreateSession)                     \
    X(xrDestroySession)                    \
    X(xrCreateReferenceSpace)              \
    X(xrDestroySpace)                      \
    X(xrPollEvent)                         \
    X(xrBeginSession)                      \
    X(xrEndSession)                        \
    X(xrWaitFrame)                         \
    X(xrBeginFrame)                        \
    X(xrEndFrame)                          \
    X(xrLocateViews)                       \
    X(xrLocateSpace)                       \
    X(xrEnumerateSwapchainFormats)         \
    X(xrCreateSwapchain)                   \
    X(xrDestroySwapchain)                  \
    X(xrEnumerateSwapchainImages)          \
    X(xrAcquireSwapchainImage)             \
    X(xrWaitSwapchainImage)                \
    X(xrReleaseSwapchainImage)             \
    X(xrResultToString)                    \
    X(xrStringToPath)                      \
    X(xrCreateActionSet)                   \
    X(xrDestroyActionSet)                  \
    X(xrCreateAction)                      \
    X(xrSuggestInteractionProfileBindings) \
    X(xrAttachSessionActionSets)           \
    X(xrCreateActionSpace)                 \
    X(xrSyncActions)                       \
    X(xrGetActionStatePose)                \
    X(xrGetActionStateFloat)               \
    X(xrGetActionStateVector2f)            \
    X(xrGetActionStateBoolean)             \
    X(xrApplyHapticFeedback)

#define DLVR_DECLARE(fn) extern PFN_##fn fn##_;
DLVR_XR_GLOBAL_FUNCS(DLVR_DECLARE)
DLVR_XR_INSTANCE_FUNCS(DLVR_DECLARE)
#undef DLVR_DECLARE

bool ResolveGlobalFunctions();
bool ResolveInstanceFunctions(XrInstance instance);

}  // namespace dlvr
