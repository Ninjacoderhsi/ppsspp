// Copyright (c) 2013- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include "ppsspp_config.h"
#include <algorithm>
#include <cmath>

#include "Common/Data/Convert/ColorConv.h"
#include "Common/Profiler/Profiler.h"
#include "Common/StringUtils.h"
#include "Common/Thread/ParallelLoop.h"
#include "Core/ThreadPools.h"
#include "Core/Config.h"
#include "Core/Debugger/MemBlockInfo.h"
#include "Core/MemMap.h"
#include "Core/Reporting.h"
#include "Core/ThreadPools.h"
#include "GPU/GPUState.h"

#include "GPU/Common/TextureDecoder.h"
#include "GPU/Software/DrawPixel.h"
#include "GPU/Software/Rasterizer.h"
#include "GPU/Software/Sampler.h"
#include "GPU/Software/SoftGpu.h"

#if defined(_M_SSE)
#include <emmintrin.h>
#endif

namespace Rasterizer {

// Only OK on x64 where our stack is aligned
#if defined(_M_SSE) && !PPSSPP_ARCH(X86)
static inline __m128 Interpolate(const __m128 &c0, const __m128 &c1, const __m128 &c2, int w0, int w1, int w2, float wsum) {
	__m128 v = _mm_mul_ps(c0, _mm_cvtepi32_ps(_mm_set1_epi32(w0)));
	v = _mm_add_ps(v, _mm_mul_ps(c1, _mm_cvtepi32_ps(_mm_set1_epi32(w1))));
	v = _mm_add_ps(v, _mm_mul_ps(c2, _mm_cvtepi32_ps(_mm_set1_epi32(w2))));
	return _mm_mul_ps(v, _mm_set_ps1(wsum));
}

static inline __m128i Interpolate(const __m128i &c0, const __m128i &c1, const __m128i &c2, int w0, int w1, int w2, float wsum) {
	return _mm_cvtps_epi32(Interpolate(_mm_cvtepi32_ps(c0), _mm_cvtepi32_ps(c1), _mm_cvtepi32_ps(c2), w0, w1, w2, wsum));
}
#endif

// NOTE: When not casting color0 and color1 to float vectors, this code suffers from severe overflow issues.
// Not sure if that should be regarded as a bug or if casting to float is a valid fix.

static inline Vec4<int> Interpolate(const Vec4<int> &c0, const Vec4<int> &c1, const Vec4<int> &c2, int w0, int w1, int w2, float wsum) {
#if defined(_M_SSE) && !PPSSPP_ARCH(X86)
	return Vec4<int>(Interpolate(c0.ivec, c1.ivec, c2.ivec, w0, w1, w2, wsum));
#else
	return ((c0.Cast<float>() * w0 + c1.Cast<float>() * w1 + c2.Cast<float>() * w2) * wsum).Cast<int>();
#endif
}

static inline Vec3<int> Interpolate(const Vec3<int> &c0, const Vec3<int> &c1, const Vec3<int> &c2, int w0, int w1, int w2, float wsum) {
#if defined(_M_SSE) && !PPSSPP_ARCH(X86)
	return Vec3<int>(Interpolate(c0.ivec, c1.ivec, c2.ivec, w0, w1, w2, wsum));
#else
	return ((c0.Cast<float>() * w0 + c1.Cast<float>() * w1 + c2.Cast<float>() * w2) * wsum).Cast<int>();
#endif
}

static inline Vec2<float> Interpolate(const Vec2<float> &c0, const Vec2<float> &c1, const Vec2<float> &c2, int w0, int w1, int w2, float wsum) {
#if defined(_M_SSE) && !PPSSPP_ARCH(X86)
	return Vec2<float>(Interpolate(c0.vec, c1.vec, c2.vec, w0, w1, w2, wsum));
#else
	return (c0 * w0 + c1 * w1 + c2 * w2) * wsum;
#endif
}

static inline Vec4<float> Interpolate(const float &c0, const float &c1, const float &c2, const Vec4<float> &w0, const Vec4<float> &w1, const Vec4<float> &w2, const Vec4<float> &wsum_recip) {
#if defined(_M_SSE) && !PPSSPP_ARCH(X86)
	__m128 v = _mm_mul_ps(w0.vec, _mm_set1_ps(c0));
	v = _mm_add_ps(v, _mm_mul_ps(w1.vec, _mm_set1_ps(c1)));
	v = _mm_add_ps(v, _mm_mul_ps(w2.vec, _mm_set1_ps(c2)));
	return _mm_mul_ps(v, wsum_recip.vec);
#else
	return (w0 * c0 + w1 * c1 + w2 * c2) * wsum_recip;
#endif
}

static inline Vec4<float> Interpolate(const float &c0, const float &c1, const float &c2, const Vec4<int> &w0, const Vec4<int> &w1, const Vec4<int> &w2, const Vec4<float> &wsum_recip) {
	return Interpolate(c0, c1, c2, w0.Cast<float>(), w1.Cast<float>(), w2.Cast<float>(), wsum_recip);
}

static inline u8 ClampFogDepth(float fogdepth) {
	union FloatBits {
		float f;
		u32 u;
	};
	FloatBits f;
	f.f = fogdepth;

	u32 exp = f.u >> 23;
	if ((f.u & 0x80000000) != 0 || exp <= 126 - 8)
		return 0;
	if (exp > 126)
		return 255;

	u32 mantissa = (f.u & 0x007FFFFF) | 0x00800000;
	return mantissa >> (16 + 126 - exp);
}

static inline void GetTextureCoordinates(const VertexData& v0, const VertexData& v1, const float p, float &s, float &t) {
	// All UV gen modes, by the time they get here, behave the same.

	// TODO: What happens if vertex has no texture coordinates?
	// Note that for environment mapping, texture coordinates have been calculated during lighting
	float q0 = 1.f / v0.clippos.w;
	float q1 = 1.f / v1.clippos.w;
	float wq0 = p * q0;
	float wq1 = (1.0f - p) * q1;

	float q_recip = 1.0f / (wq0 + wq1);
	s = (v0.texturecoords.s() * wq0 + v1.texturecoords.s() * wq1) * q_recip;
	t = (v0.texturecoords.t() * wq0 + v1.texturecoords.t() * wq1) * q_recip;
}

static inline void GetTextureCoordinates(const VertexData &v0, const VertexData &v1, const VertexData &v2, const Vec4<int> &w0, const Vec4<int> &w1, const Vec4<int> &w2, const Vec4<float> &wsum_recip, Vec4<float> &s, Vec4<float> &t) {
	// All UV gen modes, by the time they get here, behave the same.

	// TODO: What happens if vertex has no texture coordinates?
	// Note that for environment mapping, texture coordinates have been calculated during lighting.
	float q0 = 1.f / v0.clippos.w;
	float q1 = 1.f / v1.clippos.w;
	float q2 = 1.f / v2.clippos.w;
	Vec4<float> wq0 = w0.Cast<float>() * q0;
	Vec4<float> wq1 = w1.Cast<float>() * q1;
	Vec4<float> wq2 = w2.Cast<float>() * q2;

	Vec4<float> q_recip = (wq0 + wq1 + wq2).Reciprocal();
	s = Interpolate(v0.texturecoords.s(), v1.texturecoords.s(), v2.texturecoords.s(), wq0, wq1, wq2, q_recip);
	t = Interpolate(v0.texturecoords.t(), v1.texturecoords.t(), v2.texturecoords.t(), wq0, wq1, wq2, q_recip);
}

static inline void SetPixelDepth(int x, int y, u16 value)
{
	depthbuf.Set16(x, y, gstate.DepthBufStride(), value);
}

static inline u8 GetPixelStencil(GEBufferFormat fmt, int x, int y) {
	if (fmt == GE_FORMAT_565) {
		// Always treated as 0 for comparison purposes.
		return 0;
	} else if (fmt == GE_FORMAT_5551) {
		return ((fb.Get16(x, y, gstate.FrameBufStride()) & 0x8000) != 0) ? 0xFF : 0;
	} else if (fmt == GE_FORMAT_4444) {
		return Convert4To8(fb.Get16(x, y, gstate.FrameBufStride()) >> 12);
	} else {
		return fb.Get32(x, y, gstate.FrameBufStride()) >> 24;
	}
}

static inline bool IsRightSideOrFlatBottomLine(const Vec2<int>& vertex, const Vec2<int>& line1, const Vec2<int>& line2)
{
	if (line1.y == line2.y) {
		// just check if vertex is above us => bottom line parallel to x-axis
		return vertex.y < line1.y;
	} else {
		// check if vertex is on our left => right side
		return vertex.x < line1.x + (line2.x - line1.x) * (vertex.y - line1.y) / (line2.y - line1.y);
	}
}

Vec4IntResult SOFTRAST_CALL GetTextureFunctionOutput(Vec4IntArg prim_color_in, Vec4IntArg texcolor_in) {
	const Vec4<int> prim_color = prim_color_in;
	const Vec4<int> texcolor = texcolor_in;

	Vec3<int> out_rgb;
	int out_a;

	bool rgba = gstate.isTextureAlphaUsed();

	switch (gstate.getTextureFunction()) {
	case GE_TEXFUNC_MODULATE:
	{
#if defined(_M_SSE)
		// Modulate weights slightly on the tex color, by adding one to prim and dividing by 256.
		const __m128i p = _mm_slli_epi16(_mm_packs_epi32(prim_color.ivec, prim_color.ivec), 4);
		const __m128i pboost = _mm_add_epi16(p, _mm_set1_epi16(1 << 4));
		__m128i t = _mm_slli_epi16(_mm_packs_epi32(texcolor.ivec, texcolor.ivec), 4);
		if (gstate.isColorDoublingEnabled()) {
			const __m128i amask = _mm_set_epi16(-1, 0, 0, 0, -1, 0, 0, 0);
			const __m128i a = _mm_and_si128(t, amask);
			const __m128i rgb = _mm_andnot_si128(amask, t);
			t = _mm_or_si128(_mm_slli_epi16(rgb, 1), a);
		}
		const __m128i b = _mm_mulhi_epi16(pboost, t);
		out_rgb.ivec = _mm_unpacklo_epi16(b, _mm_setzero_si128());

		if (rgba) {
			return ToVec4IntResult(Vec4<int>(out_rgb.ivec));
		} else {
			out_a = prim_color.a();
		}
#else
		if (gstate.isColorDoublingEnabled()) {
			out_rgb = ((prim_color.rgb() + Vec3<int>::AssignToAll(1)) * texcolor.rgb() * 2) / 256;
		} else {
			out_rgb = (prim_color.rgb() + Vec3<int>::AssignToAll(1)) * texcolor.rgb() / 256;
		}
		out_a = (rgba) ? ((prim_color.a() + 1) * texcolor.a() / 256) : prim_color.a();
#endif
		break;
	}

	case GE_TEXFUNC_DECAL:
	{
		if (rgba) {
			int t = texcolor.a();
			int invt = 255 - t;
			// Both colors are boosted here, making the alpha have more weight.
			Vec3<int> one = Vec3<int>::AssignToAll(1);
			out_rgb = ((prim_color.rgb() + one) * invt + (texcolor.rgb() + one) * t);
			// Keep the bits of accuracy when doubling.
			if (gstate.isColorDoublingEnabled())
				out_rgb /= 128;
			else
				out_rgb /= 256;
		} else {
			out_rgb = texcolor.rgb();
		}
		out_a = prim_color.a();
		break;
	}

	case GE_TEXFUNC_BLEND:
	{
		const Vec3<int> const255(255, 255, 255);
		const Vec3<int> texenv(gstate.getTextureEnvColR(), gstate.getTextureEnvColG(), gstate.getTextureEnvColB());

		// Unlike the others (and even alpha), this one simply always rounds up.
		const Vec3<int> roundup = Vec3<int>::AssignToAll(255);
		out_rgb = ((const255 - texcolor.rgb()) * prim_color.rgb() + texcolor.rgb() * texenv + roundup);
		// Must divide by less to keep the precision for doubling to be accurate.
		if (gstate.isColorDoublingEnabled())
			out_rgb /= 128;
		else
			out_rgb /= 256;

		out_a = (rgba) ? ((prim_color.a() + 1) * texcolor.a() / 256) : prim_color.a();
		break;
	}

	case GE_TEXFUNC_REPLACE:
		out_rgb = texcolor.rgb();
		// Doubling even happens for replace.
		if (gstate.isColorDoublingEnabled())
			out_rgb *= 2;
		out_a = (rgba) ? texcolor.a() : prim_color.a();
		break;

	case GE_TEXFUNC_ADD:
	case GE_TEXFUNC_UNKNOWN1:
	case GE_TEXFUNC_UNKNOWN2:
	case GE_TEXFUNC_UNKNOWN3:
		// Don't need to clamp afterward, we always clamp before tests.
		out_rgb = prim_color.rgb() + texcolor.rgb();
		if (gstate.isColorDoublingEnabled())
			out_rgb *= 2;

		// Alpha is still blended the common way.
		out_a = (rgba) ? ((prim_color.a() + 1) * texcolor.a() / 256) : prim_color.a();
		break;
	}

	return ToVec4IntResult(Vec4<int>(out_rgb, out_a));
}

static inline Vec3<int> GetSourceFactor(GEBlendSrcFactor factor, const Vec4<int> &source, const Vec4<int> &dst) {
	switch (factor) {
	case GE_SRCBLEND_DSTCOLOR:
		return dst.rgb();

	case GE_SRCBLEND_INVDSTCOLOR:
		return Vec3<int>::AssignToAll(255) - dst.rgb();

	case GE_SRCBLEND_SRCALPHA:
#if defined(_M_SSE)
		return Vec3<int>(_mm_shuffle_epi32(source.ivec, _MM_SHUFFLE(3, 3, 3, 3)));
#else
		return Vec3<int>::AssignToAll(source.a());
#endif

	case GE_SRCBLEND_INVSRCALPHA:
#if defined(_M_SSE)
		return Vec3<int>(_mm_sub_epi32(_mm_set1_epi32(255), _mm_shuffle_epi32(source.ivec, _MM_SHUFFLE(3, 3, 3, 3))));
#else
		return Vec3<int>::AssignToAll(255 - source.a());
#endif

	case GE_SRCBLEND_DSTALPHA:
		return Vec3<int>::AssignToAll(dst.a());

	case GE_SRCBLEND_INVDSTALPHA:
		return Vec3<int>::AssignToAll(255 - dst.a());

	case GE_SRCBLEND_DOUBLESRCALPHA:
		return Vec3<int>::AssignToAll(2 * source.a());

	case GE_SRCBLEND_DOUBLEINVSRCALPHA:
		return Vec3<int>::AssignToAll(255 - std::min(2 * source.a(), 255));

	case GE_SRCBLEND_DOUBLEDSTALPHA:
		return Vec3<int>::AssignToAll(2 * dst.a());

	case GE_SRCBLEND_DOUBLEINVDSTALPHA:
		return Vec3<int>::AssignToAll(255 - std::min(2 * dst.a(), 255));

	case GE_SRCBLEND_FIXA:
	default:
		// All other dest factors (> 10) are treated as FIXA.
		return Vec3<int>::FromRGB(gstate.getFixA());
	}
}

static inline Vec3<int> GetDestFactor(GEBlendDstFactor factor, const Vec4<int> &source, const Vec4<int> &dst) {
	switch (factor) {
	case GE_DSTBLEND_SRCCOLOR:
		return source.rgb();

	case GE_DSTBLEND_INVSRCCOLOR:
		return Vec3<int>::AssignToAll(255) - source.rgb();

	case GE_DSTBLEND_SRCALPHA:
#if defined(_M_SSE)
		return Vec3<int>(_mm_shuffle_epi32(source.ivec, _MM_SHUFFLE(3, 3, 3, 3)));
#else
		return Vec3<int>::AssignToAll(source.a());
#endif

	case GE_DSTBLEND_INVSRCALPHA:
#if defined(_M_SSE)
		return Vec3<int>(_mm_sub_epi32(_mm_set1_epi32(255), _mm_shuffle_epi32(source.ivec, _MM_SHUFFLE(3, 3, 3, 3))));
#else
		return Vec3<int>::AssignToAll(255 - source.a());
#endif

	case GE_DSTBLEND_DSTALPHA:
		return Vec3<int>::AssignToAll(dst.a());

	case GE_DSTBLEND_INVDSTALPHA:
		return Vec3<int>::AssignToAll(255 - dst.a());

	case GE_DSTBLEND_DOUBLESRCALPHA:
		return Vec3<int>::AssignToAll(2 * source.a());

	case GE_DSTBLEND_DOUBLEINVSRCALPHA:
		return Vec3<int>::AssignToAll(255 - std::min(2 * source.a(), 255));

	case GE_DSTBLEND_DOUBLEDSTALPHA:
		return Vec3<int>::AssignToAll(2 * dst.a());

	case GE_DSTBLEND_DOUBLEINVDSTALPHA:
		return Vec3<int>::AssignToAll(255 - std::min(2 * dst.a(), 255));

	case GE_DSTBLEND_FIXB:
	default:
		// All other dest factors (> 10) are treated as FIXB.
		return Vec3<int>::FromRGB(gstate.getFixB());
	}
}

// Removed inline here - it was never chosen to be inlined by the compiler anyway, too complex.
Vec3<int> AlphaBlendingResult(const PixelFuncID &pixelID, const Vec4<int> &source, const Vec4<int> &dst)
{
	// Note: These factors cannot go below 0, but they can go above 255 when doubling.
	Vec3<int> srcfactor = GetSourceFactor(GEBlendSrcFactor(pixelID.AlphaBlendSrc()), source, dst);
	Vec3<int> dstfactor = GetDestFactor(GEBlendDstFactor(pixelID.AlphaBlendDst()), source, dst);

	switch (pixelID.AlphaBlendEq()) {
	case GE_BLENDMODE_MUL_AND_ADD:
	{
#if defined(_M_SSE)
		// We switch to 16 bit to use mulhi, and we use 4 bits of decimal to make the 16 bit shift free.
		const __m128i half = _mm_set1_epi16(1 << 3);

		const __m128i srgb = _mm_add_epi16(_mm_slli_epi16(_mm_packs_epi32(source.ivec, source.ivec), 4), half);
		const __m128i sf = _mm_add_epi16(_mm_slli_epi16(_mm_packs_epi32(srcfactor.ivec, srcfactor.ivec), 4), half);
		const __m128i s = _mm_mulhi_epi16(srgb, sf);

		const __m128i drgb = _mm_add_epi16(_mm_slli_epi16(_mm_packs_epi32(dst.ivec, dst.ivec), 4), half);
		const __m128i df = _mm_add_epi16(_mm_slli_epi16(_mm_packs_epi32(dstfactor.ivec, dstfactor.ivec), 4), half);
		const __m128i d = _mm_mulhi_epi16(drgb, df);

		return Vec3<int>(_mm_unpacklo_epi16(_mm_adds_epi16(s, d), _mm_setzero_si128()));
#else
		Vec3<int> half = Vec3<int>::AssignToAll(1);
		Vec3<int> lhs = ((source.rgb() * 2 + half) * (srcfactor * 2 + half)) / 1024;
		Vec3<int> rhs = ((dst.rgb() * 2 + half) * (dstfactor * 2 + half)) / 1024;
		return lhs + rhs;
#endif
	}

	case GE_BLENDMODE_MUL_AND_SUBTRACT:
	{
#if defined(_M_SSE)
		const __m128i half = _mm_set1_epi16(1 << 3);

		const __m128i srgb = _mm_add_epi16(_mm_slli_epi16(_mm_packs_epi32(source.ivec, source.ivec), 4), half);
		const __m128i sf = _mm_add_epi16(_mm_slli_epi16(_mm_packs_epi32(srcfactor.ivec, srcfactor.ivec), 4), half);
		const __m128i s = _mm_mulhi_epi16(srgb, sf);

		const __m128i drgb = _mm_add_epi16(_mm_slli_epi16(_mm_packs_epi32(dst.ivec, dst.ivec), 4), half);
		const __m128i df = _mm_add_epi16(_mm_slli_epi16(_mm_packs_epi32(dstfactor.ivec, dstfactor.ivec), 4), half);
		const __m128i d = _mm_mulhi_epi16(drgb, df);

		return Vec3<int>(_mm_unpacklo_epi16(_mm_max_epi16(_mm_subs_epi16(s, d), _mm_setzero_si128()), _mm_setzero_si128()));
#else
		Vec3<int> half = Vec3<int>::AssignToAll(1);
		Vec3<int> lhs = ((source.rgb() * 2 + half) * (srcfactor * 2 + half)) / 1024;
		Vec3<int> rhs = ((dst.rgb() * 2 + half) * (dstfactor * 2 + half)) / 1024;
		return lhs - rhs;
#endif
	}

	case GE_BLENDMODE_MUL_AND_SUBTRACT_REVERSE:
	{
#if defined(_M_SSE)
		const __m128i half = _mm_set1_epi16(1 << 3);

		const __m128i srgb = _mm_add_epi16(_mm_slli_epi16(_mm_packs_epi32(source.ivec, source.ivec), 4), half);
		const __m128i sf = _mm_add_epi16(_mm_slli_epi16(_mm_packs_epi32(srcfactor.ivec, srcfactor.ivec), 4), half);
		const __m128i s = _mm_mulhi_epi16(srgb, sf);

		const __m128i drgb = _mm_add_epi16(_mm_slli_epi16(_mm_packs_epi32(dst.ivec, dst.ivec), 4), half);
		const __m128i df = _mm_add_epi16(_mm_slli_epi16(_mm_packs_epi32(dstfactor.ivec, dstfactor.ivec), 4), half);
		const __m128i d = _mm_mulhi_epi16(drgb, df);

		return Vec3<int>(_mm_unpacklo_epi16(_mm_max_epi16(_mm_subs_epi16(d, s), _mm_setzero_si128()), _mm_setzero_si128()));
#else
		Vec3<int> half = Vec3<int>::AssignToAll(1);
		Vec3<int> lhs = ((source.rgb() * 2 + half) * (srcfactor * 2 + half)) / 1024;
		Vec3<int> rhs = ((dst.rgb() * 2 + half) * (dstfactor * 2 + half)) / 1024;
		return rhs - lhs;
#endif
	}

	case GE_BLENDMODE_MIN:
		return Vec3<int>(std::min(source.r(), dst.r()),
						std::min(source.g(), dst.g()),
						std::min(source.b(), dst.b()));

	case GE_BLENDMODE_MAX:
		return Vec3<int>(std::max(source.r(), dst.r()),
						std::max(source.g(), dst.g()),
						std::max(source.b(), dst.b()));

	case GE_BLENDMODE_ABSDIFF:
		return Vec3<int>(::abs(source.r() - dst.r()),
						::abs(source.g() - dst.g()),
						::abs(source.b() - dst.b()));

	default:
		return source.rgb();
	}
}

static inline Vec4IntResult SOFTRAST_CALL ApplyTexturing(float s, float t, int x, int y, Vec4IntArg prim_color, u8 *texptr[], int texbufw[], int texlevel, int frac_texlevel, bool bilinear, Sampler::Funcs sampler) {
	const u8 **tptr0 = const_cast<const u8 **>(&texptr[texlevel]);
	const int *bufw0 = &texbufw[texlevel];

	if (!bilinear) {
		return sampler.nearest(s, t, x, y, prim_color, tptr0, bufw0, texlevel, frac_texlevel);
	}
	return sampler.linear(s, t, x, y, prim_color, tptr0, bufw0, texlevel, frac_texlevel);
}

static inline Vec4IntResult SOFTRAST_CALL ApplyTexturingSingle(float s, float t, int x, int y, Vec4IntArg prim_color, u8 *texptr[], int texbufw[], int texlevel, int frac_texlevel, bool bilinear, Sampler::Funcs sampler) {
	return ApplyTexturing(s, t, ((x & 15) + 1) / 2, ((y & 15) + 1) / 2, prim_color, texptr, texbufw, texlevel, frac_texlevel, bilinear, sampler);
}

// Produces a signed 1.27.4 value.
static int TexLog2(float delta) {
	union FloatBits {
		float f;
		u32 u;
	};
	FloatBits f;
	f.f = delta;
	// Use the exponent as the tex level, and the top mantissa bits for a frac.
	// We can't support more than 4 bits of frac, so truncate.
	int useful = (f.u >> 19) & 0x0FFF;
	// Now offset so the exponent aligns with log2f (exp=127 is 0.)
	return useful - 127 * 16;
}

static inline void CalculateSamplingParams(const float ds, const float dt, const int maxTexLevel, int &level, int &levelFrac, bool &filt) {
	const int width = gstate.getTextureWidth(0);
	const int height = gstate.getTextureHeight(0);

	// With 8 bits of fraction (because texslope can be fairly precise.)
	int detail;
	switch (gstate.getTexLevelMode()) {
	case GE_TEXLEVEL_MODE_AUTO:
		detail = TexLog2(std::max(ds * width, dt * height));
		break;
	case GE_TEXLEVEL_MODE_SLOPE:
		// This is always offset by an extra texlevel.
		detail = 1 * 16 + TexLog2(gstate.getTextureLodSlope());
		break;
	case GE_TEXLEVEL_MODE_CONST:
	default:
		// Unused value 3 operates the same as CONST.
		detail = 0;
		break;
	}

	// Add in the bias (used in all modes), expanding to 8 bits of fraction.
	detail += gstate.getTexLevelOffset16();

	if (detail > 0 && maxTexLevel > 0) {
		bool mipFilt = gstate.isMipmapFilteringEnabled();

		int level8 = std::min(detail, maxTexLevel * 16);
		if (!mipFilt) {
			// Round up at 1.5.
			level8 += 8;
		}
		level = level8 >> 4;
		levelFrac = mipFilt ? level8 & 0xF : 0;
	} else {
		level = 0;
		levelFrac = 0;
	}

	if (g_Config.iTexFiltering == TEX_FILTER_FORCE_LINEAR) {
		filt = true;
	} else if (g_Config.iTexFiltering == TEX_FILTER_FORCE_NEAREST) {
		filt = false;
	} else {
		filt = detail > 0 ? gstate.isMinifyFilteringEnabled() : gstate.isMagnifyFilteringEnabled();
	}
}

static inline void ApplyTexturing(Sampler::Funcs sampler, Vec4<int> *prim_color, const Vec4<int> &mask, const Vec4<float> &s, const Vec4<float> &t, int maxTexLevel, u8 *texptr[], int texbufw[], int x, int y) {
	float ds = s[1] - s[0];
	float dt = t[2] - t[0];

	int level;
	int levelFrac;
	bool bilinear;
	CalculateSamplingParams(ds, dt, maxTexLevel, level, levelFrac, bilinear);

	PROFILE_THIS_SCOPE("sampler");
	for (int i = 0; i < 4; ++i) {
		if (mask[i] >= 0)
			prim_color[i] = ApplyTexturing(s[i], t[i], ((x & 15) + 1) / 2, ((y & 15) + 1) / 2, ToVec4IntArg(prim_color[i]), texptr, texbufw, level, levelFrac, bilinear, sampler);
	}
}

struct TriangleEdge {
	Vec4<int> Start(const ScreenCoords &v0, const ScreenCoords &v1, const ScreenCoords &origin);
	inline Vec4<int> StepX(const Vec4<int> &w);
	inline Vec4<int> StepY(const Vec4<int> &w);

