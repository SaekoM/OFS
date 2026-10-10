#include "OFS_ScriptPositionsOverlays.h"
#include "OpenFunscripter.h"
#include "OFS_ImGui.h"

#include "state/ProjectState.h"
#include "state/PatternFillState.h"
#include "state/states/ChapterState.h"
#include "audio/OFS_TempoMap.h"

#include "imgui_stdlib.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <limits>

void FrameOverlay::DrawScriptPositionContent(const OverlayDrawingCtx& ctx) noexcept
{
    auto app = OpenFunscripter::ptr;
    float fps = enableFpsOverride ? fpsOverride : app->player->Fps();
    float frameTime = enableFpsOverride ? (1.f / fpsOverride) : app->scripting->LogicalFrameTime();
    float visibleFrames = ctx.visibleTime / frameTime;
    constexpr float maxVisibleFrames = 400.f;

    if (visibleFrames <= (maxVisibleFrames * 0.75f)) {
        //render frame dividers
        float offset = -std::fmod(ctx.offsetTime, frameTime);
        const int lineCount = visibleFrames + 2;
        int alpha = 255 * (1.f - (visibleFrames / maxVisibleFrames));
        for (int i = 0; i < lineCount; i++) {
            ctx.drawList->AddLine(
                ctx.canvasPos + ImVec2(((offset + (i * frameTime)) / ctx.visibleTime) * ctx.canvasSize.x, 0.f),
                ctx.canvasPos + ImVec2(((offset + (i * frameTime)) / ctx.visibleTime) * ctx.canvasSize.x, ctx.canvasSize.y),
                IM_COL32(80, 80, 80, alpha),
                1.f
            );
        }
    }

    // time dividers
    constexpr float maxVisibleTimeDividers = 150.f;
    const float timeIntervalMs = std::round(fps * 0.1f) * frameTime;
    const float visibleTimeIntervals = ctx.visibleTime / timeIntervalMs;
    if (visibleTimeIntervals <= (maxVisibleTimeDividers * 0.8f)) {
        float offset = -std::fmod(ctx.offsetTime, timeIntervalMs);
        const int lineCount = visibleTimeIntervals + 2;
        int alpha = 255 * (1.f - (visibleTimeIntervals / maxVisibleTimeDividers));
        for (int i = 0; i < lineCount; i++) {
            ctx.drawList->AddLine(
                ctx.canvasPos + ImVec2(((offset + (i * timeIntervalMs)) / ctx.visibleTime) * ctx.canvasSize.x, 0.f),
                ctx.canvasPos + ImVec2(((offset + (i * timeIntervalMs)) / ctx.visibleTime) * ctx.canvasSize.x, ctx.canvasSize.y),
                IM_COL32(80, 80, 80, alpha),
                3.f
            );
        }
    }
    BaseOverlay::DrawHeightLines(ctx);
    timeline->DrawAudioWaveform(ctx);
    BaseOverlay::DrawActionLines(ctx);
    BaseOverlay::DrawActionPoints(ctx);
    BaseOverlay::DrawSecondsLabel(ctx);
    BaseOverlay::DrawScriptLabel(ctx);

    // out of sync line
    auto& state = BaseOverlay::State();
    if (state.SyncLineEnable) {
        float realFrameTime = app->player->CurrentPlayerTime() - ctx.offsetTime;
        ctx.drawList->AddLine(
            ctx.canvasPos + ImVec2((realFrameTime / ctx.visibleTime) * ctx.canvasSize.x, 0.f),
            ctx.canvasPos + ImVec2((realFrameTime / ctx.visibleTime) * ctx.canvasSize.x, ctx.canvasSize.y),
            IM_COL32(255, 0, 0, 255),
            1.f
        );
    }
}

void FrameOverlay::nextFrame(float realFrameTime) noexcept
{
    auto app = OpenFunscripter::ptr;
    if(enableFpsOverride) {
        app->player->SeekRelative(1.f / fpsOverride);
    }
    else {
        app->player->NextFrame();
    }
}

void FrameOverlay::previousFrame(float realFrameTime) noexcept
{
    auto app = OpenFunscripter::ptr;
    if(enableFpsOverride) {
        app->player->SeekRelative(-(1.f / fpsOverride));
    }
    else {
        app->player->PreviousFrame();
    }
}

float FrameOverlay::steppingIntervalBackward(float realFrameTime, float fromTime) noexcept
{
    return -logicalFrameTime(realFrameTime);
}

float FrameOverlay::steppingIntervalForward(float realFrameTime, float fromTime) noexcept
{
    return logicalFrameTime(realFrameTime);
}

float FrameOverlay::logicalFrameTime(float realFrameTime) noexcept
{
    return enableFpsOverride ? (1.f / fpsOverride) : realFrameTime;
}

void FrameOverlay::DrawSettings() noexcept
{
    if(ImGui::Checkbox(TR_ID("FPS_OVERRIDE_ENABLE", Tr::FPS_OVERRIDE), &enableFpsOverride))
    {
        fpsOverride = OpenFunscripter::ptr->player->Fps();
    }
    if(enableFpsOverride) {
        if(ImGui::InputFloat(TR_ID("FPS_OVERRIDE", Tr::FPS_OVERRIDE), &fpsOverride, 1.f, 10.f))
        {
            fpsOverride = Util::Clamp(fpsOverride, 1.f, 150.f);
            // snap to new framerate
            auto app = OpenFunscripter::ptr;
            float newPosition = std::round(app->player->CurrentTime() / (1.0 / (double)fpsOverride))
                * (1.0 / (double)fpsOverride);
            app->player->SetPositionExact(newPosition, true);
        }
    }
}

