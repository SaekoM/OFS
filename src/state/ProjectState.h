#pragma once

#include "OFS_StateHandle.h"
#include "Funscript.h"

#include <vector>
#include <string>

// One stretch of constant tempo in the tempo map. It runs from startSeconds until
// the next section starts. Beats fall at anchorSeconds + k * (60 / bpm), and the
// anchor is a downbeat (beat 1 of a 4/4 measure).
struct TempoSection
{
    float startSeconds = 0.f;
    float bpm = 100.f;
    float anchorSeconds = 0.f;
};

REFL_TYPE(TempoSection)
    REFL_FIELD(startSeconds)
    REFL_FIELD(bpm)
    REFL_FIELD(anchorSeconds)
REFL_END

struct TempoOverlayState
{
    static constexpr auto StateName = "TempoOverlayState";

    // Single-tempo fields from before tempo maps existed. Only read to migrate
    // older projects into `sections`.
    float bpm = 100.f;
    float beatOffsetSeconds = 0.f;

    uint32_t measureIndex = 0;

    // Sorted by startSeconds; the first section starts at 0.
    std::vector<TempoSection> sections;

    inline static TempoOverlayState& State(uint32_t stateHandle) noexcept
    {
        return OFS_ProjectState<TempoOverlayState>(stateHandle).Get();
    }
};

REFL_TYPE(TempoOverlayState)
    REFL_FIELD(bpm)
    REFL_FIELD(beatOffsetSeconds)
    REFL_FIELD(measureIndex)
    REFL_FIELD(sections)
REFL_END

struct ProjectState
{
    static constexpr auto StateName = "ProjectState";

    Funscript::Metadata metadata;
    std::string relativeMediaPath;
    float activeTimer = 0.f;
    float lastPlayerPosition = 0.f;
    uint32_t activeScriptIdx = 0;
    bool nudgeMetadata = true;

    std::vector<uint8_t> binaryFunscriptData;

    inline static ProjectState& State(uint32_t stateHandle) noexcept
    {
        return OFS_ProjectState<ProjectState>(stateHandle).Get();
    }
};

REFL_TYPE(ProjectState)
    REFL_FIELD(metadata)
    REFL_FIELD(relativeMediaPath)
    REFL_FIELD(activeTimer)
    REFL_FIELD(lastPlayerPosition)
    REFL_FIELD(activeScriptIdx)
    REFL_FIELD(nudgeMetadata)
    REFL_FIELD(binaryFunscriptData)
REFL_END

