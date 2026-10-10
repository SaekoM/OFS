#include "OFS_BeatAnalysis.h"
#include "OFS_TempoMap.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>

namespace
{
    // --- onset detection ---
    constexpr int BandCount = 40;
    constexpr double LowestBandHz = 40.0;
    // Band magnitudes are compressed with log1p(compression * magnitude) so a
    // quiet hi-hat still registers next to a loud kick.
    constexpr float MagnitudeCompression = 10.f;
    // Consumed samples are dropped from the pending buffer once this many pile up.
    constexpr size_t CompactThreshold = 1 << 16;

    // --- onset strength ---
    constexpr double LoudnessWindowSeconds = 0.5;  // each side of the frame

    // --- display ---
    constexpr double DisplayPercentile = 0.98;     // this strength reaches the top of the lane

    // --- local tempo ---
    constexpr double TempoWindowSeconds = 8.0;
    constexpr double TempoHopSeconds = 1.0;
    constexpr float MinTempoBpm = 50.f;
    constexpr float MaxTempoBpm = 220.f;
    constexpr float TempoStepBpm = 0.5f;
    // Prior favouring tempos near 120 BPM, one octave standard deviation. This is
    // what decides between a tempo and its half or double.
    constexpr double PriorCenterBpm = 120.0;
    constexpr double PriorOctaves = 1.0;
    // Viterbi costs: drifting up to TempoDriftSteps bins is cheap, a jump to
    // any other tempo must be supported for a few windows before it wins.
    constexpr int TempoDriftSteps = 2;
    constexpr float TempoDriftCost = 0.3f;
    constexpr float TempoJumpCost = 4.f;

    // --- beat tracking ---
    constexpr double BeatTightness = 100.0;        // how strongly spacing sticks to the tempo
    constexpr double FirstBeatThreshold = 0.01;    // fraction of the strongest onset

    // --- tempo map fitting ---
    constexpr double FitToleranceSeconds = 0.030;  // max distance of a beat from its section's grid
    constexpr int SeedBeats = 8;
    constexpr int MaxConsecutiveMisses = 2;
    constexpr int RefitEveryBeats = 8;
    // Shorter runs are usually the tracker settling after a song change.
    constexpr int MinSectionBeats = 16;

    double Square(double value) noexcept { return value * value; }

    float Median(std::vector<float> values) noexcept
    {
        if (values.empty()) return 0.f;
        auto middle = values.begin() + values.size() / 2;
        std::nth_element(values.begin(), middle, values.end());
        return *middle;
    }

    // Beats assigned to one tempo section: time ~= anchor + period * number.
    struct BeatFit
    {
        std::vector<double> times;
        std::vector<int64_t> numbers;
        double anchor = 0.0;
        double period = 0.0;

        void Add(double time, int64_t number) noexcept
        {
            times.push_back(time);
            numbers.push_back(number);
        }

        double Predict(int64_t number) const noexcept { return anchor + period * number; }

        void Solve() noexcept
        {
            const double count = (double)times.size();
            const double meanNumber = std::accumulate(numbers.begin(), numbers.end(), 0.0) / count;
            const double meanTime = std::accumulate(times.begin(), times.end(), 0.0) / count;
            double covariance = 0.0;
            double variance = 0.0;
            for (size_t i = 0; i < times.size(); i += 1) {
                covariance += (numbers[i] - meanNumber) * (times[i] - meanTime);
                variance += Square(numbers[i] - meanNumber);
            }
            period = variance > 0.0 ? covariance / variance : 0.0;
            anchor = meanTime - period * meanNumber;
        }

        double MaxResidual() const noexcept
        {
            double worst = 0.0;
            for (size_t i = 0; i < times.size(); i += 1) {
                worst = std::max(worst, std::abs(times[i] - Predict(numbers[i])));
            }
            return worst;
        }
    };

    bool FitsGrid(const BeatFit& fit, double time) noexcept
    {
        const int64_t number = std::llround((time - fit.anchor) / fit.period);
        return std::abs(time - fit.Predict(number)) <= FitToleranceSeconds;
    }