// ---------------------------------------------------------------------------
// TempoOverlay

bool TempoOverlay::ShowOnsetLane = true;
bool TempoOverlay::ApplyDetectionWhenReady = false;
std::vector<TempoSection> TempoOverlay::SectionsBeforeDetection;
std::string TempoOverlay::SectionsBeforeDetectionMedia;
std::string TempoOverlay::DetectionMessage;
TempoOverlay::LastFillInfo TempoOverlay::LastFill;
std::weak_ptr<Funscript> TempoOverlay::LastSnapScript;
std::string TempoOverlay::FillMessage;

namespace
{
    constexpr uint32_t OnsetLaneColor = IM_COL32(64, 200, 255, 130);
    constexpr uint32_t SectionStartColor = IM_COL32(255, 170, 0, 230);
    constexpr uint32_t BeatLineColor = IM_COL32(255, 255, 255, 153);
    constexpr uint32_t SubdivisionLineColor = IM_COL32(255, 255, 255, 70);
    constexpr uint32_t ErrorTextColor = IM_COL32(255, 90, 90, 255);

    constexpr uint32_t PatternLineColor = IM_COL32(64, 200, 255, 255);
    constexpr uint32_t PatternBeatTickColor = IM_COL32(255, 255, 255, 60);

    // Grids denser than this are thinned to beats, then to measures.
    constexpr float MinGridLineSpacingPx = 4.f;

    // A fill replaces [from, to); an action exactly at `to` belongs to what follows.
    constexpr double FillEndExclusionSeconds = 0.0005;
    // Snapped actions closer than this to another action are dropped instead of stacked.
    constexpr float SnapCollisionSeconds = 0.001f;

    // Pattern editor step slider size, in multiples of the frame height.
    constexpr float StepSliderWidthShare = 0.9f;
    constexpr float StepSliderHeightShare = 4.f;
    constexpr float OnsetColumnWidthPx = 2.f;
    constexpr float OnsetMinVisible = 0.02f;
    constexpr float OnsetLaneHeightShare = 0.9f;

    const char* CurrentMediaPath() noexcept
    {
        const char* path = OpenFunscripter::ptr->player->VideoPath();
        return path ? path : "";
    }

    // The finished analysis of the media loaded right now, or null.
    std::shared_ptr<const OFS_BeatAnalysisJob::Result> CurrentAnalysis() noexcept
    {
        auto result = OpenFunscripter::ptr->beatAnalysis->LatestResult();
        if (result && result->mediaPath == CurrentMediaPath()) return result;
        return nullptr;
    }

    bool AnalysingCurrentMedia() noexcept
    {
        const auto& job = *OpenFunscripter::ptr->beatAnalysis;
        return job.IsRunning() && job.RunningPath() == CurrentMediaPath();
    }

    // Whether the most recent attempt to analyse the loaded media failed.
    bool AnalysisFailedForCurrentMedia() noexcept
    {
        const auto& job = *OpenFunscripter::ptr->beatAnalysis;
        return !job.IsRunning() && !job.LastError().empty() && job.RunningPath() == CurrentMediaPath();
    }

    void StartAnalysisOfCurrentMedia() noexcept
    {
        auto app = OpenFunscripter::ptr;
        app->beatAnalysis->Start(CurrentMediaPath(), app->player->Duration());
    }

    // Section boundary as hh:mm:ss.mmm, or "end" for the open end of the last section.
    std::string FormatBoundary(double seconds) noexcept
    {
        if (std::isinf(seconds)) return "end";
        char buffer[32];
        Util::FormatTime(buffer, sizeof(buffer), (float)seconds, true);
        return buffer;
    }

    bool HasActionNear(const FunscriptArray& actions, float time) noexcept
    {
        auto it = std::lower_bound(actions.begin(), actions.end(),
            FunscriptAction(time - SnapCollisionSeconds, 0), ActionLess());
        return it != actions.end() && it->atS <= time + SnapCollisionSeconds;
    }

    // Resizes a pattern to `beats` beats of `stepsPerBeat` steps, keeping every
    // step that still lines up with a step in the new layout.
    void ReshapePattern(StrokePattern& pattern, int32_t beats, int32_t stepsPerBeat) noexcept
    {
        std::vector<int32_t> reshaped((size_t)beats * stepsPerBeat, StrokePattern::NoAction);
        const int32_t oldBeats = pattern.LengthInBeats();
        for (int32_t beat = 0; beat < std::min(beats, oldBeats); beat += 1) {
            for (int32_t step = 0; step < stepsPerBeat; step += 1) {
                if ((step * pattern.stepsPerBeat) % stepsPerBeat != 0) continue;
                const int32_t oldStep = step * pattern.stepsPerBeat / stepsPerBeat;
                reshaped[(size_t)beat * stepsPerBeat + step] = pattern.steps[(size_t)beat * pattern.stepsPerBeat + oldStep];
            }
        }
        pattern.stepsPerBeat = stepsPerBeat;
        pattern.steps = std::move(reshaped);
    }

    // Chapters the last detection created, so "Undo detection" can remove them
    // again as long as nobody has edited them since.
    std::vector<Chapter> ChaptersCreatedByDetection;

    // "128 BPM", or "127.3 BPM" for a tempo that isn't a whole number.
    std::string BpmLabel(float bpm) noexcept
    {
        constexpr float WholeNumberTolerance = 0.05f;
        char buffer[32];
        const float rounded = std::round(bpm);
        if (std::abs(bpm - rounded) < WholeNumberTolerance) stbsp_snprintf(buffer, sizeof(buffer), "%d BPM", (int)rounded);
        else stbsp_snprintf(buffer, sizeof(buffer), "%.1f BPM", bpm);
        return buffer;
    }

