#include "OFS_TempoMap.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace
{
    // Two times this close count as the same grid line (the playhead is often
    // parked exactly on one).
    constexpr double SameLineTolerance = 0.001;
    // Shortest piece a split may leave on either side.
    constexpr double MinSectionSeconds = 0.05;

    // Grid lines are counted in integer "ticks" so that classifying a line as a
    // beat or downbeat is exact. Snap sizes are either whole beats per line
    // (1, 2, 4) or whole lines per beat (2, 3, 4, ... 16).
    struct TickGrid
    {
        int64_t ticksPerBeat;
        int64_t ticksPerLine;
    };

    TickGrid MakeTickGrid(double beatsPerLine) noexcept
    {
        if (beatsPerLine >= 1.0) {
            return { 1, std::max<int64_t>(1, std::llround(beatsPerLine)) };
        }
        return { std::max<int64_t>(1, std::llround(1.0 / beatsPerLine)), 1 };
    }

    double LineSpacing(const TempoSection& section, const TickGrid& grid) noexcept
    {
        return TempoMap::BeatPeriod(section) * (double)grid.ticksPerLine / (double)grid.ticksPerBeat;
    }

    // Floor division / modulo that behave for negative values (lines before the anchor).
    int64_t FloorDiv(int64_t value, int64_t divisor) noexcept
    {
        int64_t quotient = value / divisor;
        if ((value % divisor != 0) && ((value < 0) != (divisor < 0))) quotient -= 1;
        return quotient;
    }

    int64_t FloorMod(int64_t value, int64_t divisor) noexcept
    {
        return value - FloorDiv(value, divisor) * divisor;
    }
}

int TempoMap::SectionIndexAt(const std::vector<TempoSection>& sections, double time) noexcept
{
    if (sections.empty()) return -1;
    int index = 0;
    for (int i = 1; i < (int)sections.size(); i += 1) {
        if (sections[i].startSeconds <= time) index = i;
        else break;
    }
    return index;
}

double TempoMap::SectionEnd(const std::vector<TempoSection>& sections, int index) noexcept
{
    if (index + 1 < (int)sections.size()) return sections[index + 1].startSeconds;
    return std::numeric_limits<double>::infinity();
}

double TempoMap::NextGridTime(const std::vector<TempoSection>& sections, double fromTime, double beatsPerLine) noexcept
{
    if (sections.empty() || beatsPerLine <= 0.0) return fromTime;
    const TickGrid grid = MakeTickGrid(beatsPerLine);

    for (int i = SectionIndexAt(sections, fromTime); i < (int)sections.size(); i += 1) {
        const auto& section = sections[i];
        const double spacing = LineSpacing(section, grid);
        const double searchFrom = std::max(fromTime, (double)section.startSeconds);

        int64_t lineIndex = (int64_t)std::floor((searchFrom - section.anchorSeconds) / spacing);
        double time = section.anchorSeconds + lineIndex * spacing;
        while (time <= fromTime + SameLineTolerance || time < section.startSeconds) {
            lineIndex += 1;
            time = section.anchorSeconds + lineIndex * spacing;
        }
        if (time < SectionEnd(sections, i)) return time;
    }
    return fromTime;
}

double TempoMap::PreviousGridTime(const std::vector<TempoSection>& sections, double fromTime, double beatsPerLine) noexcept
{
    if (sections.empty() || beatsPerLine <= 0.0) return fromTime;
    const TickGrid grid = MakeTickGrid(beatsPerLine);

    for (int i = SectionIndexAt(sections, fromTime); i >= 0; i -= 1) {
        const auto& section = sections[i];
        const double spacing = LineSpacing(section, grid);
        const double sectionEnd = SectionEnd(sections, i);
        const double searchFrom = std::min(fromTime, sectionEnd);

        int64_t lineIndex = (int64_t)std::ceil((searchFrom - section.anchorSeconds) / spacing);
        double time = section.anchorSeconds + lineIndex * spacing;
        while (time >= fromTime - SameLineTolerance || time >= sectionEnd) {
            lineIndex -= 1;
            time = section.anchorSeconds + lineIndex * spacing;
        }
        if (time >= section.startSeconds) return time;
    }
    return fromTime;
}

double TempoMap::NearestGridTime(const std::vector<TempoSection>& sections, double time, double beatsPerLine) noexcept
{
    // Next/Previous skip lines within SameLineTolerance of their start point, so
    // searching from just outside that tolerance also finds a line sitting on `time`.
    const double searchOffset = 2.0 * SameLineTolerance;
    const double after = NextGridTime(sections, time - searchOffset, beatsPerLine);
    const double before = PreviousGridTime(sections, time + searchOffset, beatsPerLine);
    const bool hasAfter = after != time - searchOffset;
    const bool hasBefore = before != time + searchOffset;
    if (hasAfter && hasBefore) return (after - time) < (time - before) ? after : before;
    if (hasAfter) return after;
    if (hasBefore) return before;
    return time;
}