    // Merges `next` into `current` when all their beats fit one grid together,
    // e.g. one song that a few bad beats split in two. A real song change has a
    // tempo or phase jump, so the combined fit fails and both are kept.
    bool TryJoin(BeatFit& current, const BeatFit& next) noexcept
    {
        BeatFit joined = current;
        // Continue the numbering across the gap, counting the beats in between at next's tempo.
        const int64_t gapBeats = std::max<int64_t>(1, std::llround((next.times.front() - current.times.back()) / next.period));
        const int64_t firstNumber = current.numbers.back() + gapBeats;
        for (size_t i = 0; i < next.times.size(); i += 1) {
            joined.Add(next.times[i], firstNumber + (next.numbers[i] - next.numbers.front()));
        }
        joined.Solve();
        if (joined.MaxResidual() > FitToleranceSeconds) return false;
        current = std::move(joined);
        return true;
    }
}

// ---------------------------------------------------------------------------
// OnsetDetector

BeatAnalysis::OnsetDetector::OnsetDetector() noexcept
    : fft(FftSize)
{
    window.resize(FftSize);
    for (int i = 0; i < FftSize; i += 1) {
        window[i] = 0.5f - 0.5f * (float)std::cos(2.0 * 3.14159265358979323846 * i / FftSize);
    }

    // Log-spaced bands from LowestBandHz up to Nyquist, at least one bin each.
    const double binHz = (double)SampleRate / FftSize;
    const int lastBin = FftSize / 2;
    const double nyquist = SampleRate / 2.0;
    int previousEdge = std::max(1, (int)std::lround(LowestBandHz / binHz));
    for (int band = 1; band <= BandCount && previousEdge < lastBin; band += 1) {
        const double edgeHz = LowestBandHz * std::pow(nyquist / LowestBandHz, (double)band / BandCount);
        const int edge = std::min(lastBin, std::max(previousEdge + 1, (int)std::lround(edgeHz / binHz)));
        bandFirstBin.push_back(previousEdge);
        bandEndBin.push_back(edge);
        previousEdge = edge;
    }

    re.resize(FftSize);
    im.resize(FftSize);
    bands.resize(bandFirstBin.size());
    previousBands.resize(bandFirstBin.size());

    // Half a window of silence in front centres frame 0 on time 0.
    pending.assign(FftSize / 2, 0.f);
}

void BeatAnalysis::OnsetDetector::AddSamples(const float* samples, size_t count) noexcept
{
    pending.insert(pending.end(), samples, samples + count);
    processAvailableFrames();
}

void BeatAnalysis::OnsetDetector::Finish() noexcept
{
    pending.insert(pending.end(), FftSize / 2, 0.f);
    processAvailableFrames();
}

void BeatAnalysis::OnsetDetector::processAvailableFrames() noexcept
{
    while (pending.size() - readPosition >= (size_t)FftSize) {
        processFrame(pending.data() + readPosition);
        readPosition += HopSize;
    }
    if (readPosition >= CompactThreshold) {
        pending.erase(pending.begin(), pending.begin() + readPosition);
        readPosition = 0;
    }
}

void BeatAnalysis::OnsetDetector::processFrame(const float* frameStart) noexcept
{
    for (int i = 0; i < FftSize; i += 1) {
        re[i] = frameStart[i] * window[i];
        im[i] = 0.f;
    }
    fft.Forward(re.data(), im.data());

    for (size_t band = 0; band < bands.size(); band += 1) {
        float sum = 0.f;
        for (int bin = bandFirstBin[band]; bin < bandEndBin[band]; bin += 1) {
            sum += std::sqrt(re[bin] * re[bin] + im[bin] * im[bin]);
        }
        const float mean = sum / (float)(bandEndBin[band] - bandFirstBin[band]);
        bands[band] = std::log1p(MagnitudeCompression * mean);
    }

    // Spectral flux: how much each band got louder since the previous frame.
    // Comparing against the loudest neighbouring band of the previous frame
    // ignores pitch wobble (vibrato) that would otherwise look like new notes.
    float value = 0.f;
    if (hasPreviousFrame) {
        const size_t lastBand = bands.size() - 1;
        for (size_t band = 0; band < bands.size(); band += 1) {
            float previous = previousBands[band];
            if (band > 0) previous = std::max(previous, previousBands[band - 1]);
            if (band < lastBand) previous = std::max(previous, previousBands[band + 1]);
            value += std::max(0.f, bands[band] - previous);
        }
    }
    flux.push_back(value);
    previousBands = bands;
    hasPreviousFrame = true;
}