    // One chapter per tempo section, named after its tempo. Each ends just before
    // the next one starts: OFS counts chapters sharing a boundary as overlapping.
    std::vector<Chapter> ChaptersForSections(const std::vector<TempoSection>& sections, double mediaDuration) noexcept
    {
        std::vector<Chapter> chapters;
        for (int i = 0; i < (int)sections.size(); i += 1) {
            const float start = sections[i].startSeconds;
            const float end = (float)std::min(TempoMap::SectionEnd(sections, i), mediaDuration);
            if (end <= start) continue;

            Chapter chapter;
            chapter.startTime = start;
            chapter.endTime = i + 1 < (int)sections.size() ? std::nextafter(end, start) : end;
            chapter.name = BpmLabel(sections[i].bpm);
            chapter.color = Util::RandomColor(0.65f, 0.70f);
            chapters.push_back(std::move(chapter));
        }
        return chapters;
    }

    bool SameChapters(const std::vector<Chapter>& a, const std::vector<Chapter>& b) noexcept
    {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i += 1) {
            if (a[i].startTime != b[i].startTime || a[i].endTime != b[i].endTime || a[i].name != b[i].name) return false;
        }
        return true;
    }
}

TempoOverlay::TempoOverlay(ScriptTimeline* timeline) noexcept
    : BaseOverlay(timeline)
{
    stateHandle = OFS_ProjectState<TempoOverlayState>::Register(TempoOverlayState::StateName);
    patternStateHandle = OFS_AppState<PatternFillState>::Register(PatternFillState::StateName);
}

std::vector<TempoSection>& TempoOverlay::sections() noexcept
{
    auto& tempo = TempoOverlayState::State(stateHandle);
    if (tempo.sections.empty()) {
        // Projects saved before tempo maps hold one BPM and offset.
        TempoSection legacy;
        legacy.bpm = tempo.bpm;
        legacy.anchorSeconds = tempo.beatOffsetSeconds;
        tempo.sections.push_back(legacy);
        TempoMap::Normalize(tempo.sections);
    }
    return tempo.sections;
}

double TempoOverlay::beatsPerLine() noexcept
{
    auto& tempo = TempoOverlayState::State(stateHandle);
    tempo.measureIndex = std::min<uint32_t>(tempo.measureIndex, (uint32_t)beatMultiples.size() - 1);
    return beatMultiples[tempo.measureIndex];
}

void TempoOverlay::applyDetectedTempoMap(const std::vector<TempoSection>& detected) noexcept
{
    if (detected.empty()) {
        DetectionMessage = "No steady beat found; the tempo map was left unchanged.";
        return;
    }
    auto& map = sections();
    SectionsBeforeDetection = map;
    SectionsBeforeDetectionMedia = CurrentMediaPath();
    map = detected;
    TempoMap::Normalize(map);
    DetectionMessage = map.size() == 1
        ? std::string("Detected 1 section.")
        : "Detected " + std::to_string(map.size()) + " sections.";

    // Name a chapter after each section's tempo, but never touch existing chapters.
    auto& chapterState = ChapterState::StaticStateSlow();
    ChaptersCreatedByDetection.clear();
    if (!chapterState.chapters.empty()) {
        DetectionMessage += " Existing chapters were left as they are.";
        return;
    }
    chapterState.chapters = ChaptersForSections(map, OpenFunscripter::ptr->player->Duration());
    if (chapterState.chapters.empty()) return;
    ChaptersCreatedByDetection = chapterState.chapters;
    EV::Enqueue<ChapterStateChanged>();
    DetectionMessage += chapterState.chapters.size() == 1
        ? std::string(" Added 1 chapter.")
        : " Added " + std::to_string(chapterState.chapters.size()) + " chapters.";
}

void TempoOverlay::update() noexcept
{
    BaseOverlay::update();
    auto app = OpenFunscripter::ptr;
    if (*CurrentMediaPath() == '\0' || app->player->Duration() <= 0.0) return;

    // Analyse the loaded media whenever the onset lane or a detection needs it.
    const auto analysis = CurrentAnalysis();
    const bool needsAnalysis = ShowOnsetLane || ApplyDetectionWhenReady;
    if (needsAnalysis && !analysis && !AnalysingCurrentMedia() && !AnalysisFailedForCurrentMedia()) {
        StartAnalysisOfCurrentMedia();
    }

    if (ApplyDetectionWhenReady) {
        if (analysis) {
            ApplyDetectionWhenReady = false;
            applyDetectedTempoMap(analysis->tempoMap);
        }
        else if (AnalysisFailedForCurrentMedia()) {
            ApplyDetectionWhenReady = false; // the error is shown in the settings
        }
    }
}

void TempoOverlay::DrawSettings() noexcept
{
    BaseOverlay::DrawSettings();
    auto& tempo = TempoOverlayState::State(stateHandle);
    beatsPerLine(); // keeps measureIndex in range

    if (ImGui::BeginCombo(TR(SNAP), TRD(beatMultiplesStrings[tempo.measureIndex]), ImGuiComboFlags_PopupAlignLeft)) {
        for (int i = 0; i < beatMultiples.size(); i++) {
            if (ImGui::Selectable(TRD(beatMultiplesStrings[i]))) {
                tempo.measureIndex = i;
            }
            else if (ImGui::IsItemHovered()) {
                tempo.measureIndex = i;
            }
        }
        ImGui::EndCombo();
    }
    OFS::Tooltip("Grid spacing for the timeline lines, beat stepping and Snap to grid.");

    if (ImGui::CollapsingHeader("Tempo map", ImGuiTreeNodeFlags_DefaultOpen)) {
        drawDetectionControls();
        ImGui::Separator();
        drawSectionEditor();
        drawSectionList();
    }
    if (ImGui::CollapsingHeader("Pattern fill")) {
        drawPatternFill();
    }
    if (ImGui::CollapsingHeader("Snap to grid")) {
        drawSnapToGrid();
    }
}

