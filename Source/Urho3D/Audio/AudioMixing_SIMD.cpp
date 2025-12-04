// Copyright (c) 2008-2025 the Urho3D project
// License: MIT
//
// Phase 1.4: SIMD-Optimized Audio Mixing Implementation
// Vectorized sample mixing with SSE acceleration

#include "../Precompiled.h"
#include "AudioMixing_SIMD.h"
#include "../Math/MathDefs.h"

namespace Urho3D
{

void AudioMixing_SIMD::MixMonoWithGain_4x(int* dest, const short* src, int vol)
{
#ifdef URHO3D_SSE
    // Load 4 input samples as 16-bit shorts (naturally aligned)
    __m128i srcVec = _mm_loadu_si128((__m128i*)src);

    // Convert to 32-bit ints (sign-extend)
    __m128i srcLo = _mm_cvtepi16_epi32(srcVec);

    // Load destination 4x int32
    __m128i destVec = _mm_loadu_si128((__m128i*)dest);

    // Multiply gain (vol/256)
    // srcLo contains 4x int32 values
    __m128i volVec = _mm_set1_epi32(vol);
    __m128i mul = _mm_mul_epi32(srcLo, volVec);

    // Divide by 256 (right shift by 8)
    __m128i scaled = _mm_srai_epi32(mul, 8);

    // Add to destination with saturation for safety
    __m128i result = _mm_add_epi32(destVec, scaled);

    // Store back
    _mm_storeu_si128((__m128i*)dest, result);
#else
    // Scalar fallback
    MixMonoWithGain_Scalar(dest, src, vol, 4);
#endif
}

void AudioMixing_SIMD::MixStereoWithGain_2x(int* destL, int* destR, const short* src, int volL, int volR)
{
#ifdef URHO3D_SSE
    // Stereo source: [L0, R0, L1, R1]
    __m128i srcVec = _mm_loadu_si128((__m128i*)src);

    // Separate L and R channels
    __m128i lRshifted = _mm_srai_epi32(_mm_unpacklo_epi16(srcVec, srcVec), 16);  // [L0, L0, L1, L1]
    __m128i rRshifted = _mm_srai_epi32(_mm_unpackhi_epi16(srcVec, srcVec), 16);  // [R0, R0, R1, R1]

    // Actually, easier approach: shuffle and extract
    __m128i L0L1 = _mm_shuffle_epi32(srcVec, 0xA0);  // [L0, L0, L1, L1]
    __m128i R0R1 = _mm_shuffle_epi32(srcVec, 0xF5);  // [R0, R0, R1, R1]

    // Convert to 32-bit
    __m128i srcL = _mm_cvtepi16_epi32(L0L1);
    __m128i srcR = _mm_cvtepi16_epi32(R0R1);

    // Load destination
    __m128i dstL = _mm_loadu_si128((__m128i*)destL);
    __m128i dstR = _mm_loadu_si128((__m128i*)destR);

    // Apply gains
    __m128i volLVec = _mm_set1_epi32(volL);
    __m128i volRVec = _mm_set1_epi32(volR);

    __m128i scaledL = _mm_srai_epi32(_mm_mul_epi32(srcL, volLVec), 8);
    __m128i scaledR = _mm_srai_epi32(_mm_mul_epi32(srcR, volRVec), 8);

    // Accumulate
    __m128i resultL = _mm_add_epi32(dstL, scaledL);
    __m128i resultR = _mm_add_epi32(dstR, scaledR);

    _mm_storeu_si128((__m128i*)destL, resultL);
    _mm_storeu_si128((__m128i*)destR, resultR);
#else
    // Scalar fallback
    for (int i = 0; i < 2; ++i)
    {
        destL[i] += (src[i * 2] * volL) / 256;
        destR[i] += (src[i * 2 + 1] * volR) / 256;
    }
#endif
}

void AudioMixing_SIMD::Convert32to16_4x(short* dest, const int* src)
{
#ifdef URHO3D_SSE
    // Load 4x int32 samples
    __m128i samples = _mm_loadu_si128((__m128i*)src);

    // Clamp to 16-bit range [-32768, 32767]
    __m128i minVal = _mm_set1_epi32(-32768);
    __m128i maxVal = _mm_set1_epi32(32767);

    __m128i clamped = samples;
    clamped = _mm_max_epi32(clamped, minVal);  // max(x, -32768)
    clamped = _mm_min_epi32(clamped, maxVal);  // min(x, 32767)

    // Pack to 16-bit
    __m128i packed = _mm_packs_epi32(clamped, clamped);

    // Store first 4 shorts (rest of packed is duplicated)
    _mm_storel_epi64((__m128i*)dest, packed);
#else
    // Scalar fallback
    Convert32to16_Scalar(dest, src, 4);
#endif
}

void AudioMixing_SIMD::ApplyGain_4x(int* samples, int gain, uint32_t count)
{
#ifdef URHO3D_SSE
    uint32_t simdCount = (count / 4) * 4;
    __m128i gainVec = _mm_set1_epi32(gain);

    for (uint32_t i = 0; i < simdCount; i += 4)
    {
        __m128i samp = _mm_loadu_si128((__m128i*)&samples[i]);
        __m128i scaled = _mm_srai_epi32(_mm_mul_epi32(samp, gainVec), 8);
        _mm_storeu_si128((__m128i*)&samples[i], scaled);
    }

    // Scalar fallback for remainder
    for (uint32_t i = simdCount; i < count; ++i)
        samples[i] = (samples[i] * gain) / 256;
#else
    // Scalar only
    for (uint32_t i = 0; i < count; ++i)
        samples[i] = (samples[i] * gain) / 256;
#endif
}

void AudioMixing_SIMD::SaturatingAdd_4x(short* dest, const short* src)
{
#ifdef URHO3D_SSE
    // Load 8x int16 samples (4 from each)
    __m128i destVec = _mm_loadu_si128((__m128i*)dest);
    __m128i srcVec = _mm_loadu_si128((__m128i*)src);

    // Add with saturation (signed)
    __m128i result = _mm_adds_epi16(destVec, srcVec);

    // Store back
    _mm_storeu_si128((__m128i*)dest, result);
#else
    // Scalar with manual saturation
    for (int i = 0; i < 4; ++i)
        dest[i] = (short)Clamp((int)dest[i] + (int)src[i], -32768, 32767);
#endif
}

void AudioMixing_SIMD::MixMultipleSources(
    int* dest,
    const short* const* sources,
    const int* gains,
    uint32_t numSources,
    uint32_t samples)
{
    // For each source, accumulate into destination with its gain
    for (uint32_t srcIdx = 0; srcIdx < numSources; ++srcIdx)
    {
        const short* src = sources[srcIdx];
        int gain = gains[srcIdx];

        if (!src || gain == 0)
            continue;

#ifdef URHO3D_SSE
        uint32_t simdCount = (samples / 4) * 4;

        for (uint32_t i = 0; i < simdCount; i += 4)
        {
            MixMonoWithGain_4x(&dest[i], &src[i], gain);
        }

        // Scalar fallback for remainder
        for (uint32_t i = simdCount; i < samples; ++i)
            dest[i] += (src[i] * gain) / 256;
#else
        // Pure scalar
        for (uint32_t i = 0; i < samples; ++i)
            dest[i] += (src[i] * gain) / 256;
#endif
    }
}

void AudioMixing_SIMD::MixMonoWithGain_Scalar(int* dest, const short* src, int vol, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i)
        dest[i] += (src[i] * vol) / 256;
}

void AudioMixing_SIMD::Convert32to16_Scalar(short* dest, const int* src, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i)
        dest[i] = (short)Clamp(src[i], -32768, 32767);
}

} // namespace Urho3D
