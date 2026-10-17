#pragma once

#include <string>

// Live adjustments of the native renderer's effects: read every frame by the
// renderer and the game's hooks, changed by the tuning panel (F10). The
// strengths multiply the game's own, which it keeps changing by scene and
// camera; the defaults were set by eye against the PS2 (2026-10-17). The
// SOTC_* variables set them at start. Plain fields: the panel and the renderer may race on a value for
// a frame, which only delays the change.
namespace native_renderer
{
    struct Tuning
    {
        int renderScale = 1;        // the picture drawn at 512 x 448 times this (SOTC_RENDER_SCALE; 3 on the Mac)
        bool bloom = true;          // SOTC_BLOOM_SCALE=0 turns it off
        float bloomStrength = 0.46f; // x the game's add strength (SOTC_BLOOM_SCALE)
        int bloomRounds = 1;        // blur rounds of +-3 texels at a quarter size (SOTC_BLOOM_ROUNDS)
        float bloomSpread = 1.0f;   // each further round's growth (SOTC_BLOOM_SPREAD)
        bool vertexFog = true;      // the GS fog by distance (SOTC_NATIVE_NOVFOG)
        float vertexFogAmount = 0.55f; // x the fog's share (0: none, 1: the game's; SOTC_FOG_AMOUNT)
        bool zFog = true;           // fade into the sky by depth (SOTC_NATIVE_ZFOG=0)
        bool glow = true;           // layer 8 glow (SOTC_NATIVE_NOGLOW)
        bool depthOfField = true;   // SOTC_NATIVE_NODOF
        bool shadows = true;        // SOTC_NATIVE_NOSHADOWS
        bool particles = true;      // SOTC_NATIVE_NOPTCL
        bool screenBlur = true;     // SOTC_NATIVE_NOSBLUR
        bool reflections = true;    // SOTC_NATIVE_NOREFLECT
        bool refraction = true;     // SOTC_NATIVE_NOREFRACT
    };

    Tuning &tuning();
    // This platform's defaults (no environment).
    Tuning defaultTuning();
    // The values that differ from the defaults, as environment variables
    // (to keep a setting, or to tell it).
    std::string tuningAsEnv();
}