void TempoOverlay::drawDetectionControls() noexcept
{
    const bool hasMedia = *CurrentMediaPath() != '\0';
    ImGui::BeginDisabled(ApplyDetectionWhenReady || !hasMedia);
    if (ImGui::Button("Detect tempo map")) {
        DetectionMessage.clear();
        if (auto analysis = CurrentAnalysis()) {
            applyDetectedTempoMap(analysis->tempoMap);
        }
        else {
            ApplyDetectionWhenReady = true;
            // Also retries an attempt that failed.
            if (!AnalysingCurrentMedia()) StartAnalysisOfCurrentMedia();
        }
    }
    ImGui::EndDisabled();
    OFS::Tooltip("Finds the beats in the audio and replaces the tempo map,\nsplitting it wherever the song or tempo changes.");

    if (hasMedia && SectionsBeforeDetectionMedia == CurrentMediaPath()) {
        ImGui::SameLine();
        if (ImGui::Button("Undo detection")) {
            sections() = SectionsBeforeDetection;
            SectionsBeforeDetectionMedia.clear();
            DetectionMessage.clear();

            // Remove the chapters detection added, unless they've been edited since.
            auto& chapterState = ChapterState::StaticStateSlow();
            if (!ChaptersCreatedByDetection.empty() && SameChapters(chapterState.chapters, ChaptersCreatedByDetection)) {
                chapterState.chapters.clear();
                EV::Enqueue<ChapterStateChanged>();
            }
            ChaptersCreatedByDetection.clear();
        }
    }

    ImGui::Checkbox("Onset lane", &ShowOnsetLane);
    OFS::Tooltip("Shows where notes and drum hits start, in place of the waveform.\nGrid lines should sit on the spikes.");

    if (AnalysingCurrentMedia()) {
        ImGui::ProgressBar(OpenFunscripter::ptr->beatAnalysis->Progress(), ImVec2(-1.f, 0.f), "Analyzing audio...");
    }
    else if (AnalysisFailedForCurrentMedia()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ErrorTextColor);
        ImGui::TextWrapped("%s", OpenFunscripter::ptr->beatAnalysis->LastError().c_str());
        ImGui::PopStyleColor();
    }
    else if (!DetectionMessage.empty()) {
        ImGui::TextDisabled("%s", DetectionMessage.c_str());
    }
}

void TempoOverlay::drawSectionEditor() noexcept
{
    auto app = OpenFunscripter::ptr;
    auto& map = sections();
    const double now = app->player->CurrentTime();
    const int index = TempoMap::SectionIndexAt(map, now);
    auto& section = map[index];

    ImGui::Text("Section %d of %d  (%s - %s)", index + 1, (int)map.size(),
        FormatBoundary(section.startSeconds).c_str(), FormatBoundary(TempoMap::SectionEnd(map, index)).c_str());

    float bpm = section.bpm;
    if (ImGui::InputFloat(TR(BPM), &bpm, 0.01f, 1.f, "%.3f")) {
        TempoMap::SetBpmKeepingDownbeat(section, bpm, now);
    }
    if (ImGui::Button("x2")) TempoMap::SetBpmKeepingDownbeat(section, section.bpm * 2.0, now);
    OFS::Tooltip("Double the tempo (it was detected at half speed).");
    ImGui::SameLine();
    if (ImGui::Button("/2")) TempoMap::SetBpmKeepingDownbeat(section, section.bpm / 2.0, now);
    OFS::Tooltip("Halve the tempo (it was detected at double speed).");

    float offsetMs = (section.anchorSeconds - section.startSeconds) * 1000.f;
    if (ImGui::DragFloat(TR(OFFSET), &offsetMs, 0.5f, 0.f, 0.f, "%.1f ms")) {
        section.anchorSeconds = section.startSeconds + offsetMs / 1000.f;
        TempoMap::WrapAnchor(section);
    }
    OFS::Tooltip("Slides the grid. Drag until the lines sit on the onset spikes.");

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Downbeat");
    ImGui::SameLine();
    if (ImGui::ArrowButton("##DownbeatEarlier", ImGuiDir_Left)) TempoMap::ShiftDownbeat(section, -1);
    OFS::Tooltip("Make the previous beat beat 1 of the measure.");
    ImGui::SameLine();
    if (ImGui::ArrowButton("##DownbeatLater", ImGuiDir_Right)) TempoMap::ShiftDownbeat(section, 1);
    OFS::Tooltip("Make the next beat beat 1 of the measure.");
    ImGui::SameLine();
    if (ImGui::Button("Set at playhead")) {
        section.anchorSeconds = (float)now;
        TempoMap::WrapAnchor(section);
    }
    OFS::Tooltip("Pause on the first beat of a measure and click: beat 1 moves here.");

    ImGui::Text("%s: %.2fms", TR(INTERVAL), (float)(TempoMap::BeatPeriod(section) * beatsPerLine() * 1000.0));

    // Structural edits come last because they invalidate `section`.
    if (ImGui::Button("Split at playhead")) {
        TempoMap::SplitAt(map, now);
        return;
    }
    OFS::Tooltip("Starts a new section here, e.g. where the next song begins.\nBoth halves keep the current grid until you change one.");
    ImGui::SameLine();
    ImGui::BeginDisabled(index == 0);
    if (ImGui::Button("Merge with previous")) TempoMap::MergeWithPrevious(map, index);
    ImGui::EndDisabled();
}

