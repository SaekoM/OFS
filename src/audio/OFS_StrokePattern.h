#pragma once

#include <cstdint>
#include <string>
#include <vector>

// A stroke pattern laid on the beat grid and repeated to fill a range.
// Each step is a position from 0 (bottom of the chosen depth) to 100 (top),
// or NoAction to place nothing on that step (the line glides through it).
struct StrokePattern
{
    static constexpr int32_t NoAction = -1;
    static constexpr int32_t MaxStepsPerBeat = 4;
    static constexpr int32_t MaxBeats = 8;

    std::string name;
    int32_t stepsPerBeat = 2;    // 1, 2, 3 or 4
    std::vector<int32_t> steps;  // pattern length = steps.size() / stepsPerBeat beats

    int32_t LengthInBeats() const noexcept { return stepsPerBeat > 0 ? (int32_t)steps.size() / stepsPerBeat : 0; }
};
