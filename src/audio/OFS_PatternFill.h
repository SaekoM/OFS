#pragma once

#include "OFS_StrokePattern.h"
#include "state/ProjectState.h"

#include <cstdint>
#include <vector>

// Places stroke patterns on the tempo map and snaps times to it. Pure
// computation; the Tempo overlay applies the results to the script.
namespace PatternFill
{
    // Built-in patterns, listed before the user's custom ones.
    const std::vector<StrokePattern>& BuiltInPatterns() noexcept;

    struct PlannedAction
    {
        double time;
        int32_t pos;
    };

    // The pattern laid on the grid lines in [fromTime, toTime), repeating as
    // needed. Step 0 lands on the first whole beat of the range; pattern values
    // 0..100 map onto the depth bottom..top.
    std::vector<PlannedAction> Generate(const std::vector<TempoSection>& sections, const StrokePattern& pattern,
        double fromTime, double toTime, int32_t bottom, int32_t top) noexcept;

    struct TimeRange
    {
        double from = 0.0;
        double to = 0.0;
        bool IsEmpty() const noexcept { return to <= from; }
    };

    // `measures` measures from the beat nearest the playhead, cut off where its
    // section (song) ends.
    TimeRange RangeFromPlayhead(const std::vector<TempoSection>& sections, double playhead,
        int32_t measures, double mediaDuration) noexcept;
    // The whole section under the playhead.
    TimeRange RangeOfSection(const std::vector<TempoSection>& sections, double playhead, double mediaDuration) noexcept;

    // Moves `time` toward the nearest grid line: strength 0 leaves it, 1 puts it on the line.
    double SnapTime(const std::vector<TempoSection>& sections, double time, double beatsPerLine, double strength) noexcept;
}