void TempoOverlay::drawSectionList() noexcept
{
    auto app = OpenFunscripter::ptr;
    auto& map = sections();
    if (map.size() < 2 || !ImGui::TreeNode("All sections")) return;

    const int current = TempoMap::SectionIndexAt(map, app->player->CurrentTime());
    char label[64];
    for (int i = 0; i < (int)map.size(); i += 1) {
        stbsp_snprintf(label, sizeof(label), "%d.  %s   %.2f BPM", i + 1, FormatBoundary(map[i].startSeconds).c_str(), map[i].bpm);
        ImGui::PushID(i);
        if (ImGui::Selectable(label, i == current)) {
            // Jump to the section's first beat rather than its exact boundary.
            const double firstBeat = TempoMap::NextGridTime(map, map[i].startSeconds - 0.002, 1.0);
            app->player->SetPositionExact((float)firstBeat);
        }
        ImGui::PopID();
    }
    ImGui::TreePop();
}

// ---------------------------------------------------------------------------
// Pattern fill & snap

const StrokePattern& TempoOverlay::selectedPattern() noexcept
{
    auto& fill = PatternFillState::State(patternStateHandle);
    const auto& builtIns = PatternFill::BuiltInPatterns();
    const int32_t patternCount = (int32_t)(builtIns.size() + fill.customPatterns.size());
    fill.patternIndex = std::clamp(fill.patternIndex, 0, patternCount - 1);
    if (fill.patternIndex < (int32_t)builtIns.size()) return builtIns[fill.patternIndex];
    return fill.customPatterns[fill.patternIndex - builtIns.size()];
}

PatternFill::TimeRange TempoOverlay::currentFillRange(const char** problem) noexcept
{
    auto app = OpenFunscripter::ptr;
    auto& fill = PatternFillState::State(patternStateHandle);
    const double playhead = app->player->CurrentTime();
    const double duration = app->player->Duration();
    *problem = nullptr;

    switch (fill.rangeMode) {
        case FillWholeSection:
            return PatternFill::RangeOfSection(sections(), playhead, duration);
        case FillSelectionSpan: {
            const auto& selection = app->ActiveFunscript()->Selection();
            if (selection.size() < 2) {
                *problem = "Select at least two actions to mark the span.";
                return {};
            }
            return { selection[0].atS, selection[selection.size() - 1].atS };
        }
        default:
            return PatternFill::RangeFromPlayhead(sections(), playhead, fill.measures, duration);
    }
}

void TempoOverlay::applyPatternFill(const PatternFill::TimeRange& range) noexcept
{
    auto app = OpenFunscripter::ptr;
    auto& fill = PatternFillState::State(patternStateHandle);
    auto& script = app->ActiveFunscript();
    const auto planned = PatternFill::Generate(sections(), selectedPattern(), range.from, range.to, fill.bottom, fill.top);

    app->undoSystem->Snapshot(StateType::PATTERN_FILL, script);
    script->RemoveActionsInInterval((float)range.from, (float)(range.to - FillEndExclusionSeconds));
    FunscriptArray added;
    for (const auto& action : planned) added.emplace(FunscriptAction((float)action.time, action.pos));
    script->AddMultipleActions(added);

    // Select what was written. For a selection span, keep the closing action
    // selected too so the span stays the same for the next fill.
    FunscriptArray selection = added;
    if (fill.rangeMode == FillSelectionSpan) {
        const auto& actions = script->Actions();
        auto end = std::lower_bound(actions.begin(), actions.end(), FunscriptAction((float)range.to, 0), ActionLess());
        if (end != actions.end() && end->atS == (float)range.to) selection.emplace(*end);
    }
    script->SetSelection(selection);

    LastFill = { script, range };
    FillMessage = "Filled " + std::to_string(added.size()) + " actions.";
}

void TempoOverlay::refreshLastFill() noexcept
{
    auto app = OpenFunscripter::ptr;
    auto script = LastFill.script.lock();
    if (!script || script != app->ActiveFunscript() || !app->undoSystem->MatchUndoTop(StateType::PATTERN_FILL)) return;
    app->Undo();
    applyPatternFill(LastFill.range);
}

void TempoOverlay::applySnap() noexcept
{
    auto app = OpenFunscripter::ptr;
    auto& fill = PatternFillState::State(patternStateHandle);
    auto& script = app->ActiveFunscript();
    if (!script->HasSelection()) return;

    const auto& map = sections();
    const double strength = fill.snapStrength / 100.0;
    const double gridBeats = beatsPerLine();

    app->undoSystem->Snapshot(StateType::SNAP_TO_GRID, script);
    const FunscriptArray original = script->Selection();
    script->RemoveSelectedActions();

    // When a snapped action lands on another one, the one already there wins.
    FunscriptArray snapped;
    for (const auto& action : original) {
        const float time = (float)PatternFill::SnapTime(map, action.atS, gridBeats, strength);
        if (HasActionNear(script->Actions(), time) || HasActionNear(snapped, time)) continue;
        snapped.emplace(FunscriptAction(time, action.pos));
    }
    script->AddMultipleActions(snapped);
    script->SetSelection(snapped);
    LastSnapScript = script;
}

