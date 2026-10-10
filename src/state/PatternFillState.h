#pragma once

#include "OFS_StateHandle.h"
#include "audio/OFS_StrokePattern.h"

#include <cstdint>
#include <vector>

// Which part of the timeline "Fill" writes to.
enum PatternFillRange : int32_t
{
    FillFromPlayhead = 0,  // N measures starting at the beat nearest the playhead
    FillWholeSection = 1,  // the whole tempo section under the playhead
    FillSelectionSpan = 2, // from the first to the last selected action
};

// Persisted settings for the Tempo mode pattern fill and snap tools.
struct PatternFillState
{
    static constexpr auto StateName = "PatternFillState";

    std::vector<StrokePattern> customPatterns;
    int32_t patternIndex = 0; // built-in patterns first, then customPatterns
    int32_t rangeMode = FillFromPlayhead;
    int32_t measures = 4;
    int32_t bottom = 0;
    int32_t top = 100;
    int32_t snapStrength = 100; // percent of the way to the nearest grid line

    inline static PatternFillState& State(uint32_t stateHandle) noexcept
    {
        return OFS_AppState<PatternFillState>(stateHandle).Get();
    }
};

REFL_TYPE(StrokePattern)
    REFL_FIELD(name)
    REFL_FIELD(stepsPerBeat)
    REFL_FIELD(steps)
REFL_END

REFL_TYPE(PatternFillState)
    REFL_FIELD(customPatterns)
    REFL_FIELD(patternIndex)
    REFL_FIELD(rangeMode)
    REFL_FIELD(measures)
    REFL_FIELD(bottom)
    REFL_FIELD(top)
    REFL_FIELD(snapStrength)
REFL_END