// ---------------------------------------------------------------------------
// Envelope post-processing

std::vector<float> BeatAnalysis::OnsetStrength(const std::vector<float>& flux) noexcept
{
    const size_t count = flux.size();
    std::vector<float> strength(count, 0.f);
    if (count == 0) return strength;

    std::vector<double> prefix(count + 1, 0.0);
    for (size_t i = 0; i < count; i += 1) prefix[i + 1] = prefix[i] + flux[i];

    const size_t radius = (size_t)std::lround(LoudnessWindowSeconds * FrameRate);
    double sumSquares = 0.0;
    for (size_t i = 0; i < count; i += 1) {
        const size_t from = i > radius ? i - radius : 0;
        const size_t to = std::min(count, i + radius + 1);
        const double localMean = (prefix[to] - prefix[from]) / (double)(to - from);
        strength[i] = (float)std::max(0.0, flux[i] - localMean);
        sumSquares += Square(strength[i]);
    }

    const double deviation = std::sqrt(sumSquares / count);
    if (deviation > 1e-9) {
        for (auto& value : strength) value = (float)(value / deviation);
    }
    return strength;
}

std::vector<float> BeatAnalysis::DisplayEnvelope(const std::vector<float>& strength) noexcept
{
    std::vector<float> positive;
    positive.reserve(strength.size());
    for (float value : strength) if (value > 0.f) positive.push_back(value);

    std::vector<float> display(strength.size(), 0.f);
    if (positive.empty()) return display;

    auto top = positive.begin() + (size_t)(DisplayPercentile * (positive.size() - 1));
    std::nth_element(positive.begin(), top, positive.end());
    const float scale = *top;
    if (scale <= 0.f) return display;

    for (size_t i = 0; i < strength.size(); i += 1) {
        display[i] = std::min(1.f, strength[i] / scale);
    }
    return display;
}

// ---------------------------------------------------------------------------
// Local tempo