void TempoOverlay::refreshLastSnap() noexcept
{
    auto app = OpenFunscripter::ptr;
    auto script = LastSnapScript.lock();
    if (!script || script != app->ActiveFunscript() || !app->undoSystem->MatchUndoTop(StateType::SNAP_TO_GRID)) return;
    app->Undo();
    applySnap();
}

void TempoOverlay::drawPatternFill() noexcept
{
    auto& fill = PatternFillState::State(patternStateHandle);
    const auto& builtIns = PatternFill::BuiltInPatterns();
    bool patternChanged = false;

    const StrokePattern& current = selectedPattern();
    if (ImGui::BeginCombo("Pattern", current.name.c_str())) {
        for (int32_t i = 0; i < (int32_t)builtIns.size(); i += 1) {
            if (ImGui::Selectable(builtIns[i].name.c_str(), fill.patternIndex == i)) {
                fill.patternIndex = i;
                patternChanged = true;
            }
        }
        if (!fill.customPatterns.empty()) ImGui::Separator();
        for (int32_t i = 0; i < (int32_t)fill.customPatterns.size(); i += 1) {
            const int32_t index = (int32_t)builtIns.size() + i;
            ImGui::PushID(index);
            if (ImGui::Selectable(fill.customPatterns[i].name.c_str(), fill.patternIndex == index)) {
                fill.patternIndex = index;
                patternChanged = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    drawPatternPreview(selectedPattern());

    if (ImGui::DragIntRange2("Depth", &fill.bottom, &fill.top, 0.5f, 0, 100, "Bottom %d", "Top %d", ImGuiSliderFlags_AlwaysClamp)) {
        patternChanged = true;
    }
    OFS::Tooltip("Positions the pattern's 0 and 100 map to.");

    ImGui::Combo("Range", &fill.rangeMode, "From playhead\0Whole section\0Selection span\0\0");
    OFS::Tooltip("From playhead: N measures from the nearest beat, stopping at the end of the song.\n"
        "Whole section: the entire song under the playhead.\n"
        "Selection span: from the first to the last selected action.");
    if (fill.rangeMode == FillFromPlayhead) {
        if (ImGui::InputInt("Measures", &fill.measures, 1, 4)) fill.measures = std::clamp(fill.measures, 1, 256);
    }

    const char* problem = nullptr;
    const auto range = currentFillRange(&problem);
    ImGui::BeginDisabled(range.IsEmpty());
    if (ImGui::Button("Fill")) applyPatternFill(range);
    ImGui::EndDisabled();
    OFS::Tooltip("Replaces the actions in the range on the active script.\nChanging the pattern or depth right after updates the fill.");
    ImGui::SameLine();
    if (problem) ImGui::TextDisabled("%s", problem);
    else if (range.IsEmpty()) ImGui::TextDisabled("Nothing to fill here.");
    else ImGui::TextDisabled("%s - %s", FormatBoundary(range.from).c_str(), FormatBoundary(range.to).c_str());
    if (!FillMessage.empty()) ImGui::TextDisabled("%s", FillMessage.c_str());

    if (ImGui::TreeNode("Custom patterns")) {
        drawPatternEditor();
        ImGui::TreePop();
    }

    if (patternChanged) refreshLastFill();
}

void TempoOverlay::drawPatternPreview(const StrokePattern& pattern) noexcept
{
    // Two repeats of the pattern as a small line graph, with a tick on every beat.
    constexpr int Repeats = 2;
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = ImGui::GetFrameHeight() * 2.f;
    const float padding = 3.f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(width, height));

    auto drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(origin, origin + ImVec2(width, height), ImGui::GetColorU32(ImGuiCol_FrameBg));
    const int stepCount = (int)pattern.steps.size();
    if (stepCount == 0 || pattern.stepsPerBeat < 1) return;

    const int totalSteps = stepCount * Repeats;
    const float stepWidth = width / totalSteps;
    for (int step = 0; step < totalSteps; step += pattern.stepsPerBeat) {
        const float x = origin.x + step * stepWidth;
        drawList->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + height), PatternBeatTickColor);
    }

    ImVec2 previous;
    bool hasPrevious = false;
    for (int step = 0; step < totalSteps; step += 1) {
        const int32_t value = pattern.steps[step % stepCount];
        if (value == StrokePattern::NoAction) continue;
        const ImVec2 point(origin.x + step * stepWidth,
            origin.y + padding + (height - 2.f * padding) * (1.f - value / 100.f));
        if (hasPrevious) drawList->AddLine(previous, point, PatternLineColor, 2.f);
        drawList->AddCircleFilled(point, 2.5f, PatternLineColor);
        previous = point;
        hasPrevious = true;
    }
}