std::vector<TempoMap::GridLine> TempoMap::GridLinesBetween(const std::vector<TempoSection>& sections,
    double fromTime, double toTime, double beatsPerLine) noexcept
{
    constexpr size_t MaxLines = 20000; // safety net; callers thin the grid long before this
    std::vector<GridLine> lines;
    if (sections.empty() || beatsPerLine <= 0.0 || toTime <= fromTime) return lines;
    const TickGrid grid = MakeTickGrid(beatsPerLine);
    const int64_t ticksPerMeasure = grid.ticksPerBeat * BeatsPerMeasure;

    for (int i = SectionIndexAt(sections, fromTime); i < (int)sections.size(); i += 1) {
        const auto& section = sections[i];
        if (section.startSeconds >= toTime) break;

        const double rangeStart = std::max(fromTime, (double)section.startSeconds);
        const double rangeEnd = std::min(toTime, SectionEnd(sections, i));
        if (rangeEnd <= rangeStart) continue;

        const double spacing = LineSpacing(section, grid);
        // Measure numbering starts at the first downbeat inside the section.
        const int64_t firstMeasure = (int64_t)std::ceil((section.startSeconds - section.anchorSeconds) / MeasureLength(section) - 1e-9);

        for (int64_t lineIndex = (int64_t)std::ceil((rangeStart - section.anchorSeconds) / spacing - 1e-9); ; lineIndex += 1) {
            const double time = section.anchorSeconds + lineIndex * spacing;
            if (time >= rangeEnd || lines.size() >= MaxLines) break;

            const int64_t tick = lineIndex * grid.ticksPerLine;
            GridLine line;
            line.time = time;
            line.isBeat = FloorMod(tick, grid.ticksPerBeat) == 0;
            line.isMeasureStart = FloorMod(tick, ticksPerMeasure) == 0;
            line.measureNumber = line.isMeasureStart
                ? (int)(FloorDiv(tick, ticksPerMeasure) - firstMeasure + 1)
                : 0;
            lines.push_back(line);
        }
    }
    return lines;
}

void TempoMap::WrapAnchor(TempoSection& section) noexcept
{
    const double measureLength = MeasureLength(section);
    const double wholeMeasures = std::floor((section.anchorSeconds - section.startSeconds) / measureLength);
    section.anchorSeconds = (float)(section.anchorSeconds - wholeMeasures * measureLength);
}

void TempoMap::SetBpmKeepingDownbeat(TempoSection& section, double newBpm, double nearTime) noexcept
{
    const double measureLength = MeasureLength(section);
    const double measuresAway = std::round((nearTime - section.anchorSeconds) / measureLength);
    section.anchorSeconds = (float)(section.anchorSeconds + measuresAway * measureLength);
    section.bpm = (float)std::clamp(newBpm, (double)MinBpm, (double)MaxBpm);
    WrapAnchor(section);
}

void TempoMap::ShiftDownbeat(TempoSection& section, int beats) noexcept
{
    section.anchorSeconds = (float)(section.anchorSeconds + beats * BeatPeriod(section));
    WrapAnchor(section);
}

void TempoMap::Normalize(std::vector<TempoSection>& sections) noexcept
{
    if (sections.empty()) return;
    std::stable_sort(sections.begin(), sections.end(),
        [](const TempoSection& a, const TempoSection& b) { return a.startSeconds < b.startSeconds; });

    std::vector<TempoSection> kept;
    kept.reserve(sections.size());
    for (auto section : sections) {
        if (!kept.empty() && section.startSeconds - kept.back().startSeconds < MinSectionSeconds) continue;
        section.bpm = std::clamp(section.bpm, MinBpm, MaxBpm);
        kept.push_back(section);
    }
    kept.front().startSeconds = 0.f;
    for (auto& section : kept) WrapAnchor(section);
    sections = std::move(kept);
}

void TempoMap::SplitAt(std::vector<TempoSection>& sections, double time) noexcept
{
    const int index = SectionIndexAt(sections, time);
    if (index < 0) return;
    if (time - sections[index].startSeconds < MinSectionSeconds) return;
    if (SectionEnd(sections, index) - time < MinSectionSeconds) return;

    TempoSection second = sections[index];
    second.startSeconds = (float)time;
    WrapAnchor(second);
    sections.insert(sections.begin() + index + 1, second);
}

void TempoMap::MergeWithPrevious(std::vector<TempoSection>& sections, int index) noexcept
{
    if (index <= 0 || index >= (int)sections.size()) return;
    sections.erase(sections.begin() + index);
}
