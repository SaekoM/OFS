#include "OFS_PatternFill.h"
#include "OFS_TempoMap.h"

#include <algorithm>
#include <cmath>

namespace
{
    constexpr int32_t X = StrokePattern::NoAction;

    StrokePattern MakePattern(const char* name, int32_t stepsPerBeat, std::vector<int32_t> steps) noexcept
    {
        StrokePattern pattern;
        pattern.name = name;
        pattern.stepsPerBeat = stepsPerBeat;
        pattern.steps = std::move(steps);
        return pattern;
    }
}

const std::vector<StrokePattern>& PatternFill::BuiltInPatterns() noexcept
{
    // 0 = bottom of the depth, 100 = top. Down-strokes land on the beat.
    static const std::vector<StrokePattern> patterns = {
        MakePattern("1 stroke per beat", 2, { 0, 100 }),
        MakePattern("Half time (1 stroke per 2 beats)", 1, { 0, 100 }),
        MakePattern("Double time (2 strokes per beat)", 4, { 0, 100, 0, 100 }),
        MakePattern("1 stroke per measure", 1, { 0, X, 100, X }),
        MakePattern("Triplets (3 strokes per 2 beats)", 3, { 0, 100, 0, 100, 0, 100 }),
        MakePattern("Off-beat (down on the \"and\")", 2, { 100, 0 }),
        MakePattern("Deep / shallow", 2, { 0, 100, 50, 100 }),
        MakePattern("Double, then long", 4, { 0, 100, 0, 100, X, X, X, X }),
        MakePattern("Tease (top half)", 2, { 50, 100 }),
    };
    return patterns;
}

std::vector<PatternFill::PlannedAction> PatternFill::Generate(const std::vector<TempoSection>& sections,
    const StrokePattern& pattern, double fromTime, double toTime, int32_t bottom, int32_t top) noexcept
{
    std::vector<PlannedAction> actions;
    if (pattern.steps.empty() || pattern.stepsPerBeat < 1 || toTime <= fromTime) return actions;

    const auto lines = TempoMap::GridLinesBetween(sections, fromTime, toTime, 1.0 / pattern.stepsPerBeat);
    if (lines.empty()) return actions;

    // Count steps from the first whole beat so the pattern starts on the beat;
    // lines before it (a pickup) get the tail end of the pattern.
    const auto firstBeat = std::find_if(lines.begin(), lines.end(), [](const auto& line) { return line.isBeat; });
    const int64_t firstBeatIndex = firstBeat != lines.end() ? (int64_t)(firstBeat - lines.begin()) : 0;
    const int64_t stepCount = (int64_t)pattern.steps.size();

    for (int64_t i = 0; i < (int64_t)lines.size(); i += 1) {
        const int64_t step = (((i - firstBeatIndex) % stepCount) + stepCount) % stepCount;
        const int32_t value = pattern.steps[step];
        if (value == StrokePattern::NoAction) continue;

        const double share = std::clamp(value, 0, 100) / 100.0;
        actions.push_back({ lines[i].time, (int32_t)std::lround(bottom + (top - bottom) * share) });
    }
    return actions;
}

PatternFill::TimeRange PatternFill::RangeFromPlayhead(const std::vector<TempoSection>& sections, double playhead,
    int32_t measures, double mediaDuration) noexcept
{
    TimeRange range;
    if (sections.empty() || measures < 1) return range;

    range.from = TempoMap::NearestGridTime(sections, playhead, 1.0);
    const int index = TempoMap::SectionIndexAt(sections, range.from);
    range.to = range.from + measures * TempoMap::MeasureLength(sections[index]);
    range.to = std::min({ range.to, TempoMap::SectionEnd(sections, index), mediaDuration });
    return range;
}

PatternFill::TimeRange PatternFill::RangeOfSection(const std::vector<TempoSection>& sections, double playhead,
    double mediaDuration) noexcept
{
    TimeRange range;
    const int index = TempoMap::SectionIndexAt(sections, playhead);
    if (index < 0) return range;

    range.from = sections[index].startSeconds;
    range.to = std::min(TempoMap::SectionEnd(sections, index), mediaDuration);
    return range;
}

double PatternFill::SnapTime(const std::vector<TempoSection>& sections, double time, double beatsPerLine, double strength) noexcept
{
    const double target = TempoMap::NearestGridTime(sections, time, beatsPerLine);
    return time + (target - time) * std::clamp(strength, 0.0, 1.0);
}