void TempoOverlay::drawPatternEditor() noexcept
{
    auto& fill = PatternFillState::State(patternStateHandle);
    const int32_t builtInCount = (int32_t)PatternFill::BuiltInPatterns().size();

    if (ImGui::Button("New from selected")) {
        StrokePattern copy = selectedPattern();
        copy.name = "Custom " + std::to_string(fill.customPatterns.size() + 1);
        fill.customPatterns.push_back(std::move(copy));
        fill.patternIndex = builtInCount + (int32_t)fill.customPatterns.size() - 1;
    }
    OFS::Tooltip("Copies the selected pattern into a new custom pattern you can edit.");

    if (fill.patternIndex < builtInCount) {
        ImGui::TextDisabled("Built-in patterns can't be edited; copy one to start.");
        return;
    }

    ImGui::SameLine();
    if (ImGui::Button("Delete")) {
        fill.customPatterns.erase(fill.customPatterns.begin() + (fill.patternIndex - builtInCount));
        fill.patternIndex -= 1;
        return;
    }

    auto& pattern = fill.customPatterns[fill.patternIndex - builtInCount];
    bool changed = false;
    ImGui::InputText("Name", &pattern.name);

    int32_t stepsPerBeat = pattern.stepsPerBeat;
    if (ImGui::SliderInt("Steps per beat", &stepsPerBeat, 1, StrokePattern::MaxStepsPerBeat)) {
        ReshapePattern(pattern, pattern.LengthInBeats(), stepsPerBeat);
        changed = true;
    }
    OFS::Tooltip("2 = eighths, 3 = triplets, 4 = sixteenths.");
    int32_t beats = pattern.LengthInBeats();
    if (ImGui::SliderInt("Length (beats)", &beats, 1, StrokePattern::MaxBeats)) {
        ReshapePattern(pattern, beats, pattern.stepsPerBeat);
        changed = true;
    }

    // One vertical slider per step, with a wider gap between beats.
    const float frameHeight = ImGui::GetFrameHeight();
    const ImVec2 sliderSize(frameHeight * StepSliderWidthShare, frameHeight * StepSliderHeightShare);
    const float beatGap = ImGui::GetStyle().ItemSpacing.x * 3.f;
    ImGui::BeginChild("##PatternSteps", ImVec2(0.f, sliderSize.y + frameHeight * 2.f), false, ImGuiWindowFlags_HorizontalScrollbar);
    for (int32_t step = 0; step < (int32_t)pattern.steps.size(); step += 1) {
        if (step > 0) ImGui::SameLine(0.f, step % pattern.stepsPerBeat == 0 ? beatGap : -1.f);
        ImGui::BeginGroup();
        ImGui::PushID(step);
        int32_t& value = pattern.steps[step];
        if (value == StrokePattern::NoAction) {
            if (ImGui::Button("##Empty", sliderSize)) {
                value = 50;
                changed = true;
            }
            OFS::Tooltip("No action on this step. Click to add one.");
        }
        else {
            if (ImGui::VSliderInt("##Value", sliderSize, &value, 0, 100)) changed = true;
            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                value = StrokePattern::NoAction;
                changed = true;
            }
        }
        if (step % pattern.stepsPerBeat == 0) ImGui::Text("%d", step / pattern.stepsPerBeat + 1);
        else ImGui::TextUnformatted(" ");
        ImGui::PopID();
        ImGui::EndGroup();
    }
    ImGui::EndChild();
    ImGui::TextDisabled("Drag a step to set it. Right-click to empty it; click an empty step to add one.");

    if (changed) refreshLastFill();
}

void TempoOverlay::drawSnapToGrid() noexcept
{
    auto app = OpenFunscripter::ptr;
    auto& fill = PatternFillState::State(patternStateHandle);
    const uint32_t selected = app->ActiveFunscript()->SelectionSize();

    ImGui::TextWrapped("Moves the selected actions toward the nearest line of the Snap grid above.");
    const bool strengthChanged = ImGui::SliderInt("Strength", &fill.snapStrength, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp);
    OFS::Tooltip("100% puts actions exactly on the grid; lower values only tighten the timing.\nChanging it right after a snap updates that snap.");

    ImGui::BeginDisabled(selected == 0);
    if (ImGui::Button("Snap selection")) applySnap();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (selected == 0) ImGui::TextDisabled("Select actions to snap.");
    else ImGui::TextDisabled("%u selected", selected);

    if (strengthChanged) refreshLastSnap();
}

void TempoOverlay::DrawScriptPositionContent(const OverlayDrawingCtx& ctx) noexcept
{
    BaseOverlay::DrawHeightLines(ctx);
    const auto analysis = ShowOnsetLane ? CurrentAnalysis() : nullptr;
    if (analysis) {
        drawOnsetLane(ctx, *analysis);
    }
    else {
        timeline->DrawAudioWaveform(ctx);
    }
    BaseOverlay::DrawSecondsLabel(ctx);
    BaseOverlay::DrawScriptLabel(ctx);
    drawGrid(ctx);
    BaseOverlay::DrawActionLines(ctx);
    BaseOverlay::DrawActionPoints(ctx);
}

void TempoOverlay::drawOnsetLane(const OverlayDrawingCtx& ctx, const OFS_BeatAnalysisJob::Result& analysis) noexcept
{
    const auto& envelope = analysis.onsetEnvelope;
    const int columns = (int)(ctx.canvasSize.x / OnsetColumnWidthPx);
    if (envelope.empty() || columns <= 0 || ctx.visibleTime <= 0.f) return;

    const int64_t frameCount = (int64_t)envelope.size();
    const double secondsPerColumn = ctx.visibleTime / columns;
    const float bottom = ctx.canvasPos.y + ctx.canvasSize.y;
    const float maxHeight = ctx.canvasSize.y * OnsetLaneHeightShare;

    // One bar per column showing the strongest onset inside it, so short hits
    // survive when zoomed out.
    for (int column = 0; column < columns; column += 1) {
        const double from = ctx.offsetTime + column * secondsPerColumn - analysis.envelopeStartSeconds;
        const int64_t firstFrame = std::max<int64_t>(0, (int64_t)std::floor(from * analysis.frameRate));
        const int64_t endFrame = std::min(frameCount,
            std::max(firstFrame + 1, (int64_t)std::floor((from + secondsPerColumn) * analysis.frameRate)));

        float peak = 0.f;
        for (int64_t frame = firstFrame; frame < endFrame; frame += 1) peak = std::max(peak, envelope[frame]);
        if (peak < OnsetMinVisible) continue;

        const float x = ctx.canvasPos.x + column * OnsetColumnWidthPx;
        ctx.drawList->AddRectFilled(ImVec2(x, bottom - peak * maxHeight), ImVec2(x + OnsetColumnWidthPx, bottom), OnsetLaneColor);
    }
}

