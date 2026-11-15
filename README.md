# Dying Light VR

A native 6DoF OpenXR VR mod for Dying Light (Enhanced Edition): true stereo, motion controllers, no helper program.

To play, download the latest release and run `DyingLightVR_Launcher.exe`. The launcher installs, updates and removes the
mod, switches between VR and flatscreen, and lets you change the settings before you launch.

## Building from source

Requirements: Windows, Visual Studio 2022 Build Tools (C++), CMake, and vcpkg (for OpenXR and Dear ImGui, see `vcpkg.json`).

1. Put the Oculus PC SDK headers in place: see `third_party/OVR/README.md`.
2. Download the NVIDIA DLSS SDK (it holds `include/nvsdk_ngx.h` and `lib/Windows_x86_64/x64/nvsdk_ngx_d.lib`) and the AMD
   FidelityFX SDK 2.3.0 (`Kits/FidelityFX`). By default the build looks for them in `../DLSS_SDK` and
   `../FSR_SDK/v2.3.0/Kits/FidelityFX` next to this folder; or pass their paths as below.
3. Configure and build:

   ```
   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake ^
         -DDLVR_NGX_SDK=<DLSS SDK folder> -DDLVR_FFX_SDK=<FidelityFX SDK>/Kits/FidelityFX
   cmake --build build --config Release
   cmake --build build --config Release --target DyingLightVR_Launcher
   ```

The two mod DLLs (`LibOVRRT64_1.dll`, `xinput1_3.dll`) and the launcher land in `build/Release/`.

## Credits

- HUD layout: "VR Hud and UI correct" by IMIG (Nexus Mods), used with permission.
- Dear ImGui (MIT), puff (zlib), AMD FidelityFX SDK (MIT), NVIDIA DLSS.
