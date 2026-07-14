#pragma once

#include <cstdint>
#include <atomic>
#include <string>
#include <vector>

// Exports a short animated preview (WebP or AV1) of a chapter or timestamp range
// via ffmpeg. Runs the encode on a background thread so the UI stays responsive.
// (On-video simulator compositing is a planned follow-up, not included here.)
class OFS_PreviewExporter
{
private:
    uint32_t stateHandle = 0xFFFF'FFFFu;
    int selectedChapter = 0;
    float startTime = 0.f;
    float endTime = 0.f;

    std::atomic<bool> exporting{false};
    std::atomic<int> lastResult{0};
    bool hasRun = false;
    std::string lastErrorText; // captured ffmpeg stderr on failure

    // HH:MM:SS.mmm text buffers for the timestamp range fields.
    std::string startStr, endStr;
    bool startEditing = false, endEditing = false;
    bool draggingResize = false; // true while a drag that started in the resize corner is active

    // Incremental sim-frame render job (overlay). Frames are rendered a few per
    // UI frame on the main thread (GL), then ffmpeg runs on a background thread.
    bool renderingFrames = false;
    int jobFrame = 0, jobFrameCount = 0;
    int jobW = 0, jobH = 0;
    float jobStart = 0.f, jobFps = 15.f;
    std::string jobTempDir;
    std::vector<std::string> jobArgs;
    std::vector<uint8_t> jobPixels;

    void startExport(const std::string& outputPath, float start, float end) noexcept;
    void spawnFfmpeg(std::vector<std::string> args, std::string tempDir) noexcept;

public:
    static constexpr const char* WindowId = "###PREVIEW_EXPORT";

    void Init() noexcept;
    void Update() noexcept; // advances the frame-render job; call every frame
    void ShowWindow(bool* open) noexcept;
    // Opens the exporter pre-set to a chapter's range (from the chapter context menu).
    void OpenForChapter(float start, float end) noexcept;
};