void TempoOverlay::drawGrid(const OverlayDrawingCtx& ctx) noexcept
{
    const auto& map = sections();
    if (ctx.visibleTime <= 0.f) return;

    const double viewStart = ctx.offsetTime;
    const double viewEnd = ctx.offsetTime + ctx.visibleTime;
    const double pixelsPerSecond = ctx.canvasSize.x / ctx.visibleTime;
    const float top = ctx.canvasPos.y;
    const float bottom = ctx.canvasPos.y + ctx.canvasSize.y;
    auto toX = [&](double time) { return ctx.canvasPos.x + (float)((time - viewStart) * pixelsPerSecond); };

    // Thin a crowded grid (subdivisions first, then beats), judged by the
    // fastest tempo on screen.
    const int firstVisible = TempoMap::SectionIndexAt(map, viewStart);
    double shortestBeat = std::numeric_limits<double>::infinity();
    for (int i = firstVisible; i < (int)map.size() && map[i].startSeconds < viewEnd; i += 1) {
        shortestBeat = std::min(shortestBeat, TempoMap::BeatPeriod(map[i]));
    }
    auto lineSpacingPx = [&](double beats) { return beats * shortestBeat * pixelsPerSecond; };
    double lineBeats = beatsPerLine();
    if (lineBeats < 1.0 && lineSpacingPx(lineBeats) < MinGridLineSpacingPx) lineBeats = 1.0;
    if (lineBeats < TempoMap::BeatsPerMeasure && lineSpacingPx(lineBeats) < MinGridLineSpacingPx) lineBeats = TempoMap::BeatsPerMeasure;

    if (lineSpacingPx(lineBeats) >= MinGridLineSpacingPx) {
        auto& tempo = TempoOverlayState::State(stateHandle);
        const uint32_t measureColor = beatMultipleColor[tempo.measureIndex];
        const float labelSize = ImGui::GetFontSize() * 2.f;
        float labelsBlockedUntil = -FLT_MAX; // skips measure numbers that would overlap
        char label[16];

        for (const auto& line : TempoMap::GridLinesBetween(map, viewStart, viewEnd, lineBeats)) {
            const float x = toX(line.time);
            if (line.isMeasureStart) {
                ctx.drawList->AddLine(ImVec2(x, top), ImVec2(x, bottom), measureColor, 5.f);
            }
            else if (line.isBeat) {
                ctx.drawList->AddLine(ImVec2(x, top), ImVec2(x, bottom), BeatLineColor, 3.f);
            }
            else {
                ctx.drawList->AddLine(ImVec2(x, top), ImVec2(x, bottom), SubdivisionLineColor, 1.f);
            }

            if (line.isMeasureStart && line.measureNumber > 0) {
                const float labelX = x + ImGui::GetFontSize() / 2.f;
                if (labelX < labelsBlockedUntil) continue;
                stbsp_snprintf(label, sizeof(label), "%d", line.measureNumber);
                ctx.drawList->AddText(OFS_DynFontAtlas::DefaultFont2, labelSize, ImVec2(labelX, top),
                    ImGui::GetColorU32(ImGuiCol_Text), label);
                labelsBlockedUntil = labelX + ImGui::GetFontSize()
                    + OFS_DynFontAtlas::DefaultFont2->CalcTextSizeA(labelSize, FLT_MAX, 0.f, label).x;
            }
        }
    }

    // Section boundaries, labelled with the tempo that starts there.
    char bpmLabel[32];
    for (int i = std::max(1, firstVisible); i < (int)map.size() && map[i].startSeconds < viewEnd; i += 1) {
        if (map[i].startSeconds < viewStart) continue;
        const float x = toX(map[i].startSeconds);
        ctx.drawList->AddLine(ImVec2(x, top), ImVec2(x, bottom), SectionStartColor, 2.f);
        stbsp_snprintf(bpmLabel, sizeof(bpmLabel), "%.2f BPM", map[i].bpm);
        ctx.drawList->AddText(ImVec2(x + 4.f, bottom - ImGui::GetFontSize() - 2.f), SectionStartColor, bpmLabel);
    }
}

void TempoOverlay::nextFrame(float realFrameTime) noexcept
{
    auto app = OpenFunscripter::ptr;
    const double next = TempoMap::NextGridTime(sections(), app->player->CurrentTime(), beatsPerLine());
    app->player->SetPositionExact((float)next);
}

void TempoOverlay::previousFrame(float realFrameTime) noexcept
{
    auto app = OpenFunscripter::ptr;
    const double previous = TempoMap::PreviousGridTime(sections(), app->player->CurrentTime(), beatsPerLine());
    app->player->SetPositionExact((float)previous);
}

float TempoOverlay::steppingIntervalForward(float realFrameTime, float fromTime) noexcept
{
    return (float)(TempoMap::NextGridTime(sections(), fromTime, beatsPerLine()) - fromTime);
}

float TempoOverlay::steppingIntervalBackward(float realFrameTime, float fromTime) noexcept
{
    return (float)(TempoMap::PreviousGridTime(sections(), fromTime, beatsPerLine()) - fromTime);
}