std::vector<float> BeatAnalysis::LocalTempo(const std::vector<float>& strength) noexcept
{
    const int frameCount = (int)strength.size();
    std::vector<float> bpmPerFrame(frameCount, (float)PriorCenterBpm);
    if (frameCount == 0) return bpmPerFrame;

    const int stateCount = (int)std::lround((MaxTempoBpm - MinTempoBpm) / TempoStepBpm) + 1;
    const int windowFrames = (int)std::lround(TempoWindowSeconds * FrameRate);
    const int hopFrames = (int)std::lround(TempoHopSeconds * FrameRate);
    const int windowCount = std::max(1, (frameCount + hopFrames - 1) / hopFrames);
    const int maxLag = (int)std::ceil(FrameRate * 60.0 / MinTempoBpm) + 2;

    std::vector<double> stateLag(stateCount);
    std::vector<double> statePrior(stateCount);
    for (int state = 0; state < stateCount; state += 1) {
        const double bpm = MinTempoBpm + state * TempoStepBpm;
        stateLag[state] = FrameRate * 60.0 / bpm;
        statePrior[state] = std::exp(-0.5 * Square(std::log2(bpm / PriorCenterBpm) / PriorOctaves));
    }

    // Emission cost per window and tempo: low where the autocorrelation shows
    // a strong repeat at that tempo's beat period.
    std::vector<float> cost((size_t)windowCount * stateCount, 0.f);
    std::vector<double> segment;
    std::vector<double> autocorrelation(maxLag + 2);
    std::vector<double> salience(stateCount);
    for (int w = 0; w < windowCount; w += 1) {
        float* windowCost = cost.data() + (size_t)w * stateCount;
        const int center = w * hopFrames;
        const int from = std::max(0, center - windowFrames / 2);
        const int to = std::min(frameCount, center + windowFrames / 2);
        const int length = to - from;
        if (length < 2 * (maxLag + 2)) continue; // too short to judge; leave cost flat

        segment.assign(strength.begin() + from, strength.begin() + to);
        const double mean = std::accumulate(segment.begin(), segment.end(), 0.0) / length;
        for (auto& value : segment) value -= mean;

        for (int lag = 0; lag <= maxLag + 1; lag += 1) {
            double sum = 0.0;
            for (int i = 0; i + lag < length; i += 1) sum += segment[i] * segment[i + lag];
            autocorrelation[lag] = sum / (length - lag);
        }
        if (autocorrelation[0] <= 1e-12) continue; // silence

        double maxSalience = 0.0;
        for (int state = 0; state < stateCount; state += 1) {
            const double lag = stateLag[state];
            const int lower = (int)lag;
            const double fraction = lag - lower;
            const double value = autocorrelation[lower] * (1.0 - fraction) + autocorrelation[lower + 1] * fraction;
            salience[state] = std::max(0.0, value / autocorrelation[0]) * statePrior[state];
            maxSalience = std::max(maxSalience, salience[state]);
        }
        if (maxSalience <= 0.0) continue;
        for (int state = 0; state < stateCount; state += 1) {
            windowCost[state] = (float)-std::log(salience[state] / maxSalience + 1e-3);
        }
    }

    // Viterbi: the cheapest tempo path through all windows.
    std::vector<int16_t> cameFrom((size_t)windowCount * stateCount, 0);
    std::vector<float> pathCost(cost.begin(), cost.begin() + stateCount);
    std::vector<float> nextCost(stateCount);
    for (int w = 1; w < windowCount; w += 1) {
        const int cheapestState = (int)(std::min_element(pathCost.begin(), pathCost.end()) - pathCost.begin());
        const float* windowCost = cost.data() + (size_t)w * stateCount;
        int16_t* windowCameFrom = cameFrom.data() + (size_t)w * stateCount;

        for (int state = 0; state < stateCount; state += 1) {
            float best = pathCost[state];
            int bestFrom = state;
            for (int step = -TempoDriftSteps; step <= TempoDriftSteps; step += 1) {
                const int from = state + step;
                if (step == 0 || from < 0 || from >= stateCount) continue;
                if (pathCost[from] + TempoDriftCost < best) {
                    best = pathCost[from] + TempoDriftCost;
                    bestFrom = from;
                }
            }
            if (pathCost[cheapestState] + TempoJumpCost < best) {
                best = pathCost[cheapestState] + TempoJumpCost;
                bestFrom = cheapestState;
            }
            nextCost[state] = windowCost[state] + best;
            windowCameFrom[state] = (int16_t)bestFrom;
        }
        pathCost.swap(nextCost);
    }

    std::vector<float> bpmPerWindow(windowCount);
    int state = (int)(std::min_element(pathCost.begin(), pathCost.end()) - pathCost.begin());
    for (int w = windowCount - 1; w >= 0; w -= 1) {
        bpmPerWindow[w] = MinTempoBpm + state * TempoStepBpm;
        state = cameFrom[(size_t)w * stateCount + state];
    }

    for (int frame = 0; frame < frameCount; frame += 1) {
        const int w = std::min(windowCount - 1, (int)std::lround((double)frame / hopFrames));
        bpmPerFrame[frame] = bpmPerWindow[w];
    }
    return bpmPerFrame;
}

// ---------------------------------------------------------------------------
// Beat tracking

