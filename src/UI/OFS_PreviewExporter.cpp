#include "OFS_PreviewExporter.h"
#include "state/PreviewExportState.h"
#include "state/states/ChapterState.h"

#include "OpenFunscripter.h"
#include "OFS_Simulator3D.h"
#include "OFS_Util.h"

#include "subprocess.h"
#include "imgui.h"
#include "stb_sprintf.h"
#include "stb_image_write.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <thread>
#include <vector>

void OFS_PreviewExporter::Init() noexcept
{
    stateHandle = OFS_AppState<PreviewExportState>::Register(PreviewExportState::StateName);
}

void OFS_PreviewExporter::beginExport(const std::string& outputPath, float start, float end) noexcept
{
    if (exporting.load()) return;
    auto& st = PreviewExportState::State(stateHandle);
    auto app = OpenFunscripter::ptr;
    const char* videoPath = app->player->VideoPath();
    if (!videoPath || !*videoPath || end <= start) return;

    // Output dimensions: height fixed, width from the video aspect (even).
    const int outH = st.resolution;
    const uint16_t vw = app->player->VideoWidth();
    const uint16_t vh = app->player->VideoHeight();
    int outW = (vh > 0) ? (int)std::lround((double)vw * outH / (double)vh) : (outH * 16 / 9);
    outW -= (outW & 1);
    if (outW < 2) outW = 2;

    // Optionally render the simulator to a transparent PNG sequence (main thread, GL).
    const bool doOverlay = st.overlaySim && !app->LoadedFunscripts().empty();
    std::string framePattern, tempDir;
    int ox = 0, oy = 0;
    if (doOverlay) {
        int ow = ((int)std::lround(st.simW * outW)) & ~1; if (ow < 2) ow = 2;
        int oh = ((int)std::lround(st.simH * outH)) & ~1; if (oh < 2) oh = 2;
        ox = (int)std::lround(st.simX * outW);
        oy = (int)std::lround(st.simY * outH);

        tempDir = Util::Prefpath("preview_frames");
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::u8path(tempDir), ec);
        Util::CreateDirectories(std::filesystem::u8path(tempDir));

        int frames = (int)std::lround((double)(end - start) * st.fps);
        if (frames < 1) frames = 1;

        auto& sim = app->GetSimulator3D();
        auto& scripts = app->LoadedFunscripts();
        std::vector<uint8_t> pixels;
        for (int i = 0; i < frames; ++i) {
            const float t = start + (float)i / (float)st.fps;
            if (!sim.RenderPoseRGBA(scripts, t, ow, oh, pixels)) break;
            char name[32];
            stbsp_snprintf(name, sizeof(name), "frame_%05d.png", i);
            const auto p = (std::filesystem::u8path(tempDir) / name).u8string();
            stbi_write_png(p.c_str(), ow, oh, 4, pixels.data(), ow * 4);
        }
        framePattern = (std::filesystem::u8path(tempDir) / "frame_%05d.png").u8string();
    }

    const std::string ffmpeg = Util::FfmpegPath().u8string();
    char buf[160];

    std::vector<std::string> a;
    a.push_back(ffmpeg);
    a.push_back("-y");
    a.push_back("-hide_banner");
    a.push_back("-loglevel"); a.push_back("error"); // keep stderr small
    stbsp_snprintf(buf, sizeof(buf), "%.3f", start);       a.push_back("-ss"); a.push_back(buf);
    a.push_back("-i"); a.push_back(videoPath);
    stbsp_snprintf(buf, sizeof(buf), "%.3f", end - start); a.push_back("-t"); a.push_back(buf);

    if (doOverlay) {
        stbsp_snprintf(buf, sizeof(buf), "%d", st.fps); a.push_back("-framerate"); a.push_back(buf);
        a.push_back("-i"); a.push_back(framePattern);
        stbsp_snprintf(buf, sizeof(buf), "[0:v]fps=%d,scale=-2:%d[bg];[bg][1:v]overlay=%d:%d",
                       st.fps, st.resolution, ox, oy);
        a.push_back("-filter_complex"); a.push_back(buf);
    } else {
        stbsp_snprintf(buf, sizeof(buf), "fps=%d,scale=-2:%d:flags=lanczos", st.fps, st.resolution);
        a.push_back("-vf"); a.push_back(buf);
    }
    a.push_back("-an");

    if (st.format == 0) { // animated WebP
        a.push_back("-c:v"); a.push_back("libwebp");
        stbsp_snprintf(buf, sizeof(buf), "%d", st.quality); a.push_back("-q:v"); a.push_back(buf);
        a.push_back("-loop"); a.push_back("0");
    } else { // AV1 (webm)
        const int crf = (int)std::lround((100.0 - st.quality) * 0.63); // quality 100->crf 0, 0->63
        a.push_back("-c:v"); a.push_back("libsvtav1");
        stbsp_snprintf(buf, sizeof(buf), "%d", crf); a.push_back("-crf"); a.push_back(buf);
        a.push_back("-b:v"); a.push_back("0");
    }
    a.push_back(outputPath);

    exporting.store(true);
    hasRun = true;
    std::thread([this, a = std::move(a), tempDir]() mutable {
        std::vector<const char*> argv;
        argv.reserve(a.size() + 1);
        for (auto& s : a) argv.push_back(s.c_str());
        argv.push_back(nullptr);

        int rc = -1;
        struct subprocess_s proc;
        if (subprocess_create(argv.data(), subprocess_option_no_window, &proc) == 0) {
            if (proc.stdout_file) { fclose(proc.stdout_file); proc.stdout_file = nullptr; }
            if (proc.stderr_file) { fclose(proc.stderr_file); proc.stderr_file = nullptr; }
            subprocess_join(&proc, &rc);
            subprocess_destroy(&proc);
        }
        if (!tempDir.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(std::filesystem::u8path(tempDir), ec);
        }
        lastResult.store(rc);
        exporting.store(false);
    }).detach();
}

