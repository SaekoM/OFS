#pragma once

#include <cstdint>
#include <atomic>
#include <string>

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

    void beginExport(const std::string& outputPath, float start, float end) noexcept;

public:
    static constexpr const char* WindowId = "###PREVIEW_EXPORT";

    void Init() noexcept;
    void ShowWindow(bool* open) noexcept;
};