std::vector<double> BeatAnalysis::TrackBeats(const std::vector<float>& strength, const std::vector<float>& localBpm) noexcept
{
    std::vector<double> beatTimes;
    const int frameCount = (int)strength.size();
    if (frameCount < 4 || (int)localBpm.size() != frameCount) return beatTimes;

    // Light smoothing so a beat may land a frame either side of an onset peak.
    constexpr float Kernel[] = { 0.054f, 0.242f, 0.399f, 0.242f, 0.054f }; // gaussian, sigma = 1 frame
    constexpr int KernelRadius = 2;
    std::vector<float> localScore(frameCount, 0.f);
    for (int i = 0; i < frameCount; i += 1) {
        float sum = 0.f;
        for (int k = -KernelRadius; k <= KernelRadius; k += 1) {
            const int j = i + k;
            if (j >= 0 && j < frameCount) sum += Kernel[k + KernelRadius] * strength[j];
        }
        localScore[i] = sum;
    }
    const float firstBeatScore = (float)FirstBeatThreshold * *std::max_element(localScore.begin(), localScore.end());

    // cumulativeScore[t]: best total score of a beat sequence ending with a beat at t.
    // The previous beat comes from half to two periods earlier, penalised by how
    // far the gap strays from the local beat period.
    std::vector<float> cumulativeScore(frameCount, 0.f);
    std::vector<int> previousBeat(frameCount, -1);
    std::vector<float> gapCost;
    double cachedPeriod = -1.0;
    int minGap = 1;
    int maxGap = 1;
    bool beforeFirstBeat = true;

    for (int t = 0; t < frameCount; t += 1) {
        const double period = FrameRate * 60.0 / localBpm[t];
        if (period != cachedPeriod) {
            cachedPeriod = period;
            minGap = std::max(1, (int)std::lround(period / 2.0));
            maxGap = std::max(minGap, (int)std::lround(period * 2.0));
            gapCost.resize(maxGap - minGap + 1);
            for (int gap = minGap; gap <= maxGap; gap += 1) {
                gapCost[gap - minGap] = (float)(-BeatTightness * Square(std::log(gap / period)));
            }
        }

        float best = -std::numeric_limits<float>::infinity();
        int bestFrom = -1;
        for (int gap = minGap; gap <= maxGap && t - gap >= 0; gap += 1) {
            const float candidate = cumulativeScore[t - gap] + gapCost[gap - minGap];
            if (candidate > best) {
                best = candidate;
                bestFrom = t - gap;
            }
        }

        cumulativeScore[t] = localScore[t] + (bestFrom >= 0 ? best : 0.f);
        if (beforeFirstBeat && localScore[t] < firstBeatScore) {
            previousBeat[t] = -1;
        } else {
            previousBeat[t] = bestFrom;
            beforeFirstBeat = false;
        }
    }

    // The sequence ends at the last strong local maximum of the cumulative score.
    std::vector<float> peakScores;
    for (int t = 1; t + 1 < frameCount; t += 1) {
        if (cumulativeScore[t] > cumulativeScore[t - 1] && cumulativeScore[t] >= cumulativeScore[t + 1]) {
            peakScores.push_back(cumulativeScore[t]);
        }
    }
    if (peakScores.empty()) return beatTimes;
    const float endThreshold = 0.5f * Median(peakScores);
    int lastBeat = -1;
    for (int t = frameCount - 2; t >= 1; t -= 1) {
        if (cumulativeScore[t] > cumulativeScore[t - 1] && cumulativeScore[t] >= cumulativeScore[t + 1]
            && cumulativeScore[t] > endThreshold) {
            lastBeat = t;
            break;
        }
    }

    std::vector<int> beatFrames;
    for (int t = lastBeat; t >= 0; t = previousBeat[t]) beatFrames.push_back(t);
    std::reverse(beatFrames.begin(), beatFrames.end());
    if (beatFrames.empty()) return beatTimes;

    // Trim weak beats at the start and end (silent intros/outros get filled with
    // evenly spaced filler beats otherwise).
    std::vector<float> beatStrength(beatFrames.size());
    for (size_t i = 0; i < beatFrames.size(); i += 1) {
        float sum = localScore[beatFrames[i]];
        float weight = 1.f;
        if (i > 0) { sum += 0.5f * localScore[beatFrames[i - 1]]; weight += 0.5f; }
        if (i + 1 < beatFrames.size()) { sum += 0.5f * localScore[beatFrames[i + 1]]; weight += 0.5f; }
        beatStrength[i] = sum / weight;
    }
    double meanSquare = 0.0;
    for (float value : beatStrength) meanSquare += Square(value);
    const float trimThreshold = 0.5f * (float)std::sqrt(meanSquare / beatStrength.size());

    size_t first = 0;
    size_t last = beatFrames.size();
    while (first < last && beatStrength[first] <= trimThreshold) first += 1;
    while (last > first && beatStrength[last - 1] <= trimThreshold) last -= 1;

    for (size_t i = first; i < last; i += 1) beatTimes.push_back(FrameToSeconds(beatFrames[i]));
    return beatTimes;
}

