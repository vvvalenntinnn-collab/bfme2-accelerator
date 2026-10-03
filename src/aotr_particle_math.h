#pragma once
#include <xmmintrin.h>

// Copy RGB/alpha and clamp in one pass. Comparisons and bit selection preserve
// NaN payloads and signed zero; min/max instructions do not have that contract.
static void particleColorPack(float* dst, const float* rgb, const float* alpha, unsigned count) {
    const __m128 zero = _mm_setzero_ps(), one = _mm_set1_ps(1.0f);
    for (unsigned i = 0; i < count; ++i) {
        __m128 value = one;
        __m128 a = alpha ? _mm_load_ss(alpha + i) : one;
        if (rgb) {
            const float* c = rgb + i * 3;
            __m128 xy = _mm_loadl_pi(zero, (const __m64*)c);
            __m128 za = _mm_unpacklo_ps(_mm_load_ss(c + 2), a);
            value = _mm_movelh_ps(xy, za);
        } else {
            value = _mm_movelh_ps(one, _mm_unpacklo_ps(one, a));
        }
        __m128 low = _mm_cmplt_ps(value, zero);
        value = _mm_andnot_ps(low, value);
        __m128 high = _mm_cmpgt_ps(value, one);
        value = _mm_or_ps(_mm_and_ps(high, one), _mm_andnot_ps(high, value));
        _mm_storeu_ps(dst + i * 4, value);
    }
}
