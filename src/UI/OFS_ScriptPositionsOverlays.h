#pragma once
#include "ScriptPositionsOverlayMode.h"
#include "OFS_Localization.h"
#include "audio/OFS_BeatAnalysisJob.h"
#include "audio/OFS_PatternFill.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>


// ATTENTION: no reordering
enum ScriptingOverlayModes : int32_t {
	FRAME,
	TEMPO,
	EMPTY,
};


class ScriptTimeline;

class TempoOverlay : public BaseOverlay {
private:
	static constexpr std::array<float, 10> beatMultiples{
		4.f * 1.f,
		4.f * (1.f / 2.f),
		4.f * (1.f / 4.f),
		4.f * (1.f / 8.f),
		4.f * (1.f / 12.f),
		4.f * (1.f / 16.f),
		4.f * (1.f / 24.f),
		4.f * (1.f / 32.f),
		4.f * (1.f / 48.f),
		4.f * (1.f / 64.f),
	};
	static constexpr std::array<uint32_t, 10> beatMultipleColor{
		IM_COL32(0xbb, 0xbe, 0xbc, 0xFF), // 1st ???

		IM_COL32(0x53, 0xd3, 0xdf, 0xFF), // 2nds
		IM_COL32(0xc1, 0x65, 0x77, 0xFF), // 4ths
		IM_COL32(0x24, 0x54, 0x99, 0xFF), // 8ths
		IM_COL32(0xc8, 0x86, 0xee, 0xFF), // 12ths
		IM_COL32(0xd2, 0xcc, 0x23, 0xFF), // 16ths
		IM_COL32(0xea, 0x8d, 0xe0, 0xFF), // 24ths
		IM_COL32(0xe7, 0x97, 0x5c, 0xFF), // 32nds
		IM_COL32(0xeb, 0x38, 0x99, 0xFF), // 48ths
		IM_COL32(0x23, 0xd2, 0x54, 0xFF), // 64ths
	};
	static constexpr std::array<Tr, 10> beatMultiplesStrings{
		Tr::TEMPO_WHOLE_MEASURES,
		Tr::TEMPO_2ND_MEASURES,
		Tr::TEMPO_4TH_MEASURES,
		Tr::TEMPO_8TH_MEASURES,
		Tr::TEMPO_12TH_MEASURES,
		Tr::TEMPO_16TH_MEASURES,
		Tr::TEMPO_24TH_MEASURES,
		Tr::TEMPO_32ND_MEASURES,
		Tr::TEMPO_48TH_MEASURES,
		Tr::TEMPO_64TH_MEASURES,
	};
	uint32_t stateHandle = 0xFFFF'FFFF;
	uint32_t patternStateHandle = 0xFFFF'FFFF;

	// The most recent fill, so changing the pattern or depth right after it
	// redoes that fill in place instead of needing another click.
	struct LastFillInfo
	{
		std::weak_ptr<Funscript> script;
		PatternFill::TimeRange range;
	};

	// Static so they survive the overlay being recreated on every mode switch.
	static bool ShowOnsetLane;
	static bool ApplyDetectionWhenReady; // set by "Detect", applied in update()
	static std::vector<TempoSection> SectionsBeforeDetection;
	static std::string SectionsBeforeDetectionMedia; // undo only applies to this media
	static std::string DetectionMessage;
	static LastFillInfo LastFill;
	static std::weak_ptr<Funscript> LastSnapScript;
	static std::string FillMessage;

	// The project's tempo map; older single-tempo projects are migrated on first use.
	std::vector<TempoSection>& sections() noexcept;
	// Spacing of the selected snap, in beats (4 = whole measures, 0.25 = sixteenths).
	double beatsPerLine() noexcept;
	void applyDetectedTempoMap(const std::vector<TempoSection>& detected) noexcept;

	// Built-in patterns come first in the picker, then the user's custom ones.
	const StrokePattern& selectedPattern() noexcept;
	// The range "Fill" would write to right now; empty (with a reason) if there is none.
	PatternFill::TimeRange currentFillRange(const char** problem) noexcept;
	void applyPatternFill(const PatternFill::TimeRange& range) noexcept;
	void refreshLastFill() noexcept;
	void applySnap() noexcept;
	void refreshLastSnap() noexcept;

	void drawDetectionControls() noexcept;
	void drawSectionEditor() noexcept;
	void drawSectionList() noexcept;
	void drawPatternFill() noexcept;
	void drawPatternPreview(const StrokePattern& pattern) noexcept;
	void drawPatternEditor() noexcept;
	void drawSnapToGrid() noexcept;
	void drawOnsetLane(const OverlayDrawingCtx& ctx, const OFS_BeatAnalysisJob::Result& analysis) noexcept;
	void drawGrid(const OverlayDrawingCtx& ctx) noexcept;
public:
	TempoOverlay(ScriptTimeline* timeline) noexcept;
	virtual void update() noexcept override;
	virtual void DrawSettings() noexcept override;
	virtual void DrawScriptPositionContent(const OverlayDrawingCtx& ctx) noexcept override;
	virtual void nextFrame(float realFrameTime) noexcept override;
	virtual void previousFrame(float realFrameTime) noexcept override;

	virtual float steppingIntervalForward(float realFrameTime, float fromTime) noexcept override;
	virtual float steppingIntervalBackward(float realFrameTime, float fromTime) noexcept override;
};


class FrameOverlay : public BaseOverlay {
private:
	float fpsOverride = 0.f;
	bool enableFpsOverride = false;
public:
	FrameOverlay(ScriptTimeline* timeline)
		: BaseOverlay(timeline) {}
	virtual void DrawScriptPositionContent(const OverlayDrawingCtx& ctx) noexcept override;
	virtual void DrawSettings() noexcept override;
	virtual void nextFrame(float realFrameTime) noexcept override;
	virtual void previousFrame(float realFrameTime) noexcept override;

	virtual float logicalFrameTime(float realFrameTime) noexcept override;
	virtual float steppingIntervalForward(float realFrameTime, float fromTime) noexcept override;
	virtual float steppingIntervalBackward(float realFrameTime, float fromTime) noexcept override;
};