	Vec4<int> stepX;
	Vec4<int> stepY;
};

Vec4<int> TriangleEdge::Start(const ScreenCoords &v0, const ScreenCoords &v1, const ScreenCoords &origin) {
	// Start at pixel centers.
	Vec4<int> initX = Vec4<int>::AssignToAll(origin.x) + Vec4<int>(7, 23, 7, 23);
	Vec4<int> initY = Vec4<int>::AssignToAll(origin.y) + Vec4<int>(7, 7, 23, 23);

	// orient2d refactored.
	int xf = v0.y - v1.y;
	int yf = v1.x - v0.x;
	int c = v1.y * v0.x - v1.x * v0.y;

	stepX = Vec4<int>::AssignToAll(xf * 16 * 2);
	stepY = Vec4<int>::AssignToAll(yf * 16 * 2);

	return Vec4<int>::AssignToAll(xf) * initX + Vec4<int>::AssignToAll(yf) * initY + Vec4<int>::AssignToAll(c);
}

inline Vec4<int> TriangleEdge::StepX(const Vec4<int> &w) {
#if defined(_M_SSE) && !PPSSPP_ARCH(X86)
	return _mm_add_epi32(w.ivec, stepX.ivec);
#else
	return w + stepX;
#endif
}

inline Vec4<int> TriangleEdge::StepY(const Vec4<int> &w) {
#if defined(_M_SSE) && !PPSSPP_ARCH(X86)
	return _mm_add_epi32(w.ivec, stepY.ivec);
#else
	return w + stepY;
#endif
}

static inline Vec4<int> MakeMask(const Vec4<int> &w0, const Vec4<int> &w1, const Vec4<int> &w2, const Vec4<int> &bias0, const Vec4<int> &bias1, const Vec4<int> &bias2, const Vec4<int> &scissor) {
#if defined(_M_SSE) && !PPSSPP_ARCH(X86)
	__m128i biased0 = _mm_add_epi32(w0.ivec, bias0.ivec);
	__m128i biased1 = _mm_add_epi32(w1.ivec, bias1.ivec);
	__m128i biased2 = _mm_add_epi32(w2.ivec, bias2.ivec);

	return _mm_or_si128(_mm_or_si128(biased0, _mm_or_si128(biased1, biased2)), scissor.ivec);
#else
	return (w0 + bias0) | (w1 + bias1) | (w2 + bias2) | scissor;
#endif
}

static inline bool AnyMask(const Vec4<int> &mask) {
#if defined(_M_SSE) && !PPSSPP_ARCH(X86)
	// In other words: !(mask.x < 0 && mask.y < 0 && mask.z < 0 && mask.w < 0)
	__m128i low2 = _mm_and_si128(mask.ivec, _mm_shuffle_epi32(mask.ivec, _MM_SHUFFLE(3, 2, 3, 2)));
	__m128i low1 = _mm_and_si128(low2, _mm_shuffle_epi32(low2, _MM_SHUFFLE(1, 1, 1, 1)));
	// Now we only need to check one sign bit.
	return _mm_cvtsi128_si32(low1) >= 0;
#else
	return mask.x >= 0 || mask.y >= 0 || mask.z >= 0 || mask.w >= 0;
#endif
}

static inline Vec4<float> EdgeRecip(const Vec4<int> &w0, const Vec4<int> &w1, const Vec4<int> &w2) {
#if defined(_M_SSE) && !PPSSPP_ARCH(X86)
	__m128i wsum = _mm_add_epi32(w0.ivec, _mm_add_epi32(w1.ivec, w2.ivec));
	// _mm_rcp_ps loses too much precision.
	return _mm_div_ps(_mm_set1_ps(1.0f), _mm_cvtepi32_ps(wsum));
#else
	return (w0 + w1 + w2).Cast<float>().Reciprocal();
#endif
}

template <bool clearMode>
void DrawTriangleSlice(
	const VertexData& v0, const VertexData& v1, const VertexData& v2,
	int x1, int y1, int x2, int y2,
	const PixelFuncID &pixelID,
	const Rasterizer::SingleFunc &drawPixel,
	const Sampler::Funcs &sampler)
{
	Vec4<int> bias0 = Vec4<int>::AssignToAll(IsRightSideOrFlatBottomLine(v0.screenpos.xy(), v1.screenpos.xy(), v2.screenpos.xy()) ? -1 : 0);
	Vec4<int> bias1 = Vec4<int>::AssignToAll(IsRightSideOrFlatBottomLine(v1.screenpos.xy(), v2.screenpos.xy(), v0.screenpos.xy()) ? -1 : 0);
	Vec4<int> bias2 = Vec4<int>::AssignToAll(IsRightSideOrFlatBottomLine(v2.screenpos.xy(), v0.screenpos.xy(), v1.screenpos.xy()) ? -1 : 0);

	int texbufw[8]{};

	int maxTexLevel = gstate.getTextureMaxLevel();
	u8 *texptr[8]{};

	if (!gstate.isMipmapEnabled()) {
		maxTexLevel = 0;
	}

	if (gstate.isTextureMapEnabled() && !clearMode) {
		GETextureFormat texfmt = gstate.getTextureFormat();
		for (int i = 0; i <= maxTexLevel; i++) {
			u32 texaddr = gstate.getTextureAddress(i);
			texbufw[i] = GetTextureBufw(i, texaddr, texfmt);
			if (Memory::IsValidAddress(texaddr))
				texptr[i] = Memory::GetPointerUnchecked(texaddr);
			else
				texptr[i] = 0;
		}
	}

	TriangleEdge e0;
	TriangleEdge e1;
	TriangleEdge e2;

	int64_t minX = x1, maxX = x2, minY = y1, maxY = y2;

	ScreenCoords pprime(minX, minY, 0);
	Vec4<int> w0_base = e0.Start(v1.screenpos, v2.screenpos, pprime);
	Vec4<int> w1_base = e1.Start(v2.screenpos, v0.screenpos, pprime);
	Vec4<int> w2_base = e2.Start(v0.screenpos, v1.screenpos, pprime);

	// All the z values are the same, no interpolation required.
	// This is common, and when we interpolate, we lose accuracy.
	const bool flatZ = v0.screenpos.z == v1.screenpos.z && v0.screenpos.z == v2.screenpos.z;
	const bool flatColorAll = clearMode || gstate.getShadeMode() != GE_SHADE_GOURAUD;
	const bool flatColor0 = flatColorAll || (v0.color0 == v1.color0 && v0.color0 == v2.color0);
	const bool flatColor1 = flatColorAll || (v0.color1 == v1.color1 && v0.color1 == v2.color1);
	const bool noFog = clearMode || !gstate.isFogEnabled() || (v0.fogdepth >= 1.0f && v1.fogdepth >= 1.0f && v2.fogdepth >= 1.0f);

#if defined(SOFTGPU_MEMORY_TAGGING_DETAILED) || defined(SOFTGPU_MEMORY_TAGGING_BASIC)
	uint32_t bpp = gstate.FrameBufFormat() == GE_FORMAT_8888 ? 4 : 2;
	DisplayList currentList{};
	if (gpuDebug)
		gpuDebug->GetCurrentDisplayList(currentList);
	std::string tag = StringFromFormat("DisplayListT_%08x", currentList.pc);
	std::string ztag = StringFromFormat("DisplayListTZ_%08x", currentList.pc);
#endif

	for (int64_t curY = minY; curY <= maxY; curY += 32,
										w0_base = e0.StepY(w0_base),
										w1_base = e1.StepY(w1_base),
										w2_base = e2.StepY(w2_base)) {
		Vec4<int> w0 = w0_base;
		Vec4<int> w1 = w1_base;
		Vec4<int> w2 = w2_base;

		// TODO: Maybe we can clip the edges instead?
		int scissorYPlus1 = curY + 16 > maxY ? -1 : 0;
		Vec4<int> scissor_mask = Vec4<int>(0, maxX - minX - 16, scissorYPlus1, (maxX - minX - 16) | scissorYPlus1);
		Vec4<int> scissor_step = Vec4<int>(0, -32, 0, -32);

		DrawingCoords p = TransformUnit::ScreenToDrawing(ScreenCoords(minX, curY, 0));

		for (int64_t curX = minX; curX <= maxX; curX += 32,
			w0 = e0.StepX(w0),
			w1 = e1.StepX(w1),
			w2 = e2.StepX(w2),
			scissor_mask = scissor_mask + scissor_step,
			p.x = (p.x + 2) & 0x3FF) {

			// If p is on or inside all edges, render pixel
			Vec4<int> mask = MakeMask(w0, w1, w2, bias0, bias1, bias2, scissor_mask);
			if (AnyMask(mask)) {
				Vec4<float> wsum_recip = EdgeRecip(w0, w1, w2);

				Vec4<int> prim_color[4];
				if (!flatColor0) {
					// Does the PSP do perspective-correct color interpolation? The GC doesn't.
					for (int i = 0; i < 4; ++i) {
						if (mask[i] >= 0)
							prim_color[i] = Interpolate(v0.color0, v1.color0, v2.color0, w0[i], w1[i], w2[i], wsum_recip[i]);
					}
				} else {
					for (int i = 0; i < 4; ++i) {
						prim_color[i] = v2.color0;
					}
				}
				Vec3<int> sec_color[4];
				if (!flatColor1) {
					for (int i = 0; i < 4; ++i) {
						if (mask[i] >= 0)
							sec_color[i] = Interpolate(v0.color1, v1.color1, v2.color1, w0[i], w1[i], w2[i], wsum_recip[i]);
					}
				} else {
					for (int i = 0; i < 4; ++i) {
						sec_color[i] = v2.color1;
					}
				}

				if (gstate.isTextureMapEnabled() && !clearMode) {
					Vec4<float> s, t;
					if (gstate.isModeThrough()) {
						s = Interpolate(v0.texturecoords.s(), v1.texturecoords.s(), v2.texturecoords.s(), w0, w1, w2, wsum_recip);
						t = Interpolate(v0.texturecoords.t(), v1.texturecoords.t(), v2.texturecoords.t(), w0, w1, w2, wsum_recip);

						// For levels > 0, mipmapping is always based on level 0.  Simpler to scale first.
						s *= 1.0f / (float)gstate.getTextureWidth(0);
						t *= 1.0f / (float)gstate.getTextureHeight(0);
					} else {
						// Texture coordinate interpolation must definitely be perspective-correct.
						GetTextureCoordinates(v0, v1, v2, w0, w1, w2, wsum_recip, s, t);
					}

					ApplyTexturing(sampler, prim_color, mask, s, t, maxTexLevel, texptr, texbufw, curX, curY);
				}

				if (!clearMode) {
					for (int i = 0; i < 4; ++i) {
#if defined(_M_SSE)
						// TODO: Tried making Vec4 do this, but things got slower.
						const __m128i sec = _mm_and_si128(sec_color[i].ivec, _mm_set_epi32(0, -1, -1, -1));
						prim_color[i].ivec = _mm_add_epi32(prim_color[i].ivec, sec);
#else
						prim_color[i] += Vec4<int>(sec_color[i], 0);
#endif
					}
				}

				Vec4<int> fog = Vec4<int>::AssignToAll(255);
				if (!noFog) {
					Vec4<float> fogdepths = w0.Cast<float>() * v0.fogdepth + w1.Cast<float>() * v1.fogdepth + w2.Cast<float>() * v2.fogdepth;
					fogdepths = fogdepths * wsum_recip;
					for (int i = 0; i < 4; ++i) {
						fog[i] = ClampFogDepth(fogdepths[i]);
					}
				}

				Vec4<int> z;
				if (flatZ) {
					z = Vec4<int>::AssignToAll(v2.screenpos.z);
				} else {
					// TODO: Is that the correct way to interpolate?
					Vec4<float> zfloats = w0.Cast<float>() * v0.screenpos.z + w1.Cast<float>() * v1.screenpos.z + w2.Cast<float>() * v2.screenpos.z;
					z = (zfloats * wsum_recip).Cast<int>();
				}

				PROFILE_THIS_SCOPE("draw_tri_px");
				DrawingCoords subp = p;
				for (int i = 0; i < 4; ++i) {
					if (mask[i] < 0) {
						continue;
					}
					subp.x = p.x + (i & 1);
					subp.y = p.y + (i / 2);

					drawPixel(subp.x, subp.y, z[i], fog[i], ToVec4IntArg(prim_color[i]), pixelID);

#if defined(SOFTGPU_MEMORY_TAGGING_DETAILED)
					uint32_t row = gstate.getFrameBufAddress() + subp.y * gstate.FrameBufStride() * bpp;
					NotifyMemInfo(MemBlockFlags::WRITE, row + subp.x * bpp, bpp, tag.c_str(), tag.size());
					if (pixelID.depthWrite) {
						row = gstate.getDepthBufAddress() + subp.y * gstate.DepthBufStride() * 2;
						NotifyMemInfo(MemBlockFlags::WRITE, row + subp.x * 2, 2, ztag.c_str(), ztag.size());
					}
#endif
				}
			}
		}
	}

#if !defined(SOFTGPU_MEMORY_TAGGING_DETAILED) && defined(SOFTGPU_MEMORY_TAGGING_BASIC)
	for (int y = minY; y <= maxY; y += 16) {
		DrawingCoords p = TransformUnit::ScreenToDrawing(ScreenCoords(minX, y, 0));
		DrawingCoords pend = TransformUnit::ScreenToDrawing(ScreenCoords(maxX, y, 0));
		uint32_t row = gstate.getFrameBufAddress() + p.y * gstate.FrameBufStride() * bpp;
		NotifyMemInfo(MemBlockFlags::WRITE, row + p.x * bpp, (pend.x - p.x) * bpp, tag.c_str(), tag.size());

		if (pixelID.depthWrite) {
			row = gstate.getDepthBufAddress() + p.y * gstate.DepthBufStride() * 2;
			NotifyMemInfo(MemBlockFlags::WRITE, row + p.x * 2, (pend.x - p.x) * 2, ztag.c_str(), ztag.size());
		}
	}
#endif
}

// Draws triangle, vertices specified in counter-clockwise direction
void DrawTriangle(const VertexData& v0, const VertexData& v1, const VertexData& v2)
{
	PROFILE_THIS_SCOPE("draw_tri");

	Vec2<int> d01((int)v0.screenpos.x - (int)v1.screenpos.x, (int)v0.screenpos.y - (int)v1.screenpos.y);
	Vec2<int> d02((int)v0.screenpos.x - (int)v2.screenpos.x, (int)v0.screenpos.y - (int)v2.screenpos.y);
	Vec2<int> d12((int)v1.screenpos.x - (int)v2.screenpos.x, (int)v1.screenpos.y - (int)v2.screenpos.y);

	// Drop primitives which are not in CCW order by checking the cross product
	if (d01.x * d02.y - d01.y * d02.x < 0)
		return;
	// If all points have identical coords, we'll have 0 weights and not skip properly, so skip here.
	if (d01.x == 0 && d01.y == 0 && d02.x == 0 && d02.y == 0)
		return;

	int minX = std::min(std::min(v0.screenpos.x, v1.screenpos.x), v2.screenpos.x) & ~0xF;
	int minY = std::min(std::min(v0.screenpos.y, v1.screenpos.y), v2.screenpos.y) & ~0xF;
	int maxX = std::max(std::max(v0.screenpos.x, v1.screenpos.x), v2.screenpos.x) | 0xF;
	int maxY = std::max(std::max(v0.screenpos.y, v1.screenpos.y), v2.screenpos.y) | 0xF;

	DrawingCoords scissorTL(gstate.getScissorX1(), gstate.getScissorY1(), 0);
	DrawingCoords scissorBR(gstate.getScissorX2(), gstate.getScissorY2(), 0);
	minX = std::max(minX, (int)TransformUnit::DrawingToScreen(scissorTL).x);
	maxX = std::min(maxX, (int)TransformUnit::DrawingToScreen(scissorBR).x + 15);
	minY = std::max(minY, (int)TransformUnit::DrawingToScreen(scissorTL).y);
	maxY = std::min(maxY, (int)TransformUnit::DrawingToScreen(scissorBR).y + 15);

	// Was it fully outside the scissor?
	if (maxX < minX || maxY < minY)
		return;

	// 32 because we do two pixels at once, and we don't want overlap.
	int rangeY = (maxY - minY + 31) / 32;
	int rangeX = (maxX - minX + 31) / 32;

	PixelFuncID pixelID;
	ComputePixelFuncID(&pixelID);
	Rasterizer::SingleFunc drawPixel = Rasterizer::GetSingleFunc(pixelID);
	Sampler::Funcs sampler = Sampler::GetFuncs();

	auto drawSlice = gstate.isModeClear() ? &DrawTriangleSlice<true> : &DrawTriangleSlice<false>;

	const int MIN_LINES_PER_THREAD = 4;

	if (rangeY >= 12 && rangeX >= rangeY * 4) {
		auto bound = [&](int a, int b) -> void {
			int x1 = minX + a * 16 * 2;
			int x2 = std::min(maxX, minX + b * 16 * 2 - 1);
			drawSlice(v0, v1, v2, x1, minY, x2, maxY, pixelID, drawPixel, sampler);
		};
		ParallelRangeLoop(&g_threadManager, bound, 0, rangeX, MIN_LINES_PER_THREAD);
	} else if (rangeY >= 12 && rangeX >= 12) {
		auto bound = [&](int a, int b) -> void {
			int y1 = minY + a * 16 * 2;
			int y2 = std::min(maxY, minY + b * 16 * 2 - 1);
			drawSlice(v0, v1, v2, minX, y1, maxX, y2, pixelID, drawPixel, sampler);
		};
		ParallelRangeLoop(&g_threadManager, bound, 0, rangeY, MIN_LINES_PER_THREAD);
	} else {
		drawSlice(v0, v1, v2, minX, minY, maxX, maxY, pixelID, drawPixel, sampler);
	}
}

void DrawPoint(const VertexData &v0)
{
	ScreenCoords pos = v0.screenpos;
	Vec4<int> prim_color = v0.color0;
	Vec3<int> sec_color = v0.color1;

	ScreenCoords scissorTL(TransformUnit::DrawingToScreen(DrawingCoords(gstate.getScissorX1(), gstate.getScissorY1(), 0)));
	ScreenCoords scissorBR(TransformUnit::DrawingToScreen(DrawingCoords(gstate.getScissorX2(), gstate.getScissorY2(), 0)));
	// Allow drawing within a pixel's center.
	scissorBR.x += 15;
	scissorBR.y += 15;

	if (pos.x < scissorTL.x || pos.y < scissorTL.y || pos.x > scissorBR.x || pos.y > scissorBR.y)
		return;

	Sampler::Funcs sampler = Sampler::GetFuncs();
	PixelFuncID pixelID;
	ComputePixelFuncID(&pixelID);
	Rasterizer::SingleFunc drawPixel = Rasterizer::GetSingleFunc(pixelID);

	if (gstate.isTextureMapEnabled() && !pixelID.clearMode) {
		int texbufw[8] = {0};

		int maxTexLevel = gstate.getTextureMaxLevel();
		u8 *texptr[8] = {NULL};

		if (!gstate.isMipmapEnabled()) {
			// No mipmapping enabled
			maxTexLevel = 0;
		}

		if (gstate.isTextureMapEnabled() && !pixelID.clearMode) {
			GETextureFormat texfmt = gstate.getTextureFormat();
			for (int i = 0; i <= maxTexLevel; i++) {
				u32 texaddr = gstate.getTextureAddress(i);
				texbufw[i] = GetTextureBufw(i, texaddr, texfmt);
				if (Memory::IsValidAddress(texaddr))
					texptr[i] = Memory::GetPointerUnchecked(texaddr);
				else
					texptr[i] = 0;
			}
		}

		float s = v0.texturecoords.s();
		float t = v0.texturecoords.t();
		if (gstate.isModeThrough()) {
			s *= 1.0f / (float)gstate.getTextureWidth(0);
			t *= 1.0f / (float)gstate.getTextureHeight(0);
		} else {
			// Texture coordinate interpolation must definitely be perspective-correct.
			GetTextureCoordinates(v0, v0, 0.0f, s, t);
		}

		int texLevel;
		int texLevelFrac;
		bool bilinear;
		CalculateSamplingParams(0.0f, 0.0f, maxTexLevel, texLevel, texLevelFrac, bilinear);
		PROFILE_THIS_SCOPE("sampler");
		prim_color = ApplyTexturingSingle(s, t, pos.x, pos.y, ToVec4IntArg(prim_color), texptr, texbufw, texLevel, texLevelFrac, bilinear, sampler);
	}

	if (!pixelID.clearMode)
		prim_color += Vec4<int>(sec_color, 0);

	ScreenCoords pprime = pos;

	DrawingCoords p = TransformUnit::ScreenToDrawing(pprime);
	u16 z = pos.z;

	u8 fog = 255;
	if (gstate.isFogEnabled() && !pixelID.clearMode) {
		fog = ClampFogDepth(v0.fogdepth);
	}

	PROFILE_THIS_SCOPE("draw_px");
	drawPixel(p.x, p.y, z, fog, ToVec4IntArg(prim_color), pixelID);

#if defined(SOFTGPU_MEMORY_TAGGING_DETAILED) || defined(SOFTGPU_MEMORY_TAGGING_BASIC)
	uint32_t bpp = gstate.FrameBufFormat() == GE_FORMAT_8888 ? 4 : 2;
	DisplayList currentList{};
	if (gpuDebug)
		gpuDebug->GetCurrentDisplayList(currentList);
	std::string tag = StringFromFormat("DisplayListP_%08x", currentList.pc);

	uint32_t row = gstate.getFrameBufAddress() + p.y * gstate.FrameBufStride() * bpp;
	NotifyMemInfo(MemBlockFlags::WRITE, row + p.x * bpp, bpp, tag.c_str(), tag.size());

	if (pixelID.depthWrite) {
		std::string ztag = StringFromFormat("DisplayListPZ_%08x", currentList.pc);
		row = gstate.getDepthBufAddress() + p.y * gstate.DepthBufStride() * 2;
		NotifyMemInfo(MemBlockFlags::WRITE, row + p.x * 2, 2, ztag.c_str(), ztag.size());
	}
#endif
}

void ClearRectangle(const VertexData &v0, const VertexData &v1)
{
	int minX = std::min(v0.screenpos.x, v1.screenpos.x) & ~0xF;
	int minY = std::min(v0.screenpos.y, v1.screenpos.y) & ~0xF;
	int maxX = (std::max(v0.screenpos.x, v1.screenpos.x) + 0xF) & ~0xF;
	int maxY = (std::max(v0.screenpos.y, v1.screenpos.y) + 0xF) & ~0xF;

	DrawingCoords scissorTL(gstate.getScissorX1(), gstate.getScissorY1(), 0);
	DrawingCoords scissorBR(gstate.getScissorX2(), gstate.getScissorY2(), 0);
	minX = std::max(minX, (int)TransformUnit::DrawingToScreen(scissorTL).x);
	maxX = std::max(0, std::min(maxX, (int)TransformUnit::DrawingToScreen(scissorBR).x + 16));
	minY = std::max(minY, (int)TransformUnit::DrawingToScreen(scissorTL).y);
	maxY = std::max(0, std::min(maxY, (int)TransformUnit::DrawingToScreen(scissorBR).y + 16));

	DrawingCoords pprime = TransformUnit::ScreenToDrawing(ScreenCoords(minX, minY, 0));
	DrawingCoords pend = TransformUnit::ScreenToDrawing(ScreenCoords(maxX, maxY, 0));

	constexpr int MIN_LINES_PER_THREAD = 32;
	// Min and max are in PSP fixed point screen coordinates, 16 here is for the 4 subpixel bits.
	const int w = (maxX - minX) / 16;
	if (w <= 0)
		return;

#if defined(SOFTGPU_MEMORY_TAGGING_DETAILED) || defined(SOFTGPU_MEMORY_TAGGING_BASIC)
	DisplayList currentList{};
	if (gpuDebug)
		gpuDebug->GetCurrentDisplayList(currentList);
#endif

	if (gstate.isClearModeDepthMask()) {
		const u16 z = v1.screenpos.z;
		const int stride = gstate.DepthBufStride();

		// If both bytes of Z equal, we can just use memset directly which is faster.
		if ((z & 0xFF) == (z >> 8)) {
			ParallelRangeLoop(&g_threadManager, [=](int y1, int y2) {
				DrawingCoords p = pprime;
				for (p.y = y1; p.y < y2; ++p.y) {
					u16 *row = depthbuf.Get16Ptr(p.x, p.y, stride);
					memset(row, z, w * 2);
				}
			}, pprime.y, pend.y, MIN_LINES_PER_THREAD);
		} else {
			ParallelRangeLoop(&g_threadManager, [=](int y1, int y2) {
				DrawingCoords p = pprime;
				for (p.y = y1; p.y < y2; ++p.y) {
					for (int x = 0; x < w; ++x) {
						SetPixelDepth(p.x + x, p.y, z);
					}
				}
			}, pprime.y, pend.y, MIN_LINES_PER_THREAD);
		}

#if defined(SOFTGPU_MEMORY_TAGGING_DETAILED) || defined(SOFTGPU_MEMORY_TAGGING_BASIC)
		std::string tag = StringFromFormat("DisplayListXZ_%08x", currentList.pc);
		for (int y = pprime.y; y < pend.y; ++y) {
			uint32_t row = gstate.getDepthBufAddress() + y * gstate.DepthBufStride() * 2;
			NotifyMemInfo(MemBlockFlags::WRITE, row + pprime.x * 2, w * 2, tag.c_str(), tag.size());
		}
#endif
	}

	// Note: this stays 0xFFFFFFFF if keeping color and alpha, even for 16-bit.
	u32 keepOldMask = 0xFFFFFFFF;
	if (gstate.isClearModeColorMask())
		keepOldMask &= 0xFF000000;
	if (gstate.isClearModeAlphaMask())
		keepOldMask &= 0x00FFFFFF;

	// The pixel write masks are respected in clear mode.
	keepOldMask |= gstate.getColorMask();

	const u32 new_color = v1.color0.ToRGBA();
	u16 new_color16;
	switch (gstate.FrameBufFormat()) {
	case GE_FORMAT_565:
		new_color16 = RGBA8888ToRGB565(new_color);
		keepOldMask = keepOldMask == 0 ? 0 : (0xFFFF0000 | RGBA8888ToRGB565(keepOldMask));
		break;

	case GE_FORMAT_5551:
		new_color16 = RGBA8888ToRGBA5551(new_color);
		keepOldMask = keepOldMask == 0 ? 0 : (0xFFFF0000 | RGBA8888ToRGBA5551(keepOldMask));
		break;

	case GE_FORMAT_4444:
		new_color16 = RGBA8888ToRGBA4444(new_color);
		keepOldMask = keepOldMask == 0 ? 0 : (0xFFFF0000 | RGBA8888ToRGBA4444(keepOldMask));
		break;

	case GE_FORMAT_8888:
		break;

	case GE_FORMAT_INVALID:
	case GE_FORMAT_DEPTH16:
		_dbg_assert_msg_(false, "Software: invalid framebuf format.");
		break;
	}

	if (keepOldMask == 0) {
		const int stride = gstate.FrameBufStride();

		if (gstate.FrameBufFormat() == GE_FORMAT_8888) {
			const bool canMemsetColor = (new_color & 0xFF) == (new_color >> 8) && (new_color & 0xFFFF) == (new_color >> 16);
			if (canMemsetColor) {
				ParallelRangeLoop(&g_threadManager, [=](int y1, int y2) {
					DrawingCoords p = pprime;
					for (p.y = y1; p.y < y2; ++p.y) {
						u32 *row = fb.Get32Ptr(p.x, p.y, stride);
						memset(row, new_color, w * 4);
					}
				}, pprime.y, pend.y, MIN_LINES_PER_THREAD);
			} else {
				ParallelRangeLoop(&g_threadManager, [=](int y1, int y2) {
					DrawingCoords p = pprime;
					for (p.y = y1; p.y < y2; ++p.y) {
						for (int x = 0; x < w; ++x) {
							fb.Set32(p.x + x, p.y, stride, new_color);
						}
					}
				}, pprime.y, pend.y, MIN_LINES_PER_THREAD);
			}
		} else {
			const bool canMemsetColor = (new_color16 & 0xFF) == (new_color16 >> 8);
			if (canMemsetColor) {
				ParallelRangeLoop(&g_threadManager, [=](int y1, int y2) {
					DrawingCoords p = pprime;
					for (p.y = y1; p.y < y2; ++p.y) {
						u16 *row = fb.Get16Ptr(p.x, p.y, stride);
						memset(row, new_color16, w * 2);
					}
				}, pprime.y, pend.y, MIN_LINES_PER_THREAD);
			} else {
				ParallelRangeLoop(&g_threadManager, [=](int y1, int y2) {
					DrawingCoords p = pprime;
					for (p.y = y1; p.y < y2; ++p.y) {
						for (int x = 0; x < w; ++x) {
							fb.Set16(p.x + x, p.y, stride, new_color16);
						}
					}
				}, pprime.y, pend.y, MIN_LINES_PER_THREAD);
			}
		}
	} else if (keepOldMask != 0xFFFFFFFF) {
		const int stride = gstate.FrameBufStride();

		if (gstate.FrameBufFormat() == GE_FORMAT_8888) {
			ParallelRangeLoop(&g_threadManager, [=](int y1, int y2) {
				DrawingCoords p = pprime;
				for (p.y = y1; p.y < y2; ++p.y) {
					for (int x = 0; x < w; ++x) {
						const u32 old_color = fb.Get32(p.x + x, p.y, stride);
						const u32 c = (old_color & keepOldMask) | (new_color & ~keepOldMask);
						fb.Set32(p.x + x, p.y, stride, c);
					}
				}
			}, pprime.y, pend.y, MIN_LINES_PER_THREAD);
		} else {
			ParallelRangeLoop(&g_threadManager, [=](int y1, int y2) {
				DrawingCoords p = pprime;
				for (p.y = y1; p.y < y2; ++p.y) {
					for (int x = 0; x < w; ++x) {
						const u16 old_color = fb.Get16(p.x + x, p.y, stride);
						const u16 c = (old_color & keepOldMask) | (new_color16 & ~keepOldMask);
						fb.Set16(p.x + x, p.y, stride, c);
					}
				}
			}, pprime.y, pend.y, MIN_LINES_PER_THREAD);
		}
	}

#if defined(SOFTGPU_MEMORY_TAGGING_DETAILED) || defined(SOFTGPU_MEMORY_TAGGING_BASIC)
	if (keepOldMask != 0xFFFFFFFF) {
		uint32_t bpp = gstate.FrameBufFormat() == GE_FORMAT_8888 ? 4 : 2;
		std::string tag = StringFromFormat("DisplayListX_%08x", currentList.pc);
		for (int y = pprime.y; y < pend.y; ++y) {
			uint32_t row = gstate.getFrameBufAddress() + y * gstate.FrameBufStride() * bpp;
			NotifyMemInfo(MemBlockFlags::WRITE, row + pprime.x * bpp, w * bpp, tag.c_str(), tag.size());
		}
	}
#endif
}

void DrawLine(const VertexData &v0, const VertexData &v1)
{
	// TODO: Use a proper line drawing algorithm that handles fractional endpoints correctly.
	Vec3<int> a(v0.screenpos.x, v0.screenpos.y, v0.screenpos.z);
	Vec3<int> b(v1.screenpos.x, v1.screenpos.y, v0.screenpos.z);

	int dx = b.x - a.x;
	int dy = b.y - a.y;
	int dz = b.z - a.z;

	int steps;
	if (abs(dx) < abs(dy))
		steps = abs(dy) / 16;
	else
		steps = abs(dx) / 16;

	// Avoid going too far since we typically don't start at the pixel center.
	if (dx < 0 && dx >= -16)
		dx++;
	if (dy < 0 && dy >= -16)
		dy++;

	double xinc = (double)dx / steps;
	double yinc = (double)dy / steps;
	double zinc = (double)dz / steps;

	ScreenCoords scissorTL(TransformUnit::DrawingToScreen(DrawingCoords(gstate.getScissorX1(), gstate.getScissorY1(), 0)));
	ScreenCoords scissorBR(TransformUnit::DrawingToScreen(DrawingCoords(gstate.getScissorX2(), gstate.getScissorY2(), 0)));
	// Allow drawing within a pixel's center.
	scissorBR.x += 15;
	scissorBR.y += 15;

	PixelFuncID pixelID;
	ComputePixelFuncID(&pixelID);

	int texbufw[8] = {0};

	int maxTexLevel = gstate.getTextureMaxLevel();
	u8 *texptr[8] = {NULL};

	if (!gstate.isMipmapEnabled()) {
		// No mipmapping enabled
		maxTexLevel = 0;
	}

	if (gstate.isTextureMapEnabled() && !pixelID.clearMode) {
		GETextureFormat texfmt = gstate.getTextureFormat();
		for (int i = 0; i <= maxTexLevel; i++) {
			u32 texaddr = gstate.getTextureAddress(i);
			texbufw[i] = GetTextureBufw(i, texaddr, texfmt);
			texptr[i] = Memory::GetPointer(texaddr);
		}
	}

	Sampler::Funcs sampler = Sampler::GetFuncs();
	Rasterizer::SingleFunc drawPixel = Rasterizer::GetSingleFunc(pixelID);

#if defined(SOFTGPU_MEMORY_TAGGING_DETAILED) || defined(SOFTGPU_MEMORY_TAGGING_BASIC)
	DisplayList currentList{};
	if (gpuDebug)
		gpuDebug->GetCurrentDisplayList(currentList);
	std::string tag = StringFromFormat("DisplayListL_%08x", currentList.pc);
	std::string ztag = StringFromFormat("DisplayListLZ_%08x", currentList.pc);
#endif

	double x = a.x > b.x ? a.x - 1 : a.x;
	double y = a.y > b.y ? a.y - 1 : a.y;
	double z = a.z;
	const int steps1 = steps == 0 ? 1 : steps;
	for (int i = 0; i < steps; i++) {
		if (x >= scissorTL.x && y >= scissorTL.y && x <= scissorBR.x && y <= scissorBR.y) {
			// Interpolate between the two points.
			Vec4<int> prim_color;
			Vec3<int> sec_color;
			if (gstate.getShadeMode() == GE_SHADE_GOURAUD) {
				prim_color = (v0.color0 * (steps - i) + v1.color0 * i) / steps1;
				sec_color = (v0.color1 * (steps - i) + v1.color1 * i) / steps1;
			} else {
				prim_color = v1.color0;
				sec_color = v1.color1;
			}

			u8 fog = 255;
			if (gstate.isFogEnabled() && !pixelID.clearMode) {
				fog = ClampFogDepth((v0.fogdepth * (float)(steps - i) + v1.fogdepth * (float)i) / steps1);
			}

			if (gstate.isAntiAliasEnabled()) {
				// TODO: Clearmode?
				// TODO: Calculate.
				prim_color.a() = 0x7F;
			}

			if (gstate.isTextureMapEnabled() && !pixelID.clearMode) {
				float s, s1;
				float t, t1;
				if (gstate.isModeThrough()) {
					Vec2<float> tc = (v0.texturecoords * (float)(steps - i) + v1.texturecoords * (float)i) / steps1;
					Vec2<float> tc1 = (v0.texturecoords * (float)(steps - i - 1) + v1.texturecoords * (float)(i + 1)) / steps1;

					s = tc.s() * (1.0f / (float)gstate.getTextureWidth(0));
					s1 = tc1.s() * (1.0f / (float)gstate.getTextureWidth(0));
					t = tc.t() * (1.0f / (float)gstate.getTextureHeight(0));
					t1 = tc1.t() * (1.0f / (float)gstate.getTextureHeight(0));
				} else {
					// Texture coordinate interpolation must definitely be perspective-correct.
					GetTextureCoordinates(v0, v1, (float)(steps - i) / steps1, s, t);
					GetTextureCoordinates(v0, v1, (float)(steps - i - 1) / steps1, s1, t1);
				}

				// If inc is 0, force the delta to zero.
				float ds = xinc == 0.0 ? 0.0f : (s1 - s) * 16.0f * (1.0f / xinc);
				float dt = yinc == 0.0 ? 0.0f : (t1 - t) * 16.0f * (1.0f / yinc);

				int texLevel;
				int texLevelFrac;
				bool texBilinear;
				CalculateSamplingParams(ds, dt, maxTexLevel, texLevel, texLevelFrac, texBilinear);

				if (gstate.isAntiAliasEnabled()) {
					// TODO: This is a niave and wrong implementation.
					DrawingCoords p0 = TransformUnit::ScreenToDrawing(ScreenCoords((int)x, (int)y, (int)z));
					s = ((float)p0.x + xinc / 32.0f) / 512.0f;
					t = ((float)p0.y + yinc / 32.0f) / 512.0f;

					texBilinear = true;
				}

				PROFILE_THIS_SCOPE("sampler");
				prim_color = ApplyTexturingSingle(s, t, x, y, ToVec4IntArg(prim_color), texptr, texbufw, texLevel, texLevelFrac, texBilinear, sampler);
			}

			if (!pixelID.clearMode)
				prim_color += Vec4<int>(sec_color, 0);

			ScreenCoords pprime = ScreenCoords((int)x, (int)y, (int)z);

			PROFILE_THIS_SCOPE("draw_px");
			DrawingCoords p = TransformUnit::ScreenToDrawing(pprime);
			drawPixel(p.x, p.y, z, fog, ToVec4IntArg(prim_color), pixelID);

#if defined(SOFTGPU_MEMORY_TAGGING_DETAILED) || defined(SOFTGPU_MEMORY_TAGGING_BASIC)
			uint32_t bpp = gstate.FrameBufFormat() == GE_FORMAT_8888 ? 4 : 2;
			uint32_t row = gstate.getFrameBufAddress() + p.y * gstate.FrameBufStride() * bpp;
			NotifyMemInfo(MemBlockFlags::WRITE, row + p.x * bpp, bpp, tag.c_str(), tag.size());

			if (pixelID.depthWrite) {
				uint32_t row = gstate.getDepthBufAddress() + y * gstate.DepthBufStride() * 2;
				NotifyMemInfo(MemBlockFlags::WRITE, row + p.x * 2, 2, ztag.c_str(), ztag.size());
			}
#endif
		}

		x += xinc;
		y += yinc;
		z += zinc;
	}
}

bool GetCurrentStencilbuffer(GPUDebugBuffer &buffer)
{
	int w = gstate.getRegionX2() - gstate.getRegionX1() + 1;
	int h = gstate.getRegionY2() - gstate.getRegionY1() + 1;
	buffer.Allocate(w, h, GPU_DBG_FORMAT_8BIT);

	u8 *row = buffer.GetData();
	for (int y = gstate.getRegionY1(); y <= gstate.getRegionY2(); ++y) {
		for (int x = gstate.getRegionX1(); x <= gstate.getRegionX2(); ++x) {
			row[x - gstate.getRegionX1()] = GetPixelStencil(gstate.FrameBufFormat(), x, y);
		}
		row += w;
	}
	return true;
}

bool GetCurrentTexture(GPUDebugBuffer &buffer, int level)
{
	if (!gstate.isTextureMapEnabled()) {
		return false;
	}

	GETextureFormat texfmt = gstate.getTextureFormat();
	u32 texaddr = gstate.getTextureAddress(level);
	int texbufw = GetTextureBufw(level, texaddr, texfmt);
	int w = gstate.getTextureWidth(level);
	int h = gstate.getTextureHeight(level);

	if (!texaddr || !Memory::IsValidRange(texaddr, (textureBitsPerPixel[texfmt] * texbufw * h) / 8))
		return false;

	buffer.Allocate(w, h, GE_FORMAT_8888, false);

	SamplerID id;
	ComputeSamplerID(&id);
	Sampler::FetchFunc sampler = Sampler::GetFetchFunc(id);

	u8 *texptr = Memory::GetPointer(texaddr);
	u32 *row = (u32 *)buffer.GetData();
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			row[x] = Vec4<int>(sampler(x, y, texptr, texbufw, level)).ToRGBA();
		}
		row += w;
	}
	return true;
}

} // namespace