void OFS_PreviewExporter::ShowWindow(bool* open) noexcept
{
    if (!*open) return;
    auto& st = PreviewExportState::State(stateHandle);
    auto app = OpenFunscripter::ptr;

    ImGui::Begin("Animated Preview Export###PREVIEW_EXPORT", open, ImGuiWindowFlags_None);

    ImGui::Combo("Format", &st.format, "Animated WebP\0AV1 (webm)\0\0");
    if (ImGui::InputInt("Height (px)", &st.resolution)) st.resolution = Util::Clamp(st.resolution, 64, 2160);
    if (ImGui::InputInt("FPS", &st.fps)) st.fps = Util::Clamp(st.fps, 1, 60);
    ImGui::SliderInt("Quality", &st.quality, 0, 100);

    ImGui::Separator();
    ImGui::RadioButton("Chapter", &st.rangeMode, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Timestamps", &st.rangeMode, 1);

    const double duration = app->player->Duration();
    float rStart = 0.f, rEnd = 0.f;
    bool haveRange = false;

    if (st.rangeMode == 0) {
        auto& chapters = ChapterState::StaticStateSlow().chapters;
        if (chapters.empty()) {
            ImGui::TextDisabled("No chapters \xE2\x80\x94 add chapters or switch to Timestamps.");
        } else {
            selectedChapter = Util::Clamp(selectedChapter, 0, (int)chapters.size() - 1);
            if (ImGui::BeginCombo("Chapter", chapters[selectedChapter].name.c_str())) {
                for (int i = 0; i < (int)chapters.size(); ++i) {
                    if (ImGui::Selectable(chapters[i].name.c_str(), i == selectedChapter))
                        selectedChapter = i;
                }
                ImGui::EndCombo();
            }
            rStart = chapters[selectedChapter].startTime;
            rEnd = chapters[selectedChapter].endTime;
            haveRange = true;
        }
    } else {
        ImGui::InputFloat("Start (s)", &startTime);
        ImGui::InputFloat("End (s)", &endTime);
        startTime = Util::Clamp(startTime, 0.f, (float)duration);
        endTime = Util::Clamp(endTime, 0.f, (float)duration);
        rStart = startTime;
        rEnd = endTime;
        haveRange = true;
    }

    // ---- Overlay the simulator on the video (draggable live preview) ----
    ImGui::Separator();
    ImGui::Checkbox("Overlay simulator on video", &st.overlaySim);
    if (st.overlaySim) {
        const uint32_t frameTex = app->player->FrameTexture();
        const uint16_t vw = app->player->VideoWidth();
        const uint16_t vh = app->player->VideoHeight();
        if (frameTex && vw > 0 && vh > 0) {
            ImGui::TextDisabled("Drag to move \xE2\x80\xA2 drag corner to resize");
            float previewW = ImGui::GetContentRegionAvail().x;
            if (previewW > 480.f) previewW = 480.f;
            const float previewH = previewW * (float)vh / (float)vw;
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImGui::Image((ImTextureID)(intptr_t)frameTex, ImVec2(previewW, previewH));
            auto* dl = ImGui::GetWindowDrawList();

            const ImVec2 bMin(origin.x + st.simX * previewW, origin.y + st.simY * previewH);
            const int bw = (int)(st.simW * previewW);
            const int bh = (int)(st.simH * previewH);
            const ImVec2 bMax(bMin.x + bw, bMin.y + bh);

            // Live sim rendered into the box at the current playhead time.
            if (bw > 4 && bh > 4 && !app->LoadedFunscripts().empty()) {
                const uint32_t simTex = app->GetSimulator3D().RenderPoseTexture(
                    app->LoadedFunscripts(), (float)app->player->CurrentTime(), bw, bh);
                dl->AddImage((ImTextureID)(intptr_t)simTex, bMin, bMax, ImVec2(0, 1), ImVec2(1, 0));
            }
            dl->AddRect(bMin, bMax, IM_COL32(0xFF, 0xE0, 0x40, 0xD0), 0.f, 0, 2.f);
            dl->AddTriangleFilled(ImVec2(bMax.x - 12, bMax.y), bMax, ImVec2(bMax.x, bMax.y - 12),
                                  IM_COL32(0xFF, 0xE0, 0x40, 0xF0));

            // Move handle (whole box) + resize handle (corner).
            ImGui::SetCursorScreenPos(bMin);
            ImGui::InvisibleButton("##simMove", ImVec2((float)bw, (float)bh));
            ImGui::SetItemAllowOverlap();
            if (ImGui::IsItemActive()) {
                const ImVec2 d = ImGui::GetIO().MouseDelta;
                st.simX += d.x / previewW;
                st.simY += d.y / previewH;
            }
            ImGui::SetCursorScreenPos(ImVec2(bMax.x - 14.f, bMax.y - 14.f));
            ImGui::InvisibleButton("##simResize", ImVec2(16.f, 16.f));
            if (ImGui::IsItemActive()) {
                const ImVec2 d = ImGui::GetIO().MouseDelta;
                st.simW += d.x / previewW;
                st.simH += d.y / previewH;
            }

            st.simW = Util::Clamp(st.simW, 0.05f, 1.f);
            st.simH = Util::Clamp(st.simH, 0.05f, 1.f);
            st.simX = Util::Clamp(st.simX, 0.f, 1.f - st.simW);
            st.simY = Util::Clamp(st.simY, 0.f, 1.f - st.simH);

            ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + previewH + 4.f));
        } else {
            ImGui::TextDisabled("Load a video to position the overlay.");
        }
        if (app->LoadedFunscripts().empty())
            ImGui::TextDisabled("No script loaded \xE2\x80\x94 nothing to composite.");
    }

    ImGui::Separator();
    const char* videoPath = app->player->VideoPath();
    const bool canExport = !exporting.load() && haveRange && rEnd > rStart && videoPath && *videoPath;

    ImGui::BeginDisabled(!canExport);
    if (ImGui::Button("Export\xE2\x80\xA6")) {
        const char* filter = st.format == 0 ? "*.webp" : "*.webm";
        const char* ext = st.format == 0 ? ".webp" : ".webm";
        std::string defaultName = std::string("preview") + ext;
        Util::SaveFileDialog("Export animated preview", defaultName,
            [this, rStart, rEnd](Util::FileDialogResult& result) {
                if (!result.files.empty()) beginExport(result.files[0], rStart, rEnd);
            },
            { filter });
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (exporting.load()) {
        ImGui::TextDisabled("Exporting\xE2\x80\xA6");
    } else if (hasRun) {
        if (lastResult.load() == 0)
            ImGui::TextColored(ImVec4(0.4f, 1.f, 0.4f, 1.f), "Done");
        else
            ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "ffmpeg failed (%d)", lastResult.load());
    }

    if (st.format == 1)
        ImGui::TextDisabled("AV1 requires an ffmpeg build with libsvtav1.");

    ImGui::End();
}
