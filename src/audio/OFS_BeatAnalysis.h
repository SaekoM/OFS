#pragma once

#include "OFS_Fft.h"
#include "state/ProjectState.h"

#include <cstddef>
#include <vector>

// Music beat analysis: mono audio -> onset envelope -> beats -> tempo map.
// Pure computation; decoding and threading live in OFS_BeatAnalysisJob.
//
// Pipeline:
//   1. OnsetDetector: spectral flux over log-spaced bands. Spikes where notes
//      and drum hits start.
//   2. OnsetStrength: removes slow loudness changes so quiet and loud passages
//      are comparable.
//   3. LocalTempo: autocorrelation tempo estimate every second, smoothed so it
//      only changes when the music really changes (e.g. the next song).
//   4. TrackBeats: dynamic-programming beat tracker (Ellis 2007) that places
//      beats on onsets while following the local tempo.
//   5. FitTempoMap: groups the beats into sections of constant tempo and fits
//      each with least squares, which pins the BPM down far more precisely
//      than any single beat could.
namespace BeatAnalysis
{
    constexpr int SampleRate = 22050;
    constexpr int FftSize = 1024;  // ~46 ms analysis window
    constexpr int HopSize = 256;   // ~11.6 ms between envelope frames
    constexpr double FrameRate = (double)SampleRate / HopSize;

    // The onset envelope peaks slightly before the hit it detects: measured at
    // ~10 ms on synthetic kicks regardless of background level (~8 ms on hats).
    // Envelope frames are shifted later by this much when converted to time.
    constexpr double EnvelopeLeadSeconds = 0.010;

    inline double FrameToSeconds(double frame) noexcept { return frame / FrameRate + EnvelopeLeadSeconds; }

    // Builds the onset envelope as audio streams in, so a long video never has
    // to be held in memory as raw samples.
    class OnsetDetector
    {
    public:
        OnsetDetector() noexcept;

        void AddSamples(const float* samples, size_t count) noexcept;
        // Processes the remaining tail. Call once, after the last AddSamples.
        void Finish() noexcept;

        // Spectral flux per frame. Frame n is centred on sample n * HopSize.
        const std::vector<float>& Flux() const noexcept { return flux; }

    private:
        void processFrame(const float* frameStart) noexcept;
        void processAvailableFrames() noexcept;

        OFS_Fft fft;
        std::vector<float> window;
        std::vector<int> bandFirstBin;
        std::vector<int> bandEndBin;

        std::vector<float> pending;  // samples not yet fully consumed
        size_t readPosition = 0;     // first sample of the next frame in `pending`

        std::vector<float> re;
        std::vector<float> im;
        std::vector<float> bands;
        std::vector<float> previousBands;
        bool hasPreviousFrame = false;

        std::vector<float> flux;
    };

    // Flux with slow loudness changes removed, scaled to unit variance.
    std::vector<float> OnsetStrength(const std::vector<float>& flux) noexcept;
    // Strength mapped into 0..1 for drawing the onset lane.
    std::vector<float> DisplayEnvelope(const std::vector<float>& strength) noexcept;
    // Smoothed tempo estimate in BPM for every envelope frame.
    std::vector<float> LocalTempo(const std::vector<float>& strength) noexcept;
    // Beat times in seconds.
    std::vector<double> TrackBeats(const std::vector<float>& strength, const std::vector<float>& localBpm) noexcept;
    // Sections of constant tempo fitted to the beats. Empty if no steady beat was found.
    std::vector<TempoSection> FitTempoMap(const std::vector<double>& beatTimes) noexcept;
}