// ---------------------------------------------------------------------------
// Tempo map fitting

std::vector<TempoSection> BeatAnalysis::FitTempoMap(const std::vector<double>& beatTimes) noexcept
{
    const size_t beatCount = beatTimes.size();
    std::vector<BeatFit> fits;

    // Grow sections greedily: seed with a few beats that sit on a straight line,
    // then keep adding beats while they stay on that line. A few consecutive
    // misses (song change, tempo change, phase jump) closes the section.
    size_t seedStart = 0;
    while (seedStart + SeedBeats <= beatCount) {
        BeatFit fit;
        for (int k = 0; k < SeedBeats; k += 1) fit.Add(beatTimes[seedStart + k], k);
        fit.Solve();
        if (fit.period <= 0.0 || fit.MaxResidual() > FitToleranceSeconds) {
            seedStart += 1;
            continue;
        }

        size_t lastAccepted = seedStart + SeedBeats - 1;
        int misses = 0;
        int sinceRefit = 0;
        for (size_t j = lastAccepted + 1; j < beatCount; j += 1) {
            const int64_t number = std::llround((beatTimes[j] - fit.anchor) / fit.period);
            const int64_t step = number - fit.numbers.back();
            const bool onGrid = std::abs(beatTimes[j] - fit.Predict(number)) <= FitToleranceSeconds;
            // step 2 tolerates one beat the tracker skipped
            if (onGrid && step >= 1 && step <= 2) {
                fit.Add(beatTimes[j], number);
                lastAccepted = j;
                misses = 0;
                if (++sinceRefit >= RefitEveryBeats) {
                    fit.Solve();
                    sinceRefit = 0;
                }
            } else if (++misses > MaxConsecutiveMisses) {
                break;
            }
        }
        fit.Solve();
        fits.push_back(std::move(fit));
        seedStart = lastAccepted + 1;
    }

    // Drop fragments, then merge neighbours that turned out to share one grid.
    std::vector<BeatFit> merged;
    for (auto& fit : fits) {
        if ((int)fit.times.size() < MinSectionBeats) continue;
        if (!merged.empty() && TryJoin(merged.back(), fit)) continue;
        merged.push_back(std::move(fit));
    }

    std::vector<TempoSection> sections;
    for (size_t k = 0; k < merged.size(); k += 1) {
        const auto& fit = merged[k];
        TempoSection section;
        section.bpm = (float)(60.0 / fit.period);
        section.anchorSeconds = (float)fit.Predict(fit.numbers.front());

        if (k > 0) {
            // Beats between two sections belong to neither yet. Let the later
            // section claim the ones on its grid (walking back from its start),
            // then the earlier one, and put the boundary in the remaining gap.
            const auto& previous = merged[k - 1];
            double previousEnd = previous.times.back();
            double nextStart = fit.times.front();
            const auto gapBegin = std::upper_bound(beatTimes.begin(), beatTimes.end(), previousEnd);
            auto gapEnd = std::lower_bound(beatTimes.begin(), beatTimes.end(), nextStart);
            while (gapEnd != gapBegin && FitsGrid(fit, *(gapEnd - 1))) {
                gapEnd -= 1;
                nextStart = *gapEnd;
            }
            for (auto beat = gapBegin; beat != gapEnd && FitsGrid(previous, *beat); ++beat) {
                previousEnd = *beat;
            }
            section.startSeconds = (float)((previousEnd + nextStart) / 2.0);
        }
        sections.push_back(section);
    }
    TempoMap::Normalize(sections);
    return sections;
}
