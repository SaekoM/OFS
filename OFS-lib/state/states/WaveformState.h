#pragma once

#include "OFS_StateHandle.h"
#include "OFS_BinarySerialization.h"

#include <vector>
#include <cstdint>
#include <string>

#include "sdefl.h"
#include "sinfl.h"

struct WaveformState
{
    static constexpr auto StateName = "WaveformState";
    // Bump when the meaning of the samples changes, so older caches get rebuilt.
    // 1: peak level per 128-sample slice (before: average level per 300 samples).
    static constexpr int32_t CurrentFormatVersion = 1;

    std::string Filename;
    std::vector<uint8_t> BinSamples;
    size_t UncompressedSize = 0;
    int32_t FormatVersion = 0; // missing in projects saved before versioning

    // Whether this cache holds a waveform for `path` in the current format.
    inline bool IsCurrentFor(const std::string& path) const noexcept
    {
        return Filename == path && FormatVersion == CurrentFormatVersion && UncompressedSize > 0;
    }

    std::vector<float> GetSamples() noexcept
    {
        if(UncompressedSize == 0) 
            return {};
        std::vector<uint8_t> decompressed;
        decompressed.resize(UncompressedSize);

        auto size = sinflate(decompressed.data(), decompressed.size(), BinSamples.data(), BinSamples.size());
        if(size == UncompressedSize)
        {
            std::vector<uint16_t> u16Samples;
            OFS_Binary::Deserialize(decompressed, u16Samples);
            std::vector<float> samples;
            samples.reserve(u16Samples.size());
            for(auto sample : u16Samples)
                samples.emplace_back(sample / (float) std::numeric_limits<uint16_t>::max());
            return samples;
        }
        return {};
    }

    void SetSamples(const std::vector<float>& samples)
    {
        std::vector<uint16_t> u16Samples;
        u16Samples.reserve(samples.size());
        for(auto sample : samples)
            u16Samples.emplace_back((uint16_t)(sample * (float)std::numeric_limits<uint16_t>::max()));

        BinSamples.clear();
        auto size = OFS_Binary::Serialize(BinSamples, u16Samples);
        BinSamples.resize(size);

        UncompressedSize = size;

        std::vector<uint8_t> compressedBin;
        compressedBin.resize(size);

        sdefl ctx = {0};
        auto compressedSize = sdeflate(&ctx, compressedBin.data(), BinSamples.data(), BinSamples.size(), 8);
        compressedBin.resize(compressedSize);
        BinSamples = std::move(compressedBin);
        FormatVersion = CurrentFormatVersion;
    }

    inline static WaveformState& StaticStateSlow() noexcept
    {
        // This shouldn't be done in hot paths but shouldn't be a problem otherwise.
        uint32_t handle = OFS_ProjectState<WaveformState>::Register(StateName);
        return OFS_ProjectState<WaveformState>(handle).Get();
    }
};

REFL_TYPE(WaveformState)
    REFL_FIELD(Filename)
    REFL_FIELD(BinSamples)
    REFL_FIELD(UncompressedSize)
    REFL_FIELD(FormatVersion)
REFL_END