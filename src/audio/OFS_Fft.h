#pragma once

#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

// Minimal iterative radix-2 FFT for one fixed power-of-two size.
// Real and imaginary parts are kept in separate arrays; the multiply is written
// out by hand so it stays fast in debug builds (std::complex would not).
class OFS_Fft
{
public:
    explicit OFS_Fft(uint32_t size) noexcept
        : size(size)
    {
        uint32_t bits = 0;
        while ((1u << bits) < size) bits += 1;

        bitReversed.resize(size);
        for (uint32_t i = 0; i < size; i += 1) {
            uint32_t reversed = 0;
            for (uint32_t b = 0; b < bits; b += 1) {
                if (i & (1u << b)) reversed |= 1u << (bits - 1 - b);
            }
            bitReversed[i] = reversed;
        }

        twiddleRe.resize(size / 2);
        twiddleIm.resize(size / 2);
        for (uint32_t k = 0; k < size / 2; k += 1) {
            const double angle = -2.0 * 3.14159265358979323846 * k / size;
            twiddleRe[k] = (float)std::cos(angle);
            twiddleIm[k] = (float)std::sin(angle);
        }
    }

    uint32_t Size() const noexcept { return size; }

    // In-place forward transform. Both arrays must hold Size() values.
    void Forward(float* re, float* im) const noexcept
    {
        for (uint32_t i = 0; i < size; i += 1) {
            const uint32_t j = bitReversed[i];
            if (i < j) {
                std::swap(re[i], re[j]);
                std::swap(im[i], im[j]);
            }
        }

        for (uint32_t length = 2; length <= size; length <<= 1) {
            const uint32_t half = length / 2;
            const uint32_t twiddleStep = size / length;
            for (uint32_t start = 0; start < size; start += length) {
                for (uint32_t k = 0; k < half; k += 1) {
                    const float wRe = twiddleRe[k * twiddleStep];
                    const float wIm = twiddleIm[k * twiddleStep];
                    const uint32_t a = start + k;
                    const uint32_t b = a + half;
                    const float vRe = re[b] * wRe - im[b] * wIm;
                    const float vIm = re[b] * wIm + im[b] * wRe;
                    re[b] = re[a] - vRe;
                    im[b] = im[a] - vIm;
                    re[a] += vRe;
                    im[a] += vIm;
                }
            }
        }
    }

private:
    uint32_t size;
    std::vector<uint32_t> bitReversed;
    std::vector<float> twiddleRe;
    std::vector<float> twiddleIm;
};
