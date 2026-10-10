#pragma once

#include "state/ProjectState.h"

#include <vector>

// Grid math over a tempo map: TempoSections sorted by startSeconds, the first
// starting at 0. A grid line falls every `beatsPerLine` beats from a section's
// anchor (1 = every beat, 4 = every measure, 0.25 = every sixteenth).
namespace TempoMap
{
    constexpr int BeatsPerMeasure = 4;
    constexpr float MinBpm = 20.f;
    constexpr float MaxBpm = 400.f;

    inline double BeatPeriod(const TempoSection& section) noexcept { return 60.0 / section.bpm; }
    inline double MeasureLength(const TempoSection& section) noexcept { return BeatPeriod(section) * BeatsPerMeasure; }

    // Index of the section covering `time` (the last one starting at or before it).
    int SectionIndexAt(const std::vector<TempoSection>& sections, double time) noexcept;
    // Where a section ends: the next section's start, or +infinity for the last one.
    double SectionEnd(const std::vector<TempoSection>& sections, int index) noexcept;

    // The closest grid line strictly after / before `fromTime`, crossing into
    // neighbouring sections when needed. Returns `fromTime` if there is none.
    double NextGridTime(const std::vector<TempoSection>& sections, double fromTime, double beatsPerLine) noexcept;
    double PreviousGridTime(const std::vector<TempoSection>& sections, double fromTime, double beatsPerLine) noexcept;
    // The grid line closest to `time`, which may be `time` itself.
    double NearestGridTime(const std::vector<TempoSection>& sections, double time, double beatsPerLine) noexcept;

    struct GridLine
    {
        double time;
        bool isBeat;          // falls on a whole beat
        bool isMeasureStart;  // falls on a downbeat
        int measureNumber;    // 1-based within its section; valid when isMeasureStart
    };
    // All grid lines in [fromTime, toTime) across every section they touch.
    std::vector<GridLine> GridLinesBetween(const std::vector<TempoSection>& sections,
        double fromTime, double toTime, double beatsPerLine) noexcept;

    // Moves the anchor by whole measures so it sits in [start, start + measure).
    // The grid does not change; this only keeps the stored number small.
    void WrapAnchor(TempoSection& section) noexcept;
    // Changes the tempo while keeping the downbeat nearest `nearTime` in place,
    // so the grid around the playhead does not jump.
    void SetBpmKeepingDownbeat(TempoSection& section, double newBpm, double nearTime) noexcept;
    // Rotates which beat counts as beat 1. Positive moves the downbeat later.
    void ShiftDownbeat(TempoSection& section, int beats) noexcept;

    // Sorts, clamps tempos, drops duplicate starts and forces the first start to 0.
    void Normalize(std::vector<TempoSection>& sections) noexcept;
    // Splits the section covering `time` in two; the grid is unchanged.
    void SplitAt(std::vector<TempoSection>& sections, double time) noexcept;
    // Removes section `index` so the previous section extends over it.
    void MergeWithPrevious(std::vector<TempoSection>& sections, int index) noexcept;
}
