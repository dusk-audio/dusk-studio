#pragma once

#include <algorithm>
#include <cmath>
#if defined(__SSE2__) || defined(_M_X64)
 #include <emmintrin.h>
 #define DUSK_AUDIO_VECTOR_SIMD 1
#elif defined(__ARM_NEON)
 #include <arm_neon.h>
 #include <cstdint>
 #define DUSK_AUDIO_VECTOR_SIMD 1
#endif
// Small, allocation-free float-vector operations for real-time audio buffers.
namespace dusk::audio
{
struct FloatMinMax
{
    float min;
    float max;
};
namespace detail
{
inline FloatMinMax scalarFindSignedMinMax (const float* data, int count) noexcept
{
    if (count <= 0) return { 0.0f, 0.0f };
    float minValue = data[0];
    float maxValue = data[0];
    for (int i = 1; i < count; ++i)
    {
        minValue = std::min (minValue, data[i]);
        maxValue = std::max (maxValue, data[i]);
    }
    return { minValue, maxValue };
}
#if defined(__SSE2__) || defined(_M_X64)
using Vec4 = __m128;
using Mask4 = __m128;
inline Vec4 load4 (const float* p) noexcept { return _mm_loadu_ps (p); }
inline void store4 (float* p, Vec4 v) noexcept { _mm_storeu_ps (p, v); }
inline Vec4 zero4() noexcept { return _mm_setzero_ps(); }
inline Vec4 add4 (Vec4 a, Vec4 b) noexcept { return _mm_add_ps (a, b); }
inline Vec4 negate4 (Vec4 v) noexcept { return _mm_xor_ps (v, _mm_set1_ps (-0.0f)); }
inline Vec4 min4 (Vec4 values, Vec4 acc) noexcept { return _mm_min_ps (values, acc); }
inline Vec4 max4 (Vec4 values, Vec4 acc) noexcept { return _mm_max_ps (values, acc); }
inline Mask4 unorderedMask (Vec4 v) noexcept { return _mm_cmpunord_ps (v, v); }
inline Mask4 orMask (Mask4 a, Mask4 b) noexcept { return _mm_or_ps (a, b); }
inline bool anyLaneSet (Mask4 m) noexcept { return _mm_movemask_ps (m) != 0; }
#elif defined(__ARM_NEON)
using Vec4 = float32x4_t;
using Mask4 = uint32x4_t;
inline Vec4 load4 (const float* p) noexcept { return vld1q_f32 (p); }
inline void store4 (float* p, Vec4 v) noexcept { vst1q_f32 (p, v); }
inline Vec4 zero4() noexcept { return vdupq_n_f32 (0.0f); }
inline Vec4 add4 (Vec4 a, Vec4 b) noexcept { return vaddq_f32 (a, b); }
inline Vec4 negate4 (Vec4 v) noexcept { return vnegq_f32 (v); }
inline Vec4 min4 (Vec4 values, Vec4 acc) noexcept { return vminq_f32 (values, acc); }
inline Vec4 max4 (Vec4 values, Vec4 acc) noexcept { return vmaxq_f32 (values, acc); }
inline Mask4 unorderedMask (Vec4 v) noexcept { return vmvnq_u32 (vceqq_f32 (v, v)); }
inline Mask4 orMask (Mask4 a, Mask4 b) noexcept { return vorrq_u32 (a, b); }
inline bool anyLaneSet (Mask4 m) noexcept
{
    uint32_t lanes[4];
    vst1q_u32 (lanes, m);
    return lanes[0] != 0 || lanes[1] != 0 || lanes[2] != 0 || lanes[3] != 0;
}
#endif
inline FloatMinMax finishMinMax (const float* mins, const float* maxs,
                                 const float* data, int start, int count) noexcept
{
    FloatMinMax result { mins[0], maxs[0] };
    for (int lane = 1; lane < 4; ++lane)
    {
        result.min = std::min (result.min, mins[lane]);
        result.max = std::max (result.max, maxs[lane]);
    }
    for (int i = start; i < count; ++i)
    {
        if (std::isnan (data[i])) return scalarFindSignedMinMax (data, count);
        result.min = std::min (result.min, data[i]);
        result.max = std::max (result.max, data[i]);
    }
    return result;
}
} // namespace detail
inline void vecClear (float* dst, int count) noexcept
{
#if defined(DUSK_AUDIO_VECTOR_SIMD)
    int i = 0;
    for (; i + 4 <= count; i += 4) detail::store4 (dst + i, detail::zero4());
    for (; i < count; ++i) dst[i] = 0.0f;
#else
    for (int i = 0; i < count; ++i) dst[i] = 0.0f;
#endif
}
inline void vecCopy (float* dst, const float* src, int count) noexcept
{
#if defined(DUSK_AUDIO_VECTOR_SIMD)
    int i = 0;
    for (; i + 4 <= count; i += 4) detail::store4 (dst + i, detail::load4 (src + i));
    for (; i < count; ++i) dst[i] = src[i];
#else
    for (int i = 0; i < count; ++i) dst[i] = src[i];
#endif
}
inline void vecAdd (float* dst, const float* src, int count) noexcept
{
#if defined(DUSK_AUDIO_VECTOR_SIMD)
    int i = 0;
    for (; i + 4 <= count; i += 4)
        detail::store4 (dst + i, detail::add4 (detail::load4 (dst + i), detail::load4 (src + i)));
    for (; i < count; ++i) dst[i] += src[i];
#else
    for (int i = 0; i < count; ++i) dst[i] += src[i];
#endif
}
inline void vecNegate (float* dst, const float* src, int count) noexcept
{
#if defined(DUSK_AUDIO_VECTOR_SIMD)
    int i = 0;
    for (; i + 4 <= count; i += 4) detail::store4 (dst + i, detail::negate4 (detail::load4 (src + i)));
    for (; i < count; ++i) dst[i] = -src[i];
#else
    for (int i = 0; i < count; ++i) dst[i] = -src[i];
#endif
}
// An empty range reports {0, 0}. NaNs follow std::min/std::max operand order:
// the vector pass accumulates the unordered lanes and, if any were set, redoes
// the whole range with the scalar loop so both paths agree.
inline FloatMinMax findSignedMinMax (const float* data, int count) noexcept
{
#if defined(DUSK_AUDIO_VECTOR_SIMD)
    if (count < 8) return detail::scalarFindSignedMinMax (data, count);
    auto minValues = detail::load4 (data);
    auto maxValues = minValues;
    auto unordered = detail::unorderedMask (minValues);
    int i = 4;
    for (; i + 4 <= count; i += 4)
    {
        const auto values = detail::load4 (data + i);
        unordered = detail::orMask (unordered, detail::unorderedMask (values));
        minValues = detail::min4 (values, minValues);
        maxValues = detail::max4 (values, maxValues);
    }
    if (detail::anyLaneSet (unordered)) return detail::scalarFindSignedMinMax (data, count);
    float mins[4];
    float maxs[4];
    detail::store4 (mins, minValues);
    detail::store4 (maxs, maxValues);
    return detail::finishMinMax (mins, maxs, data, i, count);
#else
    return detail::scalarFindSignedMinMax (data, count);
#endif
}
} // namespace dusk::audio
#undef DUSK_AUDIO_VECTOR_SIMD
