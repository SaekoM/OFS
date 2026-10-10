#pragma once

#include "state/ProjectState.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Runs beat analysis for one media file on a background thread. The bundled
// ffmpeg decodes the audio and streams it into BeatAnalysis; the finished
// result is handed to the UI through LatestResult().
class OFS_BeatAnalysisJob
{
public:
    struct Result
    {
        std::string mediaPath;
        double frameRate = 0.0;
        double envelopeStartSeconds = 0.0;  // time of envelope frame 0
        std::vector<float> onsetEnvelope;   // 0..1 per frame, drawn as the onset lane
        std::vector<TempoSection> tempoMap; // empty when no steady beat was found
    };

    OFS_BeatAnalysisJob() noexcept = default;
    OFS_BeatAnalysisJob(const OFS_BeatAnalysisJob&) = delete;
    OFS_BeatAnalysisJob& operator=(const OFS_BeatAnalysisJob&) = delete;
    ~OFS_BeatAnalysisJob() noexcept;

    // Starts analysing `mediaPath`, cancelling any analysis still running.
    void Start(const std::string& mediaPath, double durationSeconds) noexcept;
    // Stops a running analysis and waits for the worker to exit.
    void Cancel() noexcept;

    bool IsRunning() const noexcept { return running; }
    float Progress() const noexcept { return progress; }
    std::string RunningPath() const noexcept;
    std::string LastError() const noexcept;
    // The most recent finished analysis, or null if none has finished.
    std::shared_ptr<const Result> LatestResult() const noexcept;

private:
    void run(std::string mediaPath, double durationSeconds) noexcept;
    void fail(const std::string& message) noexcept;

    std::thread worker;
    std::atomic<bool> running{ false };
    std::atomic<bool> cancelRequested{ false };
    std::atomic<float> progress{ 0.f };

    mutable std::mutex mutex; // guards the fields below
    std::string runningPath;
    std::string lastError;
    std::shared_ptr<const Result> latestResult;
};
