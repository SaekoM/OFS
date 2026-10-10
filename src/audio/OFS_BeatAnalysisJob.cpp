#include "OFS_BeatAnalysisJob.h"
#include "OFS_BeatAnalysis.h"

#include "OFS_Util.h"
#include "subprocess.h"

#include <algorithm>
#include <cstdio>

namespace
{
    // Share of the progress bar spent decoding; the analysis passes fill the rest.
    constexpr float DecodeProgressShare = 0.85f;
    constexpr size_t ReadChunkSamples = 8192;
}

OFS_BeatAnalysisJob::~OFS_BeatAnalysisJob() noexcept
{
    Cancel();
}

void OFS_BeatAnalysisJob::Start(const std::string& mediaPath, double durationSeconds) noexcept
{
    Cancel();
    cancelRequested = false;
    progress = 0.f;
    {
        std::lock_guard lock(mutex);
        runningPath = mediaPath;
        lastError.clear();
    }
    running = true;
    worker = std::thread(&OFS_BeatAnalysisJob::run, this, mediaPath, durationSeconds);
}

void OFS_BeatAnalysisJob::Cancel() noexcept
{
    cancelRequested = true;
    if (worker.joinable()) worker.join();
    running = false;
}

std::string OFS_BeatAnalysisJob::RunningPath() const noexcept
{
    std::lock_guard lock(mutex);
    return runningPath;
}

std::string OFS_BeatAnalysisJob::LastError() const noexcept
{
    std::lock_guard lock(mutex);
    return lastError;
}

std::shared_ptr<const OFS_BeatAnalysisJob::Result> OFS_BeatAnalysisJob::LatestResult() const noexcept
{
    std::lock_guard lock(mutex);
    return latestResult;
}

void OFS_BeatAnalysisJob::fail(const std::string& message) noexcept
{
    {
        std::lock_guard lock(mutex);
        lastError = message;
    }
    running = false;
}

void OFS_BeatAnalysisJob::run(std::string mediaPath, double durationSeconds) noexcept
{
    const std::string ffmpeg = Util::FfmpegPath().u8string();
    if (!Util::FileExists(ffmpeg)) {
        fail("ffmpeg was not found.");
        return;
    }

    // Mono float samples at the analysis rate, written raw to stdout.
    const std::string sampleRate = std::to_string(BeatAnalysis::SampleRate);
    const char* args[] = {
        ffmpeg.c_str(),
        "-nostdin",
        "-loglevel", "quiet",
        "-i", mediaPath.c_str(),
        "-vn",
        "-ac", "1",
        "-ar", sampleRate.c_str(),
        "-f", "f32le",
        "pipe:1",
        nullptr
    };

    struct subprocess_s process;
    if (subprocess_create(args, subprocess_option_no_window, &process) != 0) {
        fail("Could not start ffmpeg.");
        return;
    }
    // Nothing is read from stderr ("-loglevel quiet"), so close it up front.
    if (process.stderr_file) {
        fclose(process.stderr_file);
        process.stderr_file = nullptr;
    }

    BeatAnalysis::OnsetDetector detector;
    std::vector<float> chunk(ReadChunkSamples);
    const double expectedSamples = std::max(1.0, durationSeconds * BeatAnalysis::SampleRate);
    size_t samplesRead = 0;
    FILE* audio = subprocess_stdout(&process);
    while (audio) {
        if (cancelRequested) {
            subprocess_terminate(&process);
            break;
        }
        const size_t count = fread(chunk.data(), sizeof(float), chunk.size(), audio);
        if (count == 0) break;
        detector.AddSamples(chunk.data(), count);
        samplesRead += count;
        progress = DecodeProgressShare * (float)std::min(1.0, samplesRead / expectedSamples);
    }
    // Close stdout ourselves: subprocess_destroy would fclose the (already
    // closed, now null) stderr handle whenever stdout is still open, and crash.
    if (process.stdout_file) {
        fclose(process.stdout_file);
        process.stdout_file = nullptr;
    }
    int exitCode = 0;
    subprocess_join(&process, &exitCode);
    subprocess_destroy(&process);

    if (cancelRequested) {
        running = false;
        return;
    }
    if (samplesRead == 0) {
        fail("No audio track could be decoded.");
        return;
    }

    detector.Finish();
    const auto strength = BeatAnalysis::OnsetStrength(detector.Flux());
    if (cancelRequested) { running = false; return; }

    const auto localBpm = BeatAnalysis::LocalTempo(strength);
    progress = 0.92f;
    if (cancelRequested) { running = false; return; }

    const auto beats = BeatAnalysis::TrackBeats(strength, localBpm);
    progress = 0.97f;

    auto result = std::make_shared<Result>();
    result->mediaPath = mediaPath;
    result->frameRate = BeatAnalysis::FrameRate;
    result->envelopeStartSeconds = BeatAnalysis::FrameToSeconds(0.0);
    result->onsetEnvelope = BeatAnalysis::DisplayEnvelope(strength);
    result->tempoMap = BeatAnalysis::FitTempoMap(beats);

    {
        std::lock_guard lock(mutex);
        latestResult = std::move(result);
    }
    progress = 1.f;
    running = false;
}
