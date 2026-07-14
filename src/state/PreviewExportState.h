#pragma once

#include "OFS_StateHandle.h"

// Persisted settings for the animated preview (WebP/AV1) exporter.
struct PreviewExportState
{
    static constexpr auto StateName = "PreviewExportState";

    int32_t format = 0;       // 0 = animated WebP, 1 = AV1 (webm)
    int32_t resolution = 480; // output height in px (width keeps aspect)
    int32_t fps = 15;
    int32_t quality = 75;     // 0..100 (higher = better)
    int32_t rangeMode = 0;    // 0 = chapter, 1 = timestamps

    // Composite the 3D simulator onto the exported video.
    bool overlaySim = false;
    // Normalized overlay rect on the video frame (0..1), top-left origin.
    float simX = 0.62f;
    float simY = 0.60f;
    float simW = 0.34f;
    float simH = 0.34f;

    inline static PreviewExportState& State(uint32_t stateHandle) noexcept
    {
        return OFS_AppState<PreviewExportState>(stateHandle).Get();
    }
};

REFL_TYPE(PreviewExportState)
    REFL_FIELD(format)
    REFL_FIELD(resolution)
    REFL_FIELD(fps)
    REFL_FIELD(quality)
    REFL_FIELD(rangeMode)
    REFL_FIELD(overlaySim)
    REFL_FIELD(simX)
    REFL_FIELD(simY)
    REFL_FIELD(simW)
    REFL_FIELD(simH)
REFL_END
