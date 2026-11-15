// Shim: vcpkg's imgui port installs backend headers flat (imgui_impl_dx11.h),
// not under a backends/ subdirectory like the upstream repo layout this code
// was written against.
#pragma once
#include <imgui_impl_dx11.h>
