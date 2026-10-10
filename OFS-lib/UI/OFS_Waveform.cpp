#include "OFS_Waveform.h"
#include "OFS_Util.h"
#include "OFS_Profiling.h"
#include "OFS_GL.h"
#include "OFS_ScriptTimeline.h"

#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"

#include "subprocess.h"

#include <algorithm>
#include <cmath>

bool OFS_Waveform::LoadFlac(const std::string& output) noexcept
{
	drflac* flac = drflac_open_file(output.c_str(), NULL);
	if (!flac) return false;

	// One value per slice: the loudest sample in it. Peaks keep drum hits sharp,
	// where an average would round them off. ~2.9 ms per slice at 44.1 kHz.
	// Keep in sync with WaveformState::CurrentFormatVersion.
	constexpr int SamplesPerSlice = 128;
	std::vector<drflac_int16> chunk(SamplesPerSlice * 375); // whole slices per read

	Clear();
	samples.reserve(flac->totalPCMFrameCount / SamplesPerSlice + 1);
	float loudestPeak = 0.f;
	uint64_t chunkSize = 0;
	while ((chunkSize = drflac_read_pcm_frames_s16(flac, chunk.size(), chunk.data())) > 0) {
		for (uint64_t sliceStart = 0; sliceStart < chunkSize; sliceStart += SamplesPerSlice) {
			const uint64_t sliceEnd = std::min<uint64_t>(sliceStart + SamplesPerSlice, chunkSize);
			float peak = 0.f;
			for (uint64_t i = sliceStart; i < sliceEnd; i += 1) {
				peak = Util::Max(peak, std::abs((float)chunk[i]) / 32768.f);
			}
			loudestPeak = Util::Max(loudestPeak, peak);
			samples.emplace_back(peak);
		}
	}
	drflac_close(flac);
	samples.shrink_to_fit();

	// Scale so the loudest peak fills the lane (values stay in 0..1).
	if (loudestPeak > 0.f) {
		for (auto& sample : samples) sample /= loudestPeak;
	}
	return true;
}

bool OFS_Waveform::GenerateAndLoadFlac(const std::string& ffmpegPath, const std::string& videoPath, const std::string& output) noexcept
{
	generating = true;

	std::array<const char*, 11> args =
	{
		ffmpegPath.c_str(),
		"-y",
		"-loglevel",
		"quiet",
		"-i", videoPath.c_str(),
		"-vn",
		"-ac", "1",
		output.c_str(),
		nullptr
	};
	struct subprocess_s proc;
	if(subprocess_create(args.data(), subprocess_option_no_window, &proc) != 0) {
		generating = false; 
		return false; 
	}

	if(proc.stdout_file) 
	{
		fclose(proc.stdout_file);
		proc.stdout_file = nullptr;
	}
	
	if(proc.stderr_file) 
	{
		fclose(proc.stderr_file);
		proc.stderr_file = nullptr;
	}

	int return_code;
	subprocess_join(&proc, &return_code);
	subprocess_destroy(&proc);

	if (!LoadFlac(output)) {
		generating = false;
		return false;
	}

	generating = false;
	return true;
}

void OFS_WaveformLOD::Init() noexcept
{
	glGenTextures(1, &WaveformTex);
	glBindTexture(GL_TEXTURE_2D, WaveformTex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); 
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	WaveShader = std::make_unique<WaveformShader>();
}

void OFS_WaveformLOD::Update(const OverlayDrawingCtx& ctx) noexcept
{
	OFS_PROFILE(__FUNCTION__);
	const float relStart = ctx.offsetTime / ctx.totalDuration;
	const float relDuration = ctx.visibleTime / ctx.totalDuration;
	
	const auto& samples = data.Samples();
	const float totalSampleCount = samples.size();

	float startIndexF = relStart * totalSampleCount;
	float endIndexF = (relStart* totalSampleCount) + (totalSampleCount * relDuration);

	float visibleSampleCountF = endIndexF - startIndexF;

	// One value per screen pixel; fewer would get smoothed across and blur the outline.
	const float desiredSamples = ctx.canvasSize.x;
	const float everyNth = SDL_ceilf(visibleSampleCountF / desiredSamples);

	auto& lineBuf = WaveformLineBuffer;		
	if((int32_t)lastMultiple != (int32_t)(startIndexF / everyNth)) {
		int32_t scrollBy = (startIndexF/everyNth) - lastMultiple;

		if(lastVisibleDuration == ctx.visibleTime
		&& lastCanvasX == ctx.canvasSize.x
		&& scrollBy > 0 && scrollBy < lineBuf.size()) {
			OFS_PROFILE("WaveformScrolling");
			std::memcpy(lineBuf.data(), lineBuf.data() + scrollBy, sizeof(float) * (lineBuf.size() - scrollBy));
			lineBuf.resize(lineBuf.size() - scrollBy);
			
			int addedCount = 0;
			float maxSample;
			for(int32_t i = endIndexF - (everyNth*scrollBy); i <= endIndexF; i += everyNth) {
				maxSample = 0.f;
				for(int32_t j=0; j < everyNth; j += 1) {
					int32_t currentIndex = i + j;
					if(currentIndex >= 0 && currentIndex < totalSampleCount) {
						float s = std::abs(samples[currentIndex]);
						maxSample = Util::Max(maxSample, s);
					}
				}
				lineBuf.emplace_back(maxSample);
				addedCount += 1; 
				if(addedCount == scrollBy) break;
			}
			assert(addedCount == scrollBy);
		} else if(scrollBy != 0) {
			OFS_PROFILE("WaveformUpdate");
			lineBuf.clear();
			float maxSample;
			for(int32_t i = startIndexF; i <= endIndexF; i += everyNth) {
				maxSample = 0.f;
				for(int32_t j=0; j < everyNth; j += 1) {
					int32_t currentIndex = i + j;
					if(currentIndex >= 0 && currentIndex < totalSampleCount) {
						float s = std::abs(samples[currentIndex]);
						maxSample = Util::Max(maxSample, s);
					}
				}
				lineBuf.emplace_back(maxSample);
			}
		}

		
		lastMultiple = SDL_floorf(startIndexF / everyNth);
		lastCanvasX = ctx.canvasSize.x;
		lastVisibleDuration = ctx.visibleTime;
		Upload();
	}

	samplingOffset = (1.f / lineBuf.size()) * ((startIndexF/everyNth) - lastMultiple);

#if 0
	ImGui::Begin("Waveform Debug");
	ImGui::Text("Audio samples: %lld", lineBuf.size());
	ImGui::Text("Expected samples: %f", (endIndexF - startIndexF)/everyNth);
	ImGui::Text("Samples in view: %f", (endIndexF - startIndexF));
	ImGui::Text("Start: %f", startIndexF);
	ImGui::Text("End: %f", endIndexF);
	ImGui::Text("Every nth: %f", everyNth);
	ImGui::Text("Last multiple: %f", lastMultiple);
	ImGui::SliderFloat("Offset", &samplingOffset, 0.f, 1.f/lineBuf.size(), "%f");
	ImGui::End();
#endif
}

void OFS_WaveformLOD::Upload() noexcept
{
	OFS_PROFILE(__FUNCTION__);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, WaveformTex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, WaveformLineBuffer.size(), 1, 0, GL_RED, GL_FLOAT, WaveformLineBuffer.data());
}