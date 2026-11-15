// The setup program's Documents\DyingLight\out\settings\video.scr (docs/superpowers/specs/2026-10-17-installer-design.md
// 5.6). Pure and header-only - tested by src/src/tests/setup_videoscr_test.cpp.
//
// Jay 2026-10-17: "Use my values except for the shadows, set that to 'Very High' instead" ("4096/4096 is very high"),
// and "Overwrite" an existing file. Resolution and Monitor are left out (the engine uses the monitor's own size when
// Resolution is missing - parser +0x23D970). MotionBlur(0) MUST be written: an absent MotionBlur loads as ON, while an
// absent NvidiaEffects loads as all three OFF, so no NvidiaEffects line is written. INV-593: this exact text, written
// with the game closed, survived a flat launch + quit and the menu read it as written.
#pragma once

#include <string>

namespace dlvr::setup {

inline std::string VideoScrText()
{
    // The engine's own header block, byte for byte (engine +0x23EAE0 writes it with length 1105; ida/vs1_engine_out.txt).
    std::string t =
        "!Resolution(i,i)\r\n"
        "!WindowOffset(i,i)\r\n"
        "!Monitor(i)                    // -1 primary monitor\r\n"
        "!TextureQuality(s)             // Low, Medium, High\r\n"
        "!GammaFloat(f)\r\n"
        "!Shadows(s)                    // Low, High\r\n"
        "!ShadowMapSize(i)\r\n"
        "!SpotShadowMapSize(i)\r\n"
        "!Fullscreen()\r\n"
        "!Borderless()\r\n"
        "!MaxFPS(i)\t\t\t\t\t\t// Max frame rate\r\n"
        "!VSync(i)                      // vertical sync; 0 - disable, 1 - every frame, 2 - every 2 frames, etc.\r\n"
        "!VSyncDisableBelowTarget(i)    // 1 - enable; 0 - disable\r\n"
        "!GrassQuality(i)               // lower is better\r\n"
        "!NvidiaEffects(i,i,i)          // enable extra nvidia effects(hbao+,dof,pcss)\r\n"
        "!ExtraGameFov(f)               // extra fov\r\n"
        "!VisRange(f,f)\r\n"
        "!TinyObjectsRange(f)\r\n"
        "!OculusEnabled()               // enable support for Oculus\r\n"
        "!AmbientOcclusion(i)           // 1 - enable; 0 - disable\r\n"
        "!MotionBlur(i)                 // 1 - enable; 0 - disable\r\n"
        "!AntiAliasing(i)               // 1 - enable; 0 - disable\r\n"
        "!DisableDWM(i)                 // disables DWM when fullscreen\r\n"
        "!3dtvSettings(f,f)           // /3dtv ui depth, scene separation/offset (-0.15, 0.041, -0.00722)\r\n"
        "!Version(i)\r\n"
        "\r\n";
    t +=
        "TextureQuality(\"High\")\r\n"
        "VisRange(3.40, 3.40)\r\n"
        "TinyObjectsRange(1.00)\r\n"
        "ShadowMapSize(4096)\r\n"
        "SpotShadowMapSize(4096)\r\n"
        "GammaFloat(1.00)\r\n"
        "GrassQuality(0)\r\n"
        "VSync(0)\r\n"
        "ExtraGameFov(20.00)\r\n"
        "OculusEnabled()\r\n"
        "AmbientOcclusion(1)\r\n"
        "MotionBlur(0)\r\n"
        "AntiAliasing(1)\r\n"
        "DisableDWM(1)\r\n"
        "3dtvSettings(-0.150000, 0.000000)\r\n"
        "Version(1)\r\n"
        "Borderless()\r\n";
    return t;
}

}  // namespace dlvr::setup
