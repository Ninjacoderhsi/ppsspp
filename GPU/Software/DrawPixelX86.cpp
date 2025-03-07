// Copyright (c) 2017- PPSSPP Project.

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
#if PPSSPP_ARCH(AMD64)

#include <emmintrin.h>
#include "Common/x64Emitter.h"
#include "Common/CPUDetect.h"
#include "Core/Reporting.h"
#include "GPU/GPUState.h"
#include "GPU/Software/DrawPixel.h"
#include "GPU/Software/SoftGpu.h"
#include "GPU/ge_constants.h"

using namespace Gen;

namespace Rasterizer {

// This one is the const base.  Also a set of 255s.
alignas(16) static const uint16_t const255_16s[8] = { 255, 255, 255, 255, 255, 255, 255, 255 };
// This is used for a multiply that divides by 255 with shifting.
alignas(16) static const uint16_t by255i[8] = { 0x8081, 0x8081, 0x8081, 0x8081, 0x8081, 0x8081, 0x8081, 0x8081 };
// This is used to add a fixed point 0.5 (as s.11.4) for blend factors to multiply accurately.
alignas(16) static const uint16_t blendHalf_11_4s[8] = { 8, 8, 8, 8, 8, 8, 8, 8 };
// This is used for shifted blend factors, to inverse them.
alignas(16) static const uint16_t blendInvert_11_4s[8] = { 255 << 4, 255 << 4, 255 << 4, 255 << 4, 255 << 4, 255 << 4, 255 << 4, 255 << 4 };

template <typename T>
static bool Accessible(const T *t1, const T *t2) {
	ptrdiff_t diff = (const uint8_t *)t1 - (const uint8_t *)t2;
	return diff > -0x7FFFFFE0 && diff < 0x7FFFFFE0;
}

template <typename T>
static OpArg MAccessibleDisp(X64Reg r, const T *tbase, const T *t) {
	_assert_(Accessible(tbase, t));
	ptrdiff_t diff = (const uint8_t *)t - (const uint8_t *)tbase;
	return MDisp(r, (int)diff);
}

template <typename T>
static bool ConstAccessible(const T *t) {
	return Accessible((const uint8_t *)&const255_16s[0], (const uint8_t *)t);
}

template <typename T>
static OpArg MConstDisp(X64Reg r, const T *t) {
	return MAccessibleDisp(r, (const uint8_t *)&const255_16s[0], (const uint8_t *)t);
}

SingleFunc PixelJitCache::CompileSingle(const PixelFuncID &id) {
	// Setup the reg cache and disallow spill for arguments.
	regCache_.SetupABI({
		RegCache::GEN_ARG_X,
		RegCache::GEN_ARG_Y,
		RegCache::GEN_ARG_Z,
		RegCache::GEN_ARG_FOG,
		RegCache::VEC_ARG_COLOR,
		RegCache::GEN_ARG_ID,
	});

#if PPSSPP_PLATFORM(WINDOWS)
	// Windows reserves space to save args, 1 xmm + 4 ints before the id.
	_assert_(!regCache_.Has(RegCache::GEN_ARG_ID));
	stackIDOffset_ = 1 * 16 + 4 * PTRBITS / 8;
#else
	_assert_(regCache_.Has(RegCache::GEN_ARG_ID));
	stackIDOffset_ = -1;
#endif

	BeginWrite();
	Describe("Init");
	const u8 *start = AlignCode16();
	bool success = true;

	// Start with the depth range.
	success = success && Jit_ApplyDepthRange(id);

	// Next, let's clamp the color (might affect alpha test, and everything expects it clamped.)
	// We simply convert to 4x8-bit to clamp.  Everything else expects color in this format.
	Describe("ClampColor");
	X64Reg argColorReg = regCache_.Find(RegCache::VEC_ARG_COLOR);
	PACKSSDW(argColorReg, R(argColorReg));
	PACKUSWB(argColorReg, R(argColorReg));
	regCache_.Unlock(argColorReg, RegCache::VEC_ARG_COLOR);
	colorIs16Bit_ = false;

	success = success && Jit_AlphaTest(id);
	// Fog is applied prior to color test.  Maybe before alpha test too, but it doesn't affect it...
	success = success && Jit_ApplyFog(id);
	success = success && Jit_ColorTest(id);

	if (id.stencilTest && !id.clearMode)
		success = success && Jit_StencilAndDepthTest(id);
	else if (!id.clearMode)
		success = success && Jit_DepthTest(id);
	success = success && Jit_WriteDepth(id);

	success = success && Jit_AlphaBlend(id);
	success = success && Jit_Dither(id);
	success = success && Jit_WriteColor(id);

	for (auto &fixup : discards_) {
		SetJumpTarget(fixup);
	}
	discards_.clear();

	if (regCache_.Has(RegCache::GEN_ARG_ID))
		regCache_.ForceRelease(RegCache::GEN_ARG_ID);
	regCache_.Reset(success);

	if (!success) {
		ERROR_LOG_REPORT(G3D, "Could not compile pixel func: %s", DescribePixelFuncID(id).c_str());

		EndWrite();
		ResetCodePtr(GetOffset(start));
		return nullptr;
	}

	RET();

	EndWrite();
	return (SingleFunc)start;
}

RegCache::Reg PixelJitCache::GetGState() {
	if (!regCache_.Has(RegCache::GEN_GSTATE)) {
		X64Reg r = regCache_.Alloc(RegCache::GEN_GSTATE);
		MOV(PTRBITS, R(r), ImmPtr(&gstate.nop));
		return r;
	}
	return regCache_.Find(RegCache::GEN_GSTATE);
}

RegCache::Reg PixelJitCache::GetConstBase() {
	if (!regCache_.Has(RegCache::GEN_CONST_BASE)) {
		X64Reg r = regCache_.Alloc(RegCache::GEN_CONST_BASE);
		MOV(PTRBITS, R(r), ImmPtr(&const255_16s[0]));
		return r;
	}
	return regCache_.Find(RegCache::GEN_CONST_BASE);
}

RegCache::Reg PixelJitCache::GetZeroVec() {
	if (!regCache_.Has(RegCache::VEC_ZERO)) {
		X64Reg r = regCache_.Alloc(RegCache::VEC_ZERO);
		PXOR(r, R(r));
		return r;
	}
	return regCache_.Find(RegCache::VEC_ZERO);
}

RegCache::Reg PixelJitCache::GetColorOff(const PixelFuncID &id) {
	if (!regCache_.Has(RegCache::GEN_COLOR_OFF)) {
		Describe("GetColorOff");
		if (id.useStandardStride && !id.dithering) {
			bool loadDepthOff = id.depthWrite || id.DepthTestFunc() != GE_COMP_ALWAYS;
			X64Reg depthTemp = INVALID_REG;
			X64Reg argYReg = regCache_.Find(RegCache::GEN_ARG_Y);
			X64Reg argXReg = regCache_.Find(RegCache::GEN_ARG_X);

			// In this mode, we force argXReg to the off, and throw away argYReg.
			SHL(32, R(argYReg), Imm8(9));
			ADD(32, R(argXReg), R(argYReg));

			// Now add the pointer for the color buffer.
			if (loadDepthOff) {
				_assert_(Accessible(&fb.data, &depthbuf.data));
				depthTemp = regCache_.Alloc(RegCache::GEN_DEPTH_OFF);
				if (RipAccessible(&fb.data) && RipAccessible(&depthbuf.data)) {
					MOV(PTRBITS, R(argYReg), M(&fb.data));
				} else {
					MOV(PTRBITS, R(depthTemp), ImmPtr(&fb.data));
					MOV(PTRBITS, R(argYReg), MatR(depthTemp));
				}
			} else {
				if (RipAccessible(&fb.data)) {
					MOV(PTRBITS, R(argYReg), M(&fb.data));
				} else {
					MOV(PTRBITS, R(argYReg), ImmPtr(&fb.data));
					MOV(PTRBITS, R(argYReg), MatR(argYReg));
				}
			}
			LEA(PTRBITS, argYReg, MComplex(argYReg, argXReg, id.FBFormat() == GE_FORMAT_8888 ? 4 : 2, 0));
			// With that, argYOff is now GEN_COLOR_OFF.
			regCache_.Unlock(argYReg, RegCache::GEN_ARG_Y);
			regCache_.Change(RegCache::GEN_ARG_Y, RegCache::GEN_COLOR_OFF);
			// Retain it, because we can't recalculate this.
			regCache_.ForceRetain(RegCache::GEN_COLOR_OFF);

			// Next, also calculate the depth offset, unless we won't need it at all.
			if (loadDepthOff) {
				if (RipAccessible(&fb.data) && RipAccessible(&depthbuf.data)) {
					MOV(PTRBITS, R(depthTemp), M(&depthbuf.data));
				} else {
					MOV(PTRBITS, R(depthTemp), MAccessibleDisp(depthTemp, &fb.data, &depthbuf.data));
				}
				LEA(PTRBITS, argXReg, MComplex(depthTemp, argXReg, 2, 0));
				regCache_.Release(depthTemp, RegCache::GEN_DEPTH_OFF);

				// Okay, same deal - release as GEN_DEPTH_OFF and force retain it.
				regCache_.Unlock(argXReg, RegCache::GEN_ARG_X);
				regCache_.Change(RegCache::GEN_ARG_X, RegCache::GEN_DEPTH_OFF);
				regCache_.ForceRetain(RegCache::GEN_DEPTH_OFF);
			} else {
				regCache_.Unlock(argXReg, RegCache::GEN_ARG_X);
				regCache_.ForceRelease(RegCache::GEN_ARG_X);
			}

			return regCache_.Find(RegCache::GEN_COLOR_OFF);
		}

		X64Reg argYReg = regCache_.Find(RegCache::GEN_ARG_Y);
		X64Reg r;
		if (id.useStandardStride) {
			r = regCache_.Alloc(RegCache::GEN_COLOR_OFF);
			MOV(32, R(r), R(argYReg));
			SHL(32, R(r), Imm8(9));
		} else {
			if (RipAccessible(&gstate.fbwidth)) {
				r = regCache_.Alloc(RegCache::GEN_COLOR_OFF);
				MOVZX(32, 16, r, M(&gstate.fbwidth));
			} else {
				X64Reg gstateReg = GetGState();
				r = regCache_.Alloc(RegCache::GEN_COLOR_OFF);
				MOVZX(32, 16, r, MDisp(gstateReg, offsetof(GPUgstate, fbwidth)));
				regCache_.Unlock(gstateReg, RegCache::GEN_GSTATE);
			}

			AND(16, R(r), Imm16(0x07FC));
			IMUL(32, r, R(argYReg));
		}
		regCache_.Unlock(argYReg, RegCache::GEN_ARG_Y);

		X64Reg argXReg = regCache_.Find(RegCache::GEN_ARG_X);
		ADD(32, R(r), R(argXReg));
		regCache_.Unlock(argXReg, RegCache::GEN_ARG_X);

		X64Reg temp = regCache_.Alloc(RegCache::GEN_TEMP_HELPER);
		if (RipAccessible(&fb.data)) {
			MOV(PTRBITS, R(temp), M(&fb.data));
		} else {
			MOV(PTRBITS, R(temp), ImmPtr(&fb.data));
			MOV(PTRBITS, R(temp), MatR(temp));
		}
		LEA(PTRBITS, r, MComplex(temp, r, id.FBFormat() == GE_FORMAT_8888 ? 4 : 2, 0));
		regCache_.Release(temp, RegCache::GEN_TEMP_HELPER);

		return r;
	}
	return regCache_.Find(RegCache::GEN_COLOR_OFF);
}

RegCache::Reg PixelJitCache::GetDepthOff(const PixelFuncID &id) {
	if (!regCache_.Has(RegCache::GEN_DEPTH_OFF)) {
		// If both color and depth use 512, the offsets are the same.
		if (id.useStandardStride && !id.dithering) {
			// Calculate once inside GetColorOff().
			X64Reg colorOffReg = GetColorOff(id);
			regCache_.Unlock(colorOffReg, RegCache::GEN_COLOR_OFF);
			return regCache_.Find(RegCache::GEN_DEPTH_OFF);
		}

		Describe("GetDepthOff");
		X64Reg argYReg = regCache_.Find(RegCache::GEN_ARG_Y);
		X64Reg r;
		if (id.useStandardStride) {
			r = regCache_.Alloc(RegCache::GEN_DEPTH_OFF);
			MOV(32, R(r), R(argYReg));
			SHL(32, R(r), Imm8(9));
		} else {
			if (RipAccessible(&gstate.zbwidth)) {
				r = regCache_.Alloc(RegCache::GEN_DEPTH_OFF);
				MOVZX(32, 16, r, M(&gstate.zbwidth));
			} else {
				X64Reg gstateReg = GetGState();
				r = regCache_.Alloc(RegCache::GEN_DEPTH_OFF);
				MOVZX(32, 16, r, MDisp(gstateReg, offsetof(GPUgstate, zbwidth)));
				regCache_.Unlock(gstateReg, RegCache::GEN_GSTATE);
			}

			AND(16, R(r), Imm16(0x07FC));
			IMUL(32, r, R(argYReg));
		}
		regCache_.Unlock(argYReg, RegCache::GEN_ARG_Y);

		X64Reg argXReg = regCache_.Find(RegCache::GEN_ARG_X);
		ADD(32, R(r), R(argXReg));
		regCache_.Unlock(argXReg, RegCache::GEN_ARG_X);

		X64Reg temp = regCache_.Alloc(RegCache::GEN_TEMP_HELPER);
		if (RipAccessible(&depthbuf.data)) {
			MOV(PTRBITS, R(temp), M(&depthbuf.data));
		} else {
			MOV(PTRBITS, R(temp), ImmPtr(&depthbuf.data));
			MOV(PTRBITS, R(temp), MatR(temp));
		}
		LEA(PTRBITS, r, MComplex(temp, r, 2, 0));
		regCache_.Release(temp, RegCache::GEN_TEMP_HELPER);

		return r;
	}
	return regCache_.Find(RegCache::GEN_DEPTH_OFF);
}


RegCache::Reg PixelJitCache::GetDestStencil(const PixelFuncID &id) {
	// Skip if 565, since stencil is fixed zero.
	if (id.FBFormat() == GE_FORMAT_565)
		return INVALID_REG;

	X64Reg colorOffReg = GetColorOff(id);
	Describe("GetDestStencil");
	X64Reg stencilReg = regCache_.Alloc(RegCache::GEN_STENCIL);
	if (id.FBFormat() == GE_FORMAT_8888) {
		MOVZX(32, 8, stencilReg, MDisp(colorOffReg, 3));
	} else if (id.FBFormat() == GE_FORMAT_5551) {
		MOVZX(32, 8, stencilReg, MDisp(colorOffReg, 1));
		SAR(8, R(stencilReg), Imm8(7));
	} else if (id.FBFormat() == GE_FORMAT_4444) {
		MOVZX(32, 8, stencilReg, MDisp(colorOffReg, 1));
		SHR(32, R(stencilReg), Imm8(4));
		X64Reg temp = regCache_.Alloc(RegCache::GEN_TEMP_HELPER);
		MOV(32, R(temp), R(stencilReg));
		SHL(32, R(temp), Imm8(4));
		OR(32, R(stencilReg), R(temp));
		regCache_.Release(temp, RegCache::GEN_TEMP_HELPER);
	}
	regCache_.Unlock(colorOffReg, RegCache::GEN_COLOR_OFF);

	return stencilReg;
}

void PixelJitCache::Discard() {
	discards_.push_back(J(true));
}

void PixelJitCache::Discard(Gen::CCFlags cc) {
	discards_.push_back(J_CC(cc, true));
}

bool PixelJitCache::Jit_ApplyDepthRange(const PixelFuncID &id) {
	if (id.applyDepthRange) {
		Describe("ApplyDepthR");
		X64Reg gstateReg = INVALID_REG;
		if (!RipAccessible(&gstate.minz) || !RipAccessible(&gstate.maxz))
			gstateReg = GetGState();
		X64Reg maxReg = regCache_.Alloc(RegCache::GEN_TEMP0);
		X64Reg argZReg = regCache_.Find(RegCache::GEN_ARG_Z);

		// For lower, we compare directly (we take care of the 32-bit case below.)
		if (RipAccessible(&gstate.minz))
			CMP(16, R(argZReg), M(&gstate.minz));
		else
			CMP(16, R(argZReg), MDisp(gstateReg, offsetof(GPUgstate, minz)));
		Discard(CC_B);

		// We load the low 16 bits, but compare all 32 of z.  Above handles < 0.
		if (RipAccessible(&gstate.maxz))
			MOVZX(32, 16, maxReg, M(&gstate.maxz));
		else
			MOVZX(32, 16, maxReg, MDisp(gstateReg, offsetof(GPUgstate, maxz)));
		CMP(32, R(argZReg), R(maxReg));
		Discard(CC_A);

		regCache_.Unlock(argZReg, RegCache::GEN_ARG_Z);
		if (gstateReg != INVALID_REG)
			regCache_.Unlock(gstateReg, RegCache::GEN_GSTATE);
		regCache_.Release(maxReg, RegCache::GEN_TEMP0);
	}

	// Since this is early on, try to free up the z reg if we don't need it anymore.
	if (id.clearMode && !id.DepthClear())
		regCache_.ForceRelease(RegCache::GEN_ARG_Z);
	else if (!id.clearMode && !id.depthWrite && id.DepthTestFunc() == GE_COMP_ALWAYS)
		regCache_.ForceRelease(RegCache::GEN_ARG_Z);

	return true;
}

bool PixelJitCache::Jit_AlphaTest(const PixelFuncID &id) {
	// Take care of ALWAYS/NEVER first.  ALWAYS is common, means disabled.
	Describe("AlphaTest");
	switch (id.AlphaTestFunc()) {
	case GE_COMP_NEVER:
		Discard();
		return true;

	case GE_COMP_ALWAYS:
		return true;

	default:
		break;
	}

	// Load alpha into its own general reg.
	X64Reg alphaReg;
	if (regCache_.Has(RegCache::GEN_SRC_ALPHA)) {
		alphaReg = regCache_.Find(RegCache::GEN_SRC_ALPHA);
	} else {
		alphaReg = regCache_.Alloc(RegCache::GEN_SRC_ALPHA);
		_assert_(!colorIs16Bit_);
		X64Reg argColorReg = regCache_.Find(RegCache::VEC_ARG_COLOR);
		MOVD_xmm(R(alphaReg), argColorReg);
		regCache_.Unlock(argColorReg, RegCache::VEC_ARG_COLOR);
		SHR(32, R(alphaReg), Imm8(24));
	}

	if (id.hasAlphaTestMask) {
		// Unfortunate, we'll need gstate to load the mask.
		// Note: we leave the ALPHA purpose untouched and free it, because later code may reuse.
		X64Reg gstateReg = GetGState();
		X64Reg maskedReg = regCache_.Alloc(RegCache::GEN_TEMP0);

		// The mask is >> 16, so we load + 2.
		MOVZX(32, 8, maskedReg, MDisp(gstateReg, offsetof(GPUgstate, alphatest) + 2));
		regCache_.Unlock(gstateReg, RegCache::GEN_GSTATE);
		AND(32, R(maskedReg), R(alphaReg));
		regCache_.Unlock(alphaReg, RegCache::GEN_SRC_ALPHA);

		// Okay now do the rest using the masked reg, which we modified.
		alphaReg = maskedReg;
	}

	// We hardcode the ref into this jit func.
	CMP(8, R(alphaReg), Imm8(id.alphaTestRef));
	if (id.hasAlphaTestMask)
		regCache_.Release(alphaReg, RegCache::GEN_TEMP0);
	else
		regCache_.Unlock(alphaReg, RegCache::GEN_SRC_ALPHA);

	switch (id.AlphaTestFunc()) {
	case GE_COMP_NEVER:
	case GE_COMP_ALWAYS:
		break;

	case GE_COMP_EQUAL:
		Discard(CC_NE);
		break;

	case GE_COMP_NOTEQUAL:
		Discard(CC_E);
		break;

	case GE_COMP_LESS:
		Discard(CC_AE);
		break;

	case GE_COMP_LEQUAL:
		Discard(CC_A);
		break;

	case GE_COMP_GREATER:
		Discard(CC_BE);
		break;

	case GE_COMP_GEQUAL:
		Discard(CC_B);
		break;
	}

	return true;
}

bool PixelJitCache::Jit_ColorTest(const PixelFuncID &id) {
	if (!id.colorTest || id.clearMode)
		return true;

	// We'll have 4 with fog released, so we're using them all...
	Describe("ColorTest");
	X64Reg gstateReg = GetGState();
	X64Reg funcReg = regCache_.Alloc(RegCache::GEN_TEMP0);
	X64Reg maskReg = regCache_.Alloc(RegCache::GEN_TEMP1);
	X64Reg refReg = regCache_.Alloc(RegCache::GEN_TEMP2);

	// First, load the registers: mask and ref.
	MOV(32, R(maskReg), MDisp(gstateReg, offsetof(GPUgstate, colortestmask)));
	AND(32, R(maskReg), Imm32(0x00FFFFFF));
	MOV(32, R(refReg), MDisp(gstateReg, offsetof(GPUgstate, colorref)));
	AND(32, R(refReg), R(maskReg));

	X64Reg argColorReg = regCache_.Find(RegCache::VEC_ARG_COLOR);
	if (colorIs16Bit_) {
		// If it's expanded, we need to clamp anyway if it was fogged.
		PACKUSWB(argColorReg, R(argColorReg));
		colorIs16Bit_ = false;
	}

	// Temporarily abuse funcReg to grab the color into maskReg.
	MOVD_xmm(R(funcReg), argColorReg);
	AND(32, R(maskReg), R(funcReg));
	regCache_.Unlock(argColorReg, RegCache::VEC_ARG_COLOR);

	// Now that we're setup, get the func and follow it.
	MOVZX(32, 8, funcReg, MDisp(gstateReg, offsetof(GPUgstate, colortest)));
	AND(8, R(funcReg), Imm8(3));
	regCache_.Unlock(gstateReg, RegCache::GEN_GSTATE);

	CMP(8, R(funcReg), Imm8(GE_COMP_ALWAYS));
	// Discard for GE_COMP_NEVER...
	Discard(CC_B);
	FixupBranch skip = J_CC(CC_E);

	CMP(8, R(funcReg), Imm8(GE_COMP_EQUAL));
	FixupBranch doEqual = J_CC(CC_E);
	regCache_.Release(funcReg, RegCache::GEN_TEMP0);

	// The not equal path here... if they are equal, we discard.
	CMP(32, R(refReg), R(maskReg));
	Discard(CC_E);
	FixupBranch skip2 = J();

	SetJumpTarget(doEqual);
	CMP(32, R(refReg), R(maskReg));
	Discard(CC_NE);

	regCache_.Release(maskReg, RegCache::GEN_TEMP1);
	regCache_.Release(refReg, RegCache::GEN_TEMP2);

	SetJumpTarget(skip);
	SetJumpTarget(skip2);

	return true;
}

bool PixelJitCache::Jit_ApplyFog(const PixelFuncID &id) {
	if (!id.applyFog) {
		// Okay, anyone can use the fog register then.
		regCache_.ForceRelease(RegCache::GEN_ARG_FOG);
		return true;
	}

	// Load fog and expand to 16 bit.  Ignore the high 8 bits, which'll match up with A.
	Describe("ApplyFog");
	X64Reg fogColorReg = regCache_.Alloc(RegCache::VEC_TEMP1);
	X64Reg gstateReg = GetGState();
	if (cpu_info.bSSE4_1) {
		// This actually loads the texlodslope too, but that's okay.
		PMOVZXBW(fogColorReg, MDisp(gstateReg, offsetof(GPUgstate, fogcolor)));
	} else {
		X64Reg zeroReg = GetZeroVec();
		MOVD_xmm(fogColorReg, MDisp(gstateReg, offsetof(GPUgstate, fogcolor)));
		PUNPCKLBW(fogColorReg, R(zeroReg));
		regCache_.Unlock(zeroReg, RegCache::VEC_ZERO);
	}
	regCache_.Unlock(gstateReg, RegCache::GEN_GSTATE);

	// Load a set of 255s at 16 bit into a reg for later...
	X64Reg invertReg = regCache_.Alloc(RegCache::VEC_TEMP2);
	if (RipAccessible(&const255_16s[0])) {
		MOVDQA(invertReg, M(&const255_16s[0]));
	} else {
		X64Reg constReg = GetConstBase();
		MOVDQA(invertReg, MConstDisp(constReg, &const255_16s[0]));
		regCache_.Unlock(constReg, RegCache::GEN_CONST_BASE);
	}

	// Expand (we clamped) color to 16 bit as well, so we can multiply with fog.
	X64Reg argColorReg = regCache_.Find(RegCache::VEC_ARG_COLOR);
	if (!colorIs16Bit_) {
		if (cpu_info.bSSE4_1) {
			PMOVZXBW(argColorReg, R(argColorReg));
		} else {
			X64Reg zeroReg = GetZeroVec();
			PUNPCKLBW(argColorReg, R(zeroReg));
			regCache_.Unlock(zeroReg, RegCache::VEC_ZERO);
		}
		colorIs16Bit_ = true;
	}

	// Save A so we can put it back, we don't "fog" A.
	X64Reg alphaReg;
	if (regCache_.Has(RegCache::GEN_SRC_ALPHA)) {
		alphaReg = regCache_.Find(RegCache::GEN_SRC_ALPHA);
	} else {
		alphaReg = regCache_.Alloc(RegCache::GEN_SRC_ALPHA);
		PEXTRW(alphaReg, argColorReg, 3);
	}

	// Okay, let's broadcast fog to an XMM.
	X64Reg fogMultReg = regCache_.Alloc(RegCache::VEC_TEMP3);
	X64Reg argFogReg = regCache_.Find(RegCache::GEN_ARG_FOG);
	MOVD_xmm(fogMultReg, R(argFogReg));
	PSHUFLW(fogMultReg, R(fogMultReg), _MM_SHUFFLE(0, 0, 0, 0));
	regCache_.Unlock(argFogReg, RegCache::GEN_ARG_FOG);
	// We can free up the actual fog reg now.
	regCache_.ForceRelease(RegCache::GEN_ARG_FOG);

	// Now we multiply the existing color by fog...
	PMULLW(argColorReg, R(fogMultReg));
	// And then inverse the fog value using those 255s we loaded, and multiply by fog color.
	PSUBUSW(invertReg, R(fogMultReg));
	PMULLW(fogColorReg, R(invertReg));
	// At this point, argColorReg and fogColorReg are multiplied at 16-bit, so we need to sum.
	PADDUSW(argColorReg, R(fogColorReg));
	regCache_.Release(fogColorReg, RegCache::VEC_TEMP1);
	regCache_.Release(invertReg, RegCache::VEC_TEMP2);
	regCache_.Release(fogMultReg, RegCache::VEC_TEMP3);

	// Now to divide by 255, we use bit tricks: multiply by 0x8081, and shift right by 16+7.
	if (RipAccessible(&by255i[0])) {
		PMULHUW(argColorReg, M(&by255i[0]));
	} else {
		X64Reg constReg = GetConstBase();
		PMULHUW(argColorReg, MConstDisp(constReg, &by255i[0]));
		regCache_.Unlock(constReg, RegCache::GEN_CONST_BASE);
	}
	// Now shift right by 7 (PMULHUW already did 16 of the shift.)
	PSRLW(argColorReg, 7);

	// Okay, put A back in, we'll shrink it to 8888 when needed.
	PINSRW(argColorReg, R(alphaReg), 3);
	regCache_.Unlock(argColorReg, RegCache::VEC_ARG_COLOR);

	// We most likely won't use alphaReg again.
	regCache_.Unlock(alphaReg, RegCache::GEN_SRC_ALPHA);

	return true;
}

bool PixelJitCache::Jit_StencilAndDepthTest(const PixelFuncID &id) {
	_assert_(!id.clearMode && id.stencilTest);

	X64Reg stencilReg = GetDestStencil(id);
	Describe("StencilAndDepth");
	X64Reg maskedReg = stencilReg;
	if (id.hasStencilTestMask) {
		X64Reg gstateReg = GetGState();
		maskedReg = regCache_.Alloc(RegCache::GEN_TEMP0);
		MOV(32, R(maskedReg), R(stencilReg));
		AND(8, R(maskedReg), MDisp(gstateReg, offsetof(GPUgstate, stenciltest) + 2));
		regCache_.Unlock(gstateReg, RegCache::GEN_GSTATE);
	}

	bool success = true;
	success = success && Jit_StencilTest(id, stencilReg, maskedReg);
	if (maskedReg != stencilReg)
		regCache_.Release(maskedReg, RegCache::GEN_TEMP0);

	// Next up, the depth test.
	if (stencilReg == INVALID_REG) {
		// Just use the standard one, since we don't need to write stencil.
		// We also don't need to worry about cleanup either.
		return success && Jit_DepthTest(id);
	}

	success = success && Jit_DepthTestForStencil(id, stencilReg);
	success = success && Jit_ApplyStencilOp(id, id.ZPass(), stencilReg);

	// At this point, stencilReg can't be spilled.  It contains the updated value.
	regCache_.Unlock(stencilReg, RegCache::GEN_STENCIL);
	regCache_.ForceRetain(RegCache::GEN_STENCIL);

	return success;
}

bool PixelJitCache::Jit_StencilTest(const PixelFuncID &id, RegCache::Reg stencilReg, RegCache::Reg maskedReg) {
	Describe("StencilTest");

	bool hasFixedResult = false;
	bool fixedResult = false;
	FixupBranch toPass;
	if (stencilReg == INVALID_REG) {
		// This means stencil is a fixed value 0.
		hasFixedResult = true;
		switch (id.StencilTestFunc()) {
		case GE_COMP_NEVER: fixedResult = false; break;
		case GE_COMP_ALWAYS: fixedResult = true; break;
		case GE_COMP_EQUAL: fixedResult = id.stencilTestRef == 0; break;
		case GE_COMP_NOTEQUAL: fixedResult = id.stencilTestRef != 0; break;
		case GE_COMP_LESS: fixedResult = false; break;
		case GE_COMP_LEQUAL: fixedResult = id.stencilTestRef == 0; break;
		case GE_COMP_GREATER: fixedResult = id.stencilTestRef != 0; break;
		case GE_COMP_GEQUAL: fixedResult = true; break;
		}
	} else if (id.StencilTestFunc() == GE_COMP_ALWAYS) {
		// Fairly common, skip the CMP.
		hasFixedResult = true;
		fixedResult = true;
	} else {
		// Reversed here because of the imm, so tests below are reversed.
		CMP(8, R(maskedReg), Imm8(id.stencilTestRef));
		switch (id.StencilTestFunc()) {
		case GE_COMP_NEVER:
			hasFixedResult = true;
			fixedResult = false;
			break;

		case GE_COMP_ALWAYS:
			_assert_(false);
			break;

		case GE_COMP_EQUAL:
			toPass = J_CC(CC_E);
			break;

		case GE_COMP_NOTEQUAL:
			toPass = J_CC(CC_NE);
			break;

		case GE_COMP_LESS:
			toPass = J_CC(CC_A);
			break;

		case GE_COMP_LEQUAL:
			toPass = J_CC(CC_AE);
			break;

		case GE_COMP_GREATER:
			toPass = J_CC(CC_B);
			break;

		case GE_COMP_GEQUAL:
			toPass = J_CC(CC_BE);
			break;
		}
	}

	if (hasFixedResult && !fixedResult && stencilReg == INVALID_REG) {
		Discard();
		return true;
	}

	bool hadGStateReg = regCache_.Has(RegCache::GEN_GSTATE);
	bool hadColorOffReg = regCache_.Has(RegCache::GEN_COLOR_OFF);

	bool success = true;
	if (stencilReg != INVALID_REG && (!hasFixedResult || !fixedResult)) {
		// This is the fail path.
		success = success && Jit_ApplyStencilOp(id, id.SFail(), stencilReg);
		success = success && Jit_WriteStencilOnly(id, stencilReg);

		Discard();
	}

	// If we allocated either gstate or colorOff in the conditional, forget.
	if (!hadGStateReg && regCache_.Has(RegCache::GEN_GSTATE))
		regCache_.Change(RegCache::GEN_GSTATE, RegCache::GEN_INVALID);
	if (!hadColorOffReg && regCache_.Has(RegCache::GEN_COLOR_OFF))
		regCache_.Change(RegCache::GEN_COLOR_OFF, RegCache::GEN_INVALID);

	if (!hasFixedResult)
		SetJumpTarget(toPass);
	return success;
}

bool PixelJitCache::Jit_DepthTestForStencil(const PixelFuncID &id, RegCache::Reg stencilReg) {
	if (id.DepthTestFunc() == GE_COMP_ALWAYS)
		return true;

	X64Reg depthOffReg = GetDepthOff(id);
	Describe("DepthTestStencil");
	X64Reg argZReg = regCache_.Find(RegCache::GEN_ARG_Z);
	CMP(16, R(argZReg), MatR(depthOffReg));
	regCache_.Unlock(depthOffReg, RegCache::GEN_DEPTH_OFF);
	regCache_.Unlock(argZReg, RegCache::GEN_ARG_Z);

	// We discard the opposite of the passing test.
	FixupBranch skip;
	switch (id.DepthTestFunc()) {
	case GE_COMP_NEVER:
		// Shouldn't happen, just do an extra CMP.
		CMP(32, R(RAX), R(RAX));
		// This is just to have a skip that is valid.
		skip = J_CC(CC_NE);
		break;

	case GE_COMP_ALWAYS:
		// Shouldn't happen, just do an extra CMP.
		CMP(32, R(RAX), R(RAX));
		skip = J_CC(CC_E);
		break;

	case GE_COMP_EQUAL:
		skip = J_CC(CC_E);
		break;

	case GE_COMP_NOTEQUAL:
		skip = J_CC(CC_NE);
		break;

	case GE_COMP_LESS:
		skip = J_CC(CC_B);
		break;

	case GE_COMP_LEQUAL:
		skip = J_CC(CC_BE);
		break;

	case GE_COMP_GREATER:
		skip = J_CC(CC_A);
		break;

	case GE_COMP_GEQUAL:
		skip = J_CC(CC_AE);
		break;
	}

	bool hadGStateReg = regCache_.Has(RegCache::GEN_GSTATE);
	bool hadColorOffReg = regCache_.Has(RegCache::GEN_COLOR_OFF);

	bool success = true;
	success = success && Jit_ApplyStencilOp(id, id.ZFail(), stencilReg);
	success = success && Jit_WriteStencilOnly(id, stencilReg);
	Discard();

	// If we allocated either gstate or colorOff in the conditional, forget.
	if (!hadGStateReg && regCache_.Has(RegCache::GEN_GSTATE))
		regCache_.Change(RegCache::GEN_GSTATE, RegCache::GEN_INVALID);
	if (!hadColorOffReg && regCache_.Has(RegCache::GEN_COLOR_OFF))
		regCache_.Change(RegCache::GEN_COLOR_OFF, RegCache::GEN_INVALID);

	SetJumpTarget(skip);

	// Like in Jit_DepthTest(), at this point we may not need this reg anymore.
	if (!id.depthWrite)
		regCache_.ForceRelease(RegCache::GEN_ARG_Z);

	return success;
}

bool PixelJitCache::Jit_ApplyStencilOp(const PixelFuncID &id, GEStencilOp op, RegCache::Reg stencilReg) {
	_assert_(stencilReg != INVALID_REG);

	Describe("ApplyStencil");
	FixupBranch skip;
	switch (op) {
	case GE_STENCILOP_KEEP:
		// Nothing to do.
		break;

	case GE_STENCILOP_ZERO:
		XOR(32, R(stencilReg), R(stencilReg));
		break;

	case GE_STENCILOP_REPLACE:
		if (id.hasStencilTestMask) {
			// Load the unmasked value.
			X64Reg gstateReg = GetGState();
			MOVZX(32, 8, stencilReg, MDisp(gstateReg, offsetof(GPUgstate, stenciltest) + 1));
			regCache_.Unlock(gstateReg, RegCache::GEN_GSTATE);
		} else {
			MOV(8, R(stencilReg), Imm8(id.stencilTestRef));
		}
		break;

	case GE_STENCILOP_INVERT:
		NOT(8, R(stencilReg));
		break;

	case GE_STENCILOP_INCR:
		switch (id.fbFormat) {
		case GE_FORMAT_565:
			break;

		case GE_FORMAT_5551:
			MOV(8, R(stencilReg), Imm8(0xFF));
			break;

		case GE_FORMAT_4444:
			CMP(8, R(stencilReg), Imm8(0xF0));
			skip = J_CC(CC_AE);
			ADD(8, R(stencilReg), Imm8(0x11));
			SetJumpTarget(skip);
			break;

		case GE_FORMAT_8888:
			CMP(8, R(stencilReg), Imm8(0xFF));
			skip = J_CC(CC_E);
			ADD(8, R(stencilReg), Imm8(0x01));
			SetJumpTarget(skip);
			break;
		}
		break;

	case GE_STENCILOP_DECR:
		switch (id.fbFormat) {
		case GE_FORMAT_565:
			break;

		case GE_FORMAT_5551:
			XOR(32, R(stencilReg), R(stencilReg));
			break;

		case GE_FORMAT_4444:
			CMP(8, R(stencilReg), Imm8(0x11));
			skip = J_CC(CC_B);
			SUB(8, R(stencilReg), Imm8(0x11));
			SetJumpTarget(skip);
			break;

		case GE_FORMAT_8888:
			CMP(8, R(stencilReg), Imm8(0x00));
			skip = J_CC(CC_E);
			SUB(8, R(stencilReg), Imm8(0x01));
			SetJumpTarget(skip);
			break;
		}
		break;
	}

	return true;
}

bool PixelJitCache::Jit_WriteStencilOnly(const PixelFuncID &id, RegCache::Reg stencilReg) {
	_assert_(stencilReg != INVALID_REG);

	// It's okay to destroy stencilReg here, we know we're the last writing it.
	X64Reg colorOffReg = GetColorOff(id);
	Describe("WriteStencil");
	if (id.applyColorWriteMask) {
		X64Reg gstateReg = GetGState();
		X64Reg maskReg = regCache_.Alloc(RegCache::GEN_TEMP5);

		switch (id.fbFormat) {
		case GE_FORMAT_565:
			break;

		case GE_FORMAT_5551:
			MOVZX(32, 8, maskReg, MDisp(gstateReg, offsetof(GPUgstate, pmska)));
			OR(8, R(maskReg), Imm8(0x7F));

			// Poor man's BIC...
			NOT(32, R(stencilReg));
			OR(32, R(stencilReg), R(maskReg));
			NOT(32, R(stencilReg));

			AND(8, MDisp(colorOffReg, 1), R(maskReg));
			OR(8, MDisp(colorOffReg, 1), R(stencilReg));
			break;

		case GE_FORMAT_4444:
			MOVZX(32, 8, maskReg, MDisp(gstateReg, offsetof(GPUgstate, pmska)));
			OR(8, R(maskReg), Imm8(0x0F));

			// Poor man's BIC...
			NOT(32, R(stencilReg));
			OR(32, R(stencilReg), R(maskReg));
			NOT(32, R(stencilReg));

			AND(8, MDisp(colorOffReg, 1), R(maskReg));
			OR(8, MDisp(colorOffReg, 1), R(stencilReg));
			break;

		case GE_FORMAT_8888:
			MOVZX(32, 8, maskReg, MDisp(gstateReg, offsetof(GPUgstate, pmska)));

			// Poor man's BIC...
			NOT(32, R(stencilReg));
			OR(32, R(stencilReg), R(maskReg));
			NOT(32, R(stencilReg));

			AND(8, MDisp(colorOffReg, 3), R(maskReg));
			OR(8, MDisp(colorOffReg, 3), R(stencilReg));
			break;
		}

		regCache_.Release(maskReg, RegCache::GEN_TEMP5);
		regCache_.Unlock(gstateReg, RegCache::GEN_GSTATE);
	} else {
		switch (id.fbFormat) {
		case GE_FORMAT_565:
			break;

		case GE_FORMAT_5551:
			AND(8, R(stencilReg), Imm8(0x80));
			AND(8, MDisp(colorOffReg, 1), Imm8(0x7F));
			OR(8, MDisp(colorOffReg, 1), R(stencilReg));
			break;

		case GE_FORMAT_4444:
			AND(8, MDisp(colorOffReg, 1), Imm8(0x0F));
			AND(8, R(stencilReg), Imm8(0xF0));
			OR(8, MDisp(colorOffReg, 1), R(stencilReg));
			break;

		case GE_FORMAT_8888:
			MOV(8, MDisp(colorOffReg, 3), R(stencilReg));
			break;
		}
	}

	regCache_.Unlock(colorOffReg, RegCache::GEN_COLOR_OFF);
	return true;
}

bool PixelJitCache::Jit_DepthTest(const PixelFuncID &id) {
	if (id.DepthTestFunc() == GE_COMP_ALWAYS)
		return true;

	if (id.DepthTestFunc() == GE_COMP_NEVER) {
		Discard();
		// This should be uncommon, just keep going to have shared cleanup...
	}

	X64Reg depthOffReg = GetDepthOff(id);
	Describe("DepthTest");
	X64Reg argZReg = regCache_.Find(RegCache::GEN_ARG_Z);
	CMP(16, R(argZReg), MatR(depthOffReg));
	regCache_.Unlock(depthOffReg, RegCache::GEN_DEPTH_OFF);
	regCache_.Unlock(argZReg, RegCache::GEN_ARG_Z);

	// We discard the opposite of the passing test.
	switch (id.DepthTestFunc()) {
	case GE_COMP_NEVER:
	case GE_COMP_ALWAYS:
		break;

	case GE_COMP_EQUAL:
		Discard(CC_NE);
		break;

	case GE_COMP_NOTEQUAL:
		Discard(CC_E);
		break;

	case GE_COMP_LESS:
		Discard(CC_AE);
		break;

	case GE_COMP_LEQUAL:
		Discard(CC_A);
		break;

	case GE_COMP_GREATER:
		Discard(CC_BE);
		break;

	case GE_COMP_GEQUAL:
		Discard(CC_B);
		break;
	}

	// If we're not writing, we don't need Z anymore.  We'll free GEN_DEPTH_OFF in Jit_WriteDepth().
	if (!id.depthWrite)
		regCache_.ForceRelease(RegCache::GEN_ARG_Z);

	return true;
}

bool PixelJitCache::Jit_WriteDepth(const PixelFuncID &id) {
	// Clear mode shares depthWrite for DepthClear().
	if (id.depthWrite) {
		X64Reg depthOffReg = GetDepthOff(id);
		Describe("WriteDepth");
		X64Reg argZReg = regCache_.Find(RegCache::GEN_ARG_Z);
		MOV(16, MatR(depthOffReg), R(argZReg));
		regCache_.Unlock(depthOffReg, RegCache::GEN_DEPTH_OFF);
		regCache_.Unlock(argZReg, RegCache::GEN_ARG_Z);
		regCache_.ForceRelease(RegCache::GEN_ARG_Z);
	}

	// We can free up this reg if we force locked it.
	if (regCache_.Has(RegCache::GEN_DEPTH_OFF)) {
		regCache_.ForceRelease(RegCache::GEN_DEPTH_OFF);
	}

	return true;
}

bool PixelJitCache::Jit_AlphaBlend(const PixelFuncID &id) {
	if (!id.alphaBlend)
		return true;

	// Check if we need to load and prep factors.
	PixelBlendState blendState;
	ComputePixelBlendState(blendState, id);

	bool success = true;

	// Step 1: Load and expand dest color.
	X64Reg dstReg = regCache_.Alloc(RegCache::VEC_TEMP0);
	if (id.FBFormat() == GE_FORMAT_8888) {
		X64Reg colorOff = GetColorOff(id);
		Describe("AlphaBlend");
		MOVD_xmm(dstReg, MatR(colorOff));
		regCache_.Unlock(colorOff, RegCache::GEN_COLOR_OFF);
	} else {
		X64Reg colorOff = GetColorOff(id);
		Describe("AlphaBlend");
		X64Reg dstGenReg = regCache_.Alloc(RegCache::GEN_TEMP0);
		MOVZX(32, 16, dstGenReg, MatR(colorOff));
		regCache_.Unlock(colorOff, RegCache::GEN_COLOR_OFF);

		X64Reg temp1Reg = regCache_.Alloc(RegCache::GEN_TEMP1);
		X64Reg temp2Reg = regCache_.Alloc(RegCache::GEN_TEMP2);

		switch (id.fbFormat) {
		case GE_FORMAT_565:
			success = success && Jit_ConvertFrom565(id, dstGenReg, temp1Reg, temp2Reg);
			break;

		case GE_FORMAT_5551:
			success = success && Jit_ConvertFrom5551(id, dstGenReg, temp1Reg, temp2Reg, blendState.usesDstAlpha);
			break;

		case GE_FORMAT_4444:
			success = success && Jit_ConvertFrom4444(id, dstGenReg, temp1Reg, temp2Reg, blendState.usesDstAlpha);

			break;

		case GE_FORMAT_8888:
			break;
		}

		Describe("AlphaBlend");
		MOVD_xmm(dstReg, R(dstGenReg));

		regCache_.Release(dstGenReg, RegCache::GEN_TEMP0);
		regCache_.Release(temp1Reg, RegCache::GEN_TEMP1);
		regCache_.Release(temp2Reg, RegCache::GEN_TEMP2);
	}

	// Step 2: Load and apply factors.
	X64Reg argColorReg = regCache_.Find(RegCache::VEC_ARG_COLOR);
	if (blendState.usesFactors) {
		X64Reg srcFactorReg = regCache_.Alloc(RegCache::VEC_TEMP1);
		X64Reg dstFactorReg = regCache_.Alloc(RegCache::VEC_TEMP2);

		// We apply these at 16-bit, because they can be doubled and have a half offset.
		if (cpu_info.bSSE4_1) {
			if (!colorIs16Bit_)
				PMOVZXBW(argColorReg, R(argColorReg));
			PMOVZXBW(dstReg, R(dstReg));
		} else {
			X64Reg zeroReg = GetZeroVec();
			if (!colorIs16Bit_)
				PUNPCKLBW(argColorReg, R(zeroReg));
			PUNPCKLBW(dstReg, R(zeroReg));
			regCache_.Unlock(zeroReg, RegCache::VEC_ZERO);
		}
		colorIs16Bit_ = true;

		// Skip multiplying by factors if we can.
		bool multiplySrc = id.AlphaBlendSrc() != PixelBlendFactor::ZERO && id.AlphaBlendSrc() != PixelBlendFactor::ONE;
		bool multiplyDst = id.AlphaBlendDst() != PixelBlendFactor::ZERO && id.AlphaBlendDst() != PixelBlendFactor::ONE;
		// We also shift left by 4, so mulhi gives us a free shift
		// We also need to add a half bit later, so this gives us space.
		if (multiplySrc || blendState.srcColorAsFactor)
			PSLLW(argColorReg, 4);
		if (multiplyDst || blendState.dstColorAsFactor)
			PSLLW(dstReg, 4);

		// Okay, now grab our factors.  Don't bother if they're known values.
		if (id.AlphaBlendSrc() < PixelBlendFactor::ZERO)
			success = success && Jit_BlendFactor(id, srcFactorReg, dstReg, id.AlphaBlendSrc());
		if (id.AlphaBlendDst() < PixelBlendFactor::ZERO)
			success = success && Jit_DstBlendFactor(id, srcFactorReg, dstFactorReg, dstReg);

		X64Reg halfReg = INVALID_REG;
		if (multiplySrc || multiplyDst) {
			halfReg = regCache_.Alloc(RegCache::VEC_TEMP3);
			// We'll use this several times, so load into a reg.
			if (RipAccessible(&blendHalf_11_4s[0])) {
				MOVDQA(halfReg, M(&blendHalf_11_4s[0]));
			} else {
				X64Reg constReg = GetConstBase();
				MOVDQA(halfReg, MConstDisp(constReg, &blendHalf_11_4s[0]));
				regCache_.Unlock(constReg, RegCache::GEN_CONST_BASE);
			}
		}

		// Add in the half bit to the factors and color values, then multiply.
		// We take the high 16 bits to get a free right shift by 16.
		if (multiplySrc) {
			POR(srcFactorReg, R(halfReg));
			POR(argColorReg, R(halfReg));
			PMULHUW(argColorReg, R(srcFactorReg));
		} else if (id.AlphaBlendSrc() == PixelBlendFactor::ZERO) {
			PXOR(argColorReg, R(argColorReg));
		} else if (id.AlphaBlendSrc() == PixelBlendFactor::ONE) {
			if (blendState.srcColorAsFactor)
				PSRLW(argColorReg, 4);
		}

		if (multiplyDst) {
			POR(dstFactorReg, R(halfReg));
			POR(dstReg, R(halfReg));
			PMULHUW(dstReg, R(dstFactorReg));
		} else if (id.AlphaBlendDst() == PixelBlendFactor::ZERO) {
			// No need to add or subtract zero, unless we're negating.
			// This is common for bloom preparation.
			if (id.AlphaBlendEq() == GE_BLENDMODE_MUL_AND_SUBTRACT_REVERSE)
				PXOR(dstReg, R(dstReg));
		} else if (id.AlphaBlendDst() == PixelBlendFactor::ONE) {
			if (blendState.dstColorAsFactor)
				PSRLW(dstReg, 4);
		}

		regCache_.Release(srcFactorReg, RegCache::VEC_TEMP1);
		regCache_.Release(dstFactorReg, RegCache::VEC_TEMP2);
		if (halfReg != INVALID_REG)
			regCache_.Release(halfReg, RegCache::VEC_TEMP3);
	} else if (colorIs16Bit_) {
		// If it's expanded, shrink and clamp for our min/max/absdiff handling.
		PACKUSWB(argColorReg, R(argColorReg));
		colorIs16Bit_ = false;
	}

	// Step 3: Apply equation.
	// Note: below, we completely ignore what happens to the alpha bits.
	// It won't matter, since we'll replace those with stencil anyway.
	X64Reg tempReg = regCache_.Alloc(RegCache::VEC_TEMP1);
	switch (id.AlphaBlendEq()) {
	case GE_BLENDMODE_MUL_AND_ADD:
		if (id.AlphaBlendDst() != PixelBlendFactor::ZERO)
			PADDUSW(argColorReg, R(dstReg));
		break;

	case GE_BLENDMODE_MUL_AND_SUBTRACT:
		if (id.AlphaBlendDst() != PixelBlendFactor::ZERO)
			PSUBUSW(argColorReg, R(dstReg));
		break;

	case GE_BLENDMODE_MUL_AND_SUBTRACT_REVERSE:
		if (cpu_info.bAVX) {
			VPSUBUSW(128, argColorReg, dstReg, R(argColorReg));
		} else {
			MOVDQA(tempReg, R(argColorReg));
			MOVDQA(argColorReg, R(dstReg));
			PSUBUSW(argColorReg, R(tempReg));
		}
		break;

	case GE_BLENDMODE_MIN:
		PMINUB(argColorReg, R(dstReg));
		break;

	case GE_BLENDMODE_MAX:
		PMAXUB(argColorReg, R(dstReg));
		break;

	case GE_BLENDMODE_ABSDIFF:
		// Calculate A=(dst-src < 0 ? 0 : dst-src) and B=(src-dst < 0 ? 0 : src-dst)...
		MOVDQA(tempReg, R(dstReg));
		PSUBUSB(tempReg, R(argColorReg));
		PSUBUSB(argColorReg, R(dstReg));

		// Now, one of those must be zero, and the other one is the result (could also be zero.)
		POR(argColorReg, R(tempReg));
		break;
	}

	regCache_.Release(dstReg, RegCache::VEC_TEMP0);
	regCache_.Release(tempReg, RegCache::VEC_TEMP1);
	regCache_.Unlock(argColorReg, RegCache::VEC_ARG_COLOR);

	return success;
}


bool PixelJitCache::Jit_BlendFactor(const PixelFuncID &id, RegCache::Reg factorReg, RegCache::Reg dstReg, PixelBlendFactor factor) {
	X64Reg constReg = INVALID_REG;
	X64Reg gstateReg = INVALID_REG;
	X64Reg tempReg = INVALID_REG;
	X64Reg argColorReg = regCache_.Find(RegCache::VEC_ARG_COLOR);

	// Everything below expects an expanded 16-bit color
	_assert_(colorIs16Bit_);

	// Between source and dest factors, only DSTCOLOR, INVDSTCOLOR, and FIXA differ.
	// In those cases, it uses SRCCOLOR, INVSRCCOLOR, and FIXB respectively.

	// Load the invert constant first off, if needed.
	switch (factor) {
	case PixelBlendFactor::INVOTHERCOLOR:
	case PixelBlendFactor::INVSRCALPHA:
	case PixelBlendFactor::INVDSTALPHA:
	case PixelBlendFactor::DOUBLEINVSRCALPHA:
	case PixelBlendFactor::DOUBLEINVDSTALPHA:
		if (RipAccessible(&blendInvert_11_4s[0])) {
			MOVDQA(factorReg, M(&blendInvert_11_4s[0]));
		} else {
			constReg = GetConstBase();
			MOVDQA(factorReg, MConstDisp(constReg, &blendInvert_11_4s[0]));
			regCache_.Unlock(constReg, RegCache::GEN_CONST_BASE);
		}
		break;

	default:
		break;
	}

	switch (factor) {
	case PixelBlendFactor::OTHERCOLOR:
		MOVDQA(factorReg, R(dstReg));
		break;

	case PixelBlendFactor::INVOTHERCOLOR:
		PSUBUSW(factorReg, R(dstReg));
		break;

	case PixelBlendFactor::SRCALPHA:
		PSHUFLW(factorReg, R(argColorReg), _MM_SHUFFLE(3, 3, 3, 3));
		break;

	case PixelBlendFactor::INVSRCALPHA:
		tempReg = regCache_.Alloc(RegCache::VEC_TEMP3);

		PSHUFLW(tempReg, R(argColorReg), _MM_SHUFFLE(3, 3, 3, 3));
		PSUBUSW(factorReg, R(tempReg));
		break;

	case PixelBlendFactor::DSTALPHA:
		PSHUFLW(factorReg, R(dstReg), _MM_SHUFFLE(3, 3, 3, 3));
		break;

	case PixelBlendFactor::INVDSTALPHA:
		tempReg = regCache_.Alloc(RegCache::VEC_TEMP3);

		PSHUFLW(tempReg, R(dstReg), _MM_SHUFFLE(3, 3, 3, 3));
		PSUBUSW(factorReg, R(tempReg));
		break;

	case PixelBlendFactor::DOUBLESRCALPHA:
		PSHUFLW(factorReg, R(argColorReg), _MM_SHUFFLE(3, 3, 3, 3));
		PSLLW(factorReg, 1);
		break;

	case PixelBlendFactor::DOUBLEINVSRCALPHA:
		tempReg = regCache_.Alloc(RegCache::VEC_TEMP3);

		PSHUFLW(tempReg, R(argColorReg), _MM_SHUFFLE(3, 3, 3, 3));
		PSLLW(tempReg, 1);
		PSUBUSW(factorReg, R(tempReg));
		break;

	case PixelBlendFactor::DOUBLEDSTALPHA:
		PSHUFLW(factorReg, R(dstReg), _MM_SHUFFLE(3, 3, 3, 3));
		PSLLW(factorReg, 1);
		break;

	case PixelBlendFactor::DOUBLEINVDSTALPHA:
		tempReg = regCache_.Alloc(RegCache::VEC_TEMP3);

		PSHUFLW(tempReg, R(dstReg), _MM_SHUFFLE(3, 3, 3, 3));
		PSLLW(tempReg, 1);
		PSUBUSW(factorReg, R(tempReg));
		break;

	case PixelBlendFactor::ZERO:
		// Special value meaning zero.
		PXOR(factorReg, R(factorReg));
		break;

	case PixelBlendFactor::ONE:
		// Special value meaning all 255s.
		PCMPEQD(factorReg, R(factorReg));
		PSLLW(factorReg, 8);
		PSRLW(factorReg, 4);
		break;

	case PixelBlendFactor::FIX:
	default:
		gstateReg = GetGState();
		if (cpu_info.bSSE4_1) {
			PMOVZXBW(factorReg, MDisp(gstateReg, offsetof(GPUgstate, blendfixa)));
		} else {
			X64Reg zeroReg = GetZeroVec();
			MOVD_xmm(factorReg, MDisp(gstateReg, offsetof(GPUgstate, blendfixa)));
			PUNPCKLBW(factorReg, R(zeroReg));
			regCache_.Unlock(zeroReg, RegCache::VEC_ZERO);
		}
		// Round it out by shifting into place.
		PSLLW(factorReg, 4);
		break;
	}

	if (gstateReg != INVALID_REG)
		regCache_.Unlock(gstateReg, RegCache::GEN_GSTATE);
	if (tempReg != INVALID_REG)
		regCache_.Release(tempReg, RegCache::VEC_TEMP3);
	regCache_.Unlock(argColorReg, RegCache::VEC_ARG_COLOR);

	return true;
}

bool PixelJitCache::Jit_DstBlendFactor(const PixelFuncID &id, RegCache::Reg srcFactorReg, RegCache::Reg dstFactorReg, RegCache::Reg dstReg) {
	bool success = true;
	X64Reg constReg = INVALID_REG;
	X64Reg gstateReg = INVALID_REG;
	X64Reg argColorReg = regCache_.Find(RegCache::VEC_ARG_COLOR);

	// Everything below expects an expanded 16-bit color
	_assert_(colorIs16Bit_);

	PixelBlendState blendState;
	ComputePixelBlendState(blendState, id);

	// We might be able to reuse srcFactorReg for dst, in some cases.
	switch (id.AlphaBlendDst()) {
	case PixelBlendFactor::OTHERCOLOR:
		MOVDQA(dstFactorReg, R(argColorReg));
		break;

	case PixelBlendFactor::INVOTHERCOLOR:
		if (RipAccessible(&blendInvert_11_4s[0])) {
			MOVDQA(dstFactorReg, M(&blendInvert_11_4s[0]));
		} else {
			constReg = GetConstBase();
			MOVDQA(dstFactorReg, MConstDisp(constReg, &blendInvert_11_4s[0]));
		}
		PSUBUSW(dstFactorReg, R(argColorReg));
		break;

	case PixelBlendFactor::SRCALPHA:
	case PixelBlendFactor::INVSRCALPHA:
	case PixelBlendFactor::DSTALPHA:
	case PixelBlendFactor::INVDSTALPHA:
	case PixelBlendFactor::DOUBLESRCALPHA:
	case PixelBlendFactor::DOUBLEINVSRCALPHA:
	case PixelBlendFactor::DOUBLEDSTALPHA:
	case PixelBlendFactor::DOUBLEINVDSTALPHA:
	case PixelBlendFactor::ZERO:
	case PixelBlendFactor::ONE:
		// These are all equivalent for src factor, so reuse that logic.
		if (id.AlphaBlendSrc() == id.AlphaBlendDst()) {
			MOVDQA(dstFactorReg, R(srcFactorReg));
		} else if (blendState.dstFactorIsInverse) {
			if (RipAccessible(&blendInvert_11_4s[0])) {
				MOVDQA(dstFactorReg, M(&blendInvert_11_4s[0]));
			} else {
				constReg = GetConstBase();
				MOVDQA(dstFactorReg, MConstDisp(constReg, &blendInvert_11_4s[0]));
			}
			PSUBUSW(dstFactorReg, R(srcFactorReg));
		} else {
			success = success && Jit_BlendFactor(id, dstFactorReg, dstReg, id.AlphaBlendDst());
		}
		break;

	case PixelBlendFactor::FIX:
	default:
		gstateReg = GetGState();
		if (cpu_info.bSSE4_1) {
			PMOVZXBW(dstFactorReg, MDisp(gstateReg, offsetof(GPUgstate, blendfixb)));
		} else {
			X64Reg zeroReg = GetZeroVec();
			MOVD_xmm(dstFactorReg, MDisp(gstateReg, offsetof(GPUgstate, blendfixb)));
			PUNPCKLBW(dstFactorReg, R(zeroReg));
			regCache_.Unlock(zeroReg, RegCache::VEC_ZERO);
		}
		// Round it out by shifting into place.
		PSLLW(dstFactorReg, 4);
		break;
	}

	if (constReg != INVALID_REG)
		regCache_.Unlock(constReg, RegCache::GEN_CONST_BASE);
	if (gstateReg != INVALID_REG)
		regCache_.Unlock(gstateReg, RegCache::GEN_GSTATE);
	regCache_.Unlock(argColorReg, RegCache::VEC_ARG_COLOR);

	return success;
}

bool PixelJitCache::Jit_Dither(const PixelFuncID &id) {
	if (!id.dithering)
		return true;

	Describe("Dither");
#ifndef SOFTPIXEL_USE_CACHE
	X64Reg gstateReg = GetGState();
#endif
	X64Reg valueReg = regCache_.Alloc(RegCache::GEN_TEMP0);

	// Load the row dither matrix entry (will still need to get the X.)
	X64Reg argYReg = regCache_.Find(RegCache::GEN_ARG_Y);
	MOV(32, R(valueReg), R(argYReg));
	AND(32, R(valueReg), Imm8(3));
#ifndef SOFTPIXEL_USE_CACHE
	MOVZX(32, 16, valueReg, MComplex(gstateReg, valueReg, 4, offsetof(GPUgstate, dithmtx)));
	regCache_.Unlock(gstateReg, RegCache::GEN_GSTATE);
#endif

	// At this point, we're done with depth and y, so let's grab GEN_COLOR_OFF and retain it.
	// Then we can modify x and throw it away too, which is our actual goal.
	X64Reg colorOffReg = GetColorOff(id);
	Describe("Dither");
	regCache_.Unlock(colorOffReg, RegCache::GEN_COLOR_OFF);
	regCache_.ForceRetain(RegCache::GEN_COLOR_OFF);
	// And get rid of y, we can use for other regs.
	regCache_.Unlock(argYReg, RegCache::GEN_ARG_Y);
	regCache_.ForceRelease(RegCache::GEN_ARG_Y);

	X64Reg argXReg = regCache_.Find(RegCache::GEN_ARG_X);
	AND(32, R(argXReg), Imm32(3));

#ifndef SOFTPIXEL_USE_CACHE
	SHL(32, R(argXReg), Imm8(2));

	// Conveniently, this is ECX on Windows, but otherwise we need to swap it.
	X64Reg shiftReg = INVALID_REG;
	if (argXReg != RCX) {
		bool needsSwap = false;
		// This will force release argXReg if swapped.
		regCache_.GrabReg(RCX, RegCache::GEN_TEMP1, needsSwap, argXReg, RegCache::GEN_ARG_X);
		shiftReg = RCX;

		if (needsSwap) {
			XCHG(PTRBITS, R(argXReg), R(RCX));
			if (valueReg == RCX)
				valueReg = argXReg;

			// At this point, argXReg is some other unknown reg... basically, it's released.
			argXReg = INVALID_REG;
		} else {
			// We'll unlock and force release argXReg later, but copy for now.
			MOV(32, R(RCX), R(argXReg));
		}
	}

	// Okay shift to the specific value to add.
	SHR(32, R(valueReg), R(CL));
	AND(16, R(valueReg), Imm16(0x000F));

	// Release RCX if we explicitly grabbed.
	if (shiftReg != INVALID_REG)
		regCache_.Release(shiftReg, RegCache::GEN_TEMP1);

	// Now we need to make 0-7 positive, 8-F negative.. so sign extend.
	SHL(32, R(valueReg), Imm8(4));
	MOVSX(32, 8, valueReg, R(valueReg));
	SAR(8, R(valueReg), Imm8(4));
#else
	// Sum up (x + y * 4) + ditherMatrix offset to valueReg.
	LEA(32, valueReg, MComplex(argXReg, valueReg, 4, offsetof(PixelFuncID, cached.ditherMatrix)));

	// Okay, now abuse argXReg to read the PixelFuncID pointer on the stack.
	if (regCache_.Has(RegCache::GEN_ARG_ID)) {
		X64Reg idReg = regCache_.Find(RegCache::GEN_ARG_ID);
		MOVSX(32, 8, valueReg, MRegSum(idReg, valueReg));
		regCache_.Unlock(idReg, RegCache::GEN_ARG_ID);
	} else {
		_assert_(stackIDOffset_ != -1);
		MOV(PTRBITS, R(argXReg), MDisp(RSP, stackIDOffset_));
		MOVSX(32, 8, valueReg, MRegSum(argXReg, valueReg));
	}
#endif
	if (argXReg != INVALID_REG) {
		regCache_.Unlock(argXReg, RegCache::GEN_ARG_X);
		regCache_.ForceRelease(RegCache::GEN_ARG_X);
	}

	// Copy that value into a vec to add to the color.
	X64Reg vecValueReg = regCache_.Alloc(RegCache::VEC_TEMP0);
	MOVD_xmm(vecValueReg, R(valueReg));
	regCache_.Release(valueReg, RegCache::GEN_TEMP0);

	// Now we want to broadcast RGB in 16-bit, but keep A as 0.
	// Luckily, we know that second lane (in 16-bit) is zero from valueReg's high 16 bits.
	// We use 16-bit because we need a signed add, but we also want to saturate.
	PSHUFLW(vecValueReg, R(vecValueReg), _MM_SHUFFLE(1, 0, 0, 0));

	// With that, now let's convert the color to 16 bit...
	X64Reg argColorReg = regCache_.Find(RegCache::VEC_ARG_COLOR);
	if (!colorIs16Bit_) {
		if (cpu_info.bSSE4_1) {
			PMOVZXBW(argColorReg, R(argColorReg));
		} else {
			X64Reg zeroReg = GetZeroVec();
			PUNPCKLBW(argColorReg, R(zeroReg));
			regCache_.Unlock(zeroReg, RegCache::VEC_ZERO);
		}
		colorIs16Bit_ = true;
	}
	// And simply add the dither values.
	PADDSW(argColorReg, R(vecValueReg));
	regCache_.Release(vecValueReg, RegCache::VEC_TEMP0);
	regCache_.Unlock(argColorReg, RegCache::VEC_ARG_COLOR);

	return true;
}

bool PixelJitCache::Jit_WriteColor(const PixelFuncID &id) {
	X64Reg colorOff = GetColorOff(id);
	Describe("WriteColor");
	if (regCache_.Has(RegCache::GEN_ARG_X)) {
		// We normally toss x and y during dithering or useStandardStride with no dithering.
		// Free up the regs now to get more reg space.
		regCache_.ForceRelease(RegCache::GEN_ARG_X);
		regCache_.ForceRelease(RegCache::GEN_ARG_Y);

		// But make sure we don't lose GEN_COLOR_OFF, we'll be lost without that now.
		regCache_.ForceRetain(RegCache::GEN_COLOR_OFF);
	}

	// Convert back to 8888 and clamp.
	X64Reg argColorReg = regCache_.Find(RegCache::VEC_ARG_COLOR);
	if (colorIs16Bit_) {
		PACKUSWB(argColorReg, R(argColorReg));
		colorIs16Bit_ = false;
	}

	if (id.clearMode) {
		bool drawingDone = false;
		if (!id.ColorClear() && !id.StencilClear())
			drawingDone = true;
		if (!id.ColorClear() && id.FBFormat() == GE_FORMAT_565)
			drawingDone = true;

		bool success = true;
		if (!id.ColorClear() && !drawingDone) {
			// Let's reuse Jit_WriteStencilOnly for this path.
			X64Reg alphaReg;
			if (regCache_.Has(RegCache::GEN_SRC_ALPHA)) {
				alphaReg = regCache_.Find(RegCache::GEN_SRC_ALPHA);
			} else {
				alphaReg = regCache_.Alloc(RegCache::GEN_SRC_ALPHA);
				MOVD_xmm(R(alphaReg), argColorReg);
				SHR(32, R(alphaReg), Imm8(24));
			}
			success = Jit_WriteStencilOnly(id, alphaReg);
			regCache_.Release(alphaReg, RegCache::GEN_SRC_ALPHA);

			drawingDone = true;
		}

		if (drawingDone) {
			regCache_.Unlock(argColorReg, RegCache::VEC_ARG_COLOR);
			regCache_.ForceRelease(RegCache::VEC_ARG_COLOR);
			regCache_.Unlock(colorOff, RegCache::GEN_COLOR_OFF);
			regCache_.ForceRelease(RegCache::GEN_COLOR_OFF);
			return success;
		}

		// In this case, we're clearing only color or only color and stencil.  Proceed.
	}

	X64Reg colorReg = regCache_.Alloc(RegCache::GEN_TEMP0);
	MOVD_xmm(R(colorReg), argColorReg);
	regCache_.Unlock(argColorReg, RegCache::VEC_ARG_COLOR);
	regCache_.ForceRelease(RegCache::VEC_ARG_COLOR);

	X64Reg stencilReg = INVALID_REG;
	if (regCache_.Has(RegCache::GEN_STENCIL))
		stencilReg = regCache_.Find(RegCache::GEN_STENCIL);

	X64Reg temp1Reg = regCache_.Alloc(RegCache::GEN_TEMP1);
	X64Reg temp2Reg = regCache_.Alloc(RegCache::GEN_TEMP2);
	bool convertAlpha = id.clearMode && id.StencilClear();
	bool writeAlpha = convertAlpha || stencilReg != INVALID_REG;
	uint32_t fixedKeepMask = 0x00000000;

	bool success = true;

	// Step 1: Load the color into colorReg.
	switch (id.fbFormat) {
	case GE_FORMAT_565:
		// In this case, stencil doesn't matter.
		success = success && Jit_ConvertTo565(id, colorReg, temp1Reg, temp2Reg);
		break;

	case GE_FORMAT_5551:
		success = success && Jit_ConvertTo5551(id, colorReg, temp1Reg, temp2Reg, convertAlpha);

		if (stencilReg != INVALID_REG) {
			// Truncate off the top bit of the stencil.
			SHR(32, R(stencilReg), Imm8(7));
			SHL(32, R(stencilReg), Imm8(15));
		} else if (!writeAlpha) {
			fixedKeepMask = 0x8000;
		}
		break;

	case GE_FORMAT_4444:
		success = success && Jit_ConvertTo4444(id, colorReg, temp1Reg, temp2Reg, convertAlpha);

		if (stencilReg != INVALID_REG) {
			// Truncate off the top bit of the stencil.
			SHR(32, R(stencilReg), Imm8(4));
			SHL(32, R(stencilReg), Imm8(12));
		} else if (!writeAlpha) {
			fixedKeepMask = 0xF000;
		}
		break;

	case GE_FORMAT_8888:
		if (stencilReg != INVALID_REG) {
			SHL(32, R(stencilReg), Imm8(24));
			// Clear out the alpha bits so we can fit the stencil.
			AND(32, R(colorReg), Imm32(0x00FFFFFF));
		} else if (!writeAlpha) {
			fixedKeepMask = 0xFF000000;
		}
		break;
	}

	// Step 2: Load write mask if needed.
	// Note that we apply the write mask at the destination bit depth.
	Describe("WriteColor");
	X64Reg maskReg = INVALID_REG;
	if (id.applyColorWriteMask) {
#ifndef SOFTPIXEL_USE_CACHE
		X64Reg gstateReg = GetGState();
		maskReg = regCache_.Alloc(RegCache::GEN_TEMP3);

		// Load the write mask, combine in the stencil/alpha mask bits.
		MOV(32, R(maskReg), MDisp(gstateReg, offsetof(GPUgstate, pmskc)));
		if (writeAlpha) {
			MOVZX(32, 8, temp2Reg, MDisp(gstateReg, offsetof(GPUgstate, pmska)));
			SHL(32, R(temp2Reg), Imm8(24));
			OR(32, R(maskReg), R(temp2Reg));
		}
		regCache_.Unlock(gstateReg, RegCache::GEN_GSTATE);

		// Switch the mask into the specified bit depth.  This is easier.
		switch (id.fbFormat) {
		case GE_FORMAT_565:
			success = success && Jit_ConvertTo565(id, maskReg, temp1Reg, temp2Reg);
			break;

		case GE_FORMAT_5551:
			success = success && Jit_ConvertTo5551(id, maskReg, temp1Reg, temp2Reg, writeAlpha);
			if (fixedKeepMask != 0)
				OR(16, R(maskReg), Imm16((uint16_t)fixedKeepMask));
			break;

		case GE_FORMAT_4444:
			success = success && Jit_ConvertTo4444(id, maskReg, temp1Reg, temp2Reg, writeAlpha);
			if (fixedKeepMask != 0)
				OR(16, R(maskReg), Imm16((uint16_t)fixedKeepMask));
			break;

		case GE_FORMAT_8888:
			if (fixedKeepMask != 0)
				OR(32, R(maskReg), Imm32(fixedKeepMask));
			break;
		}
#else
		maskReg = regCache_.Alloc(RegCache::GEN_TEMP3);
		// Load the pre-converted and combined write mask.
		if (regCache_.Has(RegCache::GEN_ARG_ID)) {
			X64Reg idReg = regCache_.Find(RegCache::GEN_ARG_ID);
			MOV(32, R(maskReg), MDisp(idReg, offsetof(PixelFuncID, cached.colorWriteMask)));
			regCache_.Unlock(idReg, RegCache::GEN_ARG_ID);
		} else {
			_assert_(stackIDOffset_ != -1);
			MOV(PTRBITS, R(maskReg), MDisp(RSP, stackIDOffset_));
			MOV(32, R(maskReg), MDisp(maskReg, offsetof(PixelFuncID, cached.colorWriteMask)));
		}
#endif
	}

	// We've run out of regs, let's live without temp2 from here on.
	regCache_.Release(temp2Reg, RegCache::GEN_TEMP2);

	// Step 3: Apply logic op, combine stencil.
	skipStandardWrites_.clear();
	if (id.applyLogicOp) {
		// Note: we combine stencil during logic op, because it's a bit complex to retain.
		success = success && Jit_ApplyLogicOp(id, colorReg, maskReg);
	} else if (stencilReg != INVALID_REG) {
		OR(32, R(colorReg), R(stencilReg));
	}

	// Step 4: Write and apply write mask.
	Describe("WriteColor");
	switch (id.fbFormat) {
	case GE_FORMAT_565:
	case GE_FORMAT_5551:
	case GE_FORMAT_4444:
		if (maskReg != INVALID_REG) {
			// Zero all other bits, then flip maskReg to clear the bits we're keeping in colorReg.
			AND(16, MatR(colorOff), R(maskReg));
			NOT(32, R(maskReg));
			AND(32, R(colorReg), R(maskReg));
			OR(16, MatR(colorOff), R(colorReg));
		} else if (fixedKeepMask == 0) {
			MOV(16, MatR(colorOff), R(colorReg));
		} else {
			// Clear the non-stencil bits and or in the color.
			AND(16, MatR(colorOff), Imm16((uint16_t)fixedKeepMask));
			OR(16, MatR(colorOff), R(colorReg));
		}
		break;

	case GE_FORMAT_8888:
		if (maskReg != INVALID_REG) {
			// Zero all other bits, then flip maskReg to clear the bits we're keeping in colorReg.
			AND(32, MatR(colorOff), R(maskReg));
			NOT(32, R(maskReg));
			AND(32, R(colorReg), R(maskReg));
			OR(32, MatR(colorOff), R(colorReg));
		} else if (fixedKeepMask == 0) {
			MOV(32, MatR(colorOff), R(colorReg));
		} else if (fixedKeepMask == 0xFF000000) {
			// We want to set 24 bits only, since we're not changing stencil.
			// For now, let's do two writes rather than reading in the old stencil.
			MOV(16, MatR(colorOff), R(colorReg));
			SHR(32, R(colorReg), Imm8(16));
			MOV(8, MDisp(colorOff, 2), R(colorReg));
		} else {
			AND(32, MatR(colorOff), Imm32(fixedKeepMask));
			OR(32, MatR(colorOff), R(colorReg));
		}
		break;
	}

	for (FixupBranch &fixup : skipStandardWrites_)
		SetJumpTarget(fixup);
	skipStandardWrites_.clear();

	regCache_.Unlock(colorOff, RegCache::GEN_COLOR_OFF);
	regCache_.ForceRelease(RegCache::GEN_COLOR_OFF);
	regCache_.Release(colorReg, RegCache::GEN_TEMP0);
	regCache_.Release(temp1Reg, RegCache::GEN_TEMP1);
	if (maskReg != INVALID_REG)
		regCache_.Release(maskReg, RegCache::GEN_TEMP3);
	if (stencilReg != INVALID_REG) {
		regCache_.Unlock(stencilReg, RegCache::GEN_STENCIL);
		regCache_.ForceRelease(RegCache::GEN_STENCIL);
	}

	return success;
}

bool PixelJitCache::Jit_ApplyLogicOp(const PixelFuncID &id, RegCache::Reg colorReg, RegCache::Reg maskReg) {
	Describe("LogicOp");
	X64Reg logicOpReg = INVALID_REG;
	if (RipAccessible(&gstate.lop)) {
		logicOpReg = regCache_.Alloc(RegCache::GEN_TEMP4);
		MOVZX(32, 8, logicOpReg, M(&gstate.lop));
	} else {
		X64Reg gstateReg = GetGState();
		logicOpReg = regCache_.Alloc(RegCache::GEN_TEMP4);
		MOVZX(32, 8, logicOpReg, MDisp(gstateReg, offsetof(GPUgstate, lop)));
		regCache_.Unlock(gstateReg, RegCache::GEN_GSTATE);
	}
	AND(8, R(logicOpReg), Imm8(0x0F));

	X64Reg stencilReg = INVALID_REG;
	if (regCache_.Has(RegCache::GEN_STENCIL))
		stencilReg = regCache_.Find(RegCache::GEN_STENCIL);

	// Should already be allocated.
	X64Reg colorOff = regCache_.Find(RegCache::GEN_COLOR_OFF);
	X64Reg temp1Reg = regCache_.Alloc(RegCache::GEN_TEMP5);

	// We'll use these in several cases, so prepare.
	int bits = id.fbFormat == GE_FORMAT_8888 ? 32 : 16;
	OpArg stencilMask, notStencilMask;
	switch (id.fbFormat) {
	case GE_FORMAT_565:
		stencilMask = Imm16(0);
		notStencilMask = Imm16(0xFFFF);
		break;
	case GE_FORMAT_5551:
		stencilMask = Imm16(0x8000);
		notStencilMask = Imm16(0x7FFF);
		break;
	case GE_FORMAT_4444:
		stencilMask = Imm16(0xF000);
		notStencilMask = Imm16(0x0FFF);
		break;
	case GE_FORMAT_8888:
		stencilMask = Imm32(0xFF000000);
		notStencilMask = Imm32(0x00FFFFFF);
		break;
	}

	std::vector<FixupBranch> finishes;
	FixupBranch skipTable = J(true);
	const u8 *tableValues[16]{};

	tableValues[GE_LOGIC_CLEAR] = GetCodePointer();
	if (stencilReg != INVALID_REG) {
		// If clearing and setting the stencil, that's easy - stencilReg has it.
		MOV(32, R(colorReg), R(stencilReg));
		finishes.push_back(J(true));
	} else if (maskReg != INVALID_REG) {
		// Just and out the unmasked bits (stencil already included in maskReg.)
		AND(bits, MatR(colorOff), R(maskReg));
		skipStandardWrites_.push_back(J(true));
	} else {
		// Otherwise, no mask, just AND the stencil bits to zero the rest.
		AND(bits, MatR(colorOff), stencilMask);
		skipStandardWrites_.push_back(J(true));
	}

	tableValues[GE_LOGIC_AND] = GetCodePointer();
	if (stencilReg != INVALID_REG && maskReg != INVALID_REG) {
		// Since we're ANDing, set the mask bits (AND will keep them as-is.)
		OR(32, R(colorReg), R(maskReg));
		OR(32, R(colorReg), R(stencilReg));

		// To apply stencil, we'll OR the stencil unmasked bits in memory, so our AND keeps them.
		NOT(32, R(maskReg));
		AND(bits, R(maskReg), stencilMask);
		OR(bits, MatR(colorOff), R(maskReg));
	} else if (stencilReg != INVALID_REG) {
		OR(32, R(colorReg), R(stencilReg));
		// No mask, so just or in the stencil bits so our AND can set any we want.
		OR(bits, MatR(colorOff), stencilMask);
	} else if (maskReg != INVALID_REG) {
		// Force in the mask (which includes all stencil bits) so both are kept as-is.
		OR(32, R(colorReg), R(maskReg));
	} else {
		// Force on the stencil bits so they AND and keep the existing value.
		if (stencilMask.GetImmValue() != 0)
			OR(bits, R(colorReg), stencilMask);
	}
	// Now the AND, which applies stencil and the logic op.
	AND(bits, MatR(colorOff), R(colorReg));
	skipStandardWrites_.push_back(J(true));

	tableValues[GE_LOGIC_AND_REVERSE] = GetCodePointer();
	// Reverse memory in a temp reg so we can apply the write mask easily.
	MOV(bits, R(temp1Reg), MatR(colorOff));
	NOT(32, R(temp1Reg));
	AND(32, R(colorReg), R(temp1Reg));
	// Now add in the stencil bits (must be zero before, since we used AND.)
	if (stencilReg != INVALID_REG) {
		OR(32, R(colorReg), R(stencilReg));
	}
	finishes.push_back(J(true));

	tableValues[GE_LOGIC_COPY] = GetCodePointer();
	// This is just a standard write, nothing complex.
	if (stencilReg != INVALID_REG) {
		OR(32, R(colorReg), R(stencilReg));
	}
	finishes.push_back(J(true));

	tableValues[GE_LOGIC_AND_INVERTED] = GetCodePointer();
	if (stencilReg != INVALID_REG) {
		// Set the stencil bits, so they're zero when we invert.
		OR(bits, R(colorReg), stencilMask);
		NOT(32, R(colorReg));
		OR(32, R(colorReg), R(stencilReg));

		if (maskReg != INVALID_REG) {
			// This way our AND will keep all those bits.
			OR(32, R(colorReg), R(maskReg));

			// To apply stencil, we'll OR the stencil unmasked bits in memory, so our AND keeps them.
			NOT(32, R(maskReg));
			AND(bits, R(maskReg), stencilMask);
			OR(bits, MatR(colorOff), R(maskReg));
		} else {
			// Force memory to take our stencil bits by ORing for the AND.
			OR(bits, MatR(colorOff), stencilMask);
		}
	} else if (maskReg != INVALID_REG) {
		NOT(32, R(colorReg));
		// This way our AND will keep all those bits.
		OR(32, R(colorReg), R(maskReg));
	} else {
		// Invert our color, but then add in stencil bits so the AND keeps them.
		NOT(32, R(colorReg));
		// We only do this for 8888 since the rest will have had 0 stencil bits (which turned to 1s.)
		if (id.FBFormat() == GE_FORMAT_8888)
			OR(bits, R(colorReg), stencilMask);
	}
	AND(bits, MatR(colorOff), R(colorReg));
	skipStandardWrites_.push_back(J(true));

	tableValues[GE_LOGIC_NOOP] = GetCodePointer();
	if (stencilReg != INVALID_REG && maskReg != INVALID_REG) {
		// Start by clearing masked bits from stencilReg.
		NOT(32, R(maskReg));
		AND(32, R(stencilReg), R(maskReg));
		NOT(32, R(maskReg));

		// Now mask out the stencil bits we're writing from memory.
		OR(bits, R(maskReg), notStencilMask);
		AND(bits, MatR(colorOff), R(maskReg));

		// Now set those remaining stencil bits.
		OR(bits, MatR(colorOff), R(stencilReg));
		skipStandardWrites_.push_back(J(true));
	} else if (stencilReg != INVALID_REG) {
		// Clear and set just the stencil bits.
		AND(bits, MatR(colorOff), notStencilMask);
		OR(bits, MatR(colorOff), R(stencilReg));
		skipStandardWrites_.push_back(J(true));
	} else {
		Discard();
	}

	tableValues[GE_LOGIC_XOR] = GetCodePointer();
	XOR(bits, R(colorReg), MatR(colorOff));
	if (stencilReg != INVALID_REG) {
		// Purge out the stencil bits from the XOR and copy ours in.
		AND(bits, R(colorReg), notStencilMask);
		OR(32, R(colorReg), R(stencilReg));
	} else if (maskReg == INVALID_REG && stencilMask.GetImmValue() != 0) {
		// XOR might've set some bits, and without a maskReg we won't clear them.
		AND(bits, R(colorReg), notStencilMask);
	}
	finishes.push_back(J(true));

	tableValues[GE_LOGIC_OR] = GetCodePointer();
	if (stencilReg != INVALID_REG && maskReg != INVALID_REG) {
		OR(32, R(colorReg), R(stencilReg));

		// Clear the bits we should be masking out.
		NOT(32, R(maskReg));
		AND(32, R(colorReg), R(maskReg));
		NOT(32, R(maskReg));

		// Clear all the unmasked stencil bits, so we can set our own.
		OR(bits, R(maskReg), notStencilMask);
		AND(bits, MatR(colorOff), R(maskReg));
	} else if (stencilReg != INVALID_REG) {
		OR(32, R(colorReg), R(stencilReg));
		// AND out the stencil bits so we set our own.
		AND(bits, MatR(colorOff), notStencilMask);
	} else if (maskReg != INVALID_REG) {
		// Clear the bits we should be masking out.
		NOT(32, R(maskReg));
		AND(32, R(colorReg), R(maskReg));
	} else if (id.FBFormat() == GE_FORMAT_8888) {
		// We only need to do this for 8888, the others already have 0 stencil.
		AND(bits, R(colorReg), notStencilMask);
	}
	// Now the OR, which applies stencil and the logic op itself.
	OR(bits, MatR(colorOff), R(colorReg));
	skipStandardWrites_.push_back(J(true));

	tableValues[GE_LOGIC_NOR] = GetCodePointer();
	OR(bits, R(colorReg), MatR(colorOff));
	NOT(32, R(colorReg));
	if (stencilReg != INVALID_REG) {
		AND(bits, R(colorReg), notStencilMask);
		OR(32, R(colorReg), R(stencilReg));
	} else if (maskReg == INVALID_REG && stencilMask.GetImmValue() != 0) {
		// We need to clear the stencil bits since the standard write logic assumes they're zero.
		AND(bits, R(colorReg), notStencilMask);
	}
	finishes.push_back(J(true));

	tableValues[GE_LOGIC_EQUIV] = GetCodePointer();
	XOR(bits, R(colorReg), MatR(colorOff));
	NOT(32, R(colorReg));
	if (stencilReg != INVALID_REG) {
		AND(bits, R(colorReg), notStencilMask);
		OR(32, R(colorReg), R(stencilReg));
	} else if (maskReg == INVALID_REG && stencilMask.GetImmValue() != 0) {
		// We need to clear the stencil bits since the standard write logic assumes they're zero.
		AND(bits, R(colorReg), notStencilMask);
	}
	finishes.push_back(J(true));

	tableValues[GE_LOGIC_INVERTED] = GetCodePointer();
	// We just toss our color entirely.
	MOV(bits, R(colorReg), MatR(colorOff));
	NOT(32, R(colorReg));
	if (stencilReg != INVALID_REG) {
		AND(bits, R(colorReg), notStencilMask);
		OR(32, R(colorReg), R(stencilReg));
	} else if (maskReg == INVALID_REG && stencilMask.GetImmValue() != 0) {
		// We need to clear the stencil bits since the standard write logic assumes they're zero.
		AND(bits, R(colorReg), notStencilMask);
	}
	finishes.push_back(J(true));

	tableValues[GE_LOGIC_OR_REVERSE] = GetCodePointer();
	// Reverse in a temp reg so we can mask properly.
	MOV(bits, R(temp1Reg), MatR(colorOff));
	NOT(32, R(temp1Reg));
	OR(32, R(colorReg), R(temp1Reg));
	if (stencilReg != INVALID_REG) {
		AND(bits, R(colorReg), notStencilMask);
		OR(32, R(colorReg), R(stencilReg));
	} else if (maskReg == INVALID_REG && stencilMask.GetImmValue() != 0) {
		// We need to clear the stencil bits since the standard write logic assumes they're zero.
		AND(bits, R(colorReg), notStencilMask);
	}
	finishes.push_back(J(true));

	tableValues[GE_LOGIC_COPY_INVERTED] = GetCodePointer();
	NOT(32, R(colorReg));
	if (stencilReg != INVALID_REG) {
		AND(bits, R(colorReg), notStencilMask);
		OR(32, R(colorReg), R(stencilReg));
	} else if (maskReg == INVALID_REG && stencilMask.GetImmValue() != 0) {
		// We need to clear the stencil bits since the standard write logic assumes they're zero.
		AND(bits, R(colorReg), notStencilMask);
	}
	finishes.push_back(J(true));

	tableValues[GE_LOGIC_OR_INVERTED] = GetCodePointer();
	NOT(32, R(colorReg));
	if (stencilReg != INVALID_REG && maskReg != INVALID_REG) {
		AND(bits, R(colorReg), notStencilMask);
		OR(32, R(colorReg), R(stencilReg));

		// Clear the bits we should be masking out.
		NOT(32, R(maskReg));
		AND(32, R(colorReg), R(maskReg));
		NOT(32, R(maskReg));

		// Clear all the unmasked stencil bits, so we can set our own.
		OR(bits, R(maskReg), notStencilMask);
		AND(bits, MatR(colorOff), R(maskReg));
	} else if (stencilReg != INVALID_REG) {
		AND(bits, R(colorReg), notStencilMask);
		OR(32, R(colorReg), R(stencilReg));
		// AND out the stencil bits so we set our own.
		AND(bits, MatR(colorOff), notStencilMask);
	} else if (maskReg != INVALID_REG) {
		// Clear the bits we should be masking out.
		NOT(32, R(maskReg));
		AND(32, R(colorReg), R(maskReg));
	} else if (id.FBFormat() == GE_FORMAT_8888) {
		// We only need to do this for 8888, the others already have 0 stencil.
		AND(bits, R(colorReg), notStencilMask);
	}
	OR(bits, MatR(colorOff), R(colorReg));
	skipStandardWrites_.push_back(J(true));

	tableValues[GE_LOGIC_NAND] = GetCodePointer();
	AND(bits, R(temp1Reg), MatR(colorOff));
	NOT(32, R(colorReg));
	if (stencilReg != INVALID_REG) {
		AND(bits, R(colorReg), notStencilMask);
		OR(32, R(colorReg), R(stencilReg));
	} else if (maskReg == INVALID_REG && stencilMask.GetImmValue() != 0) {
		// We need to clear the stencil bits since the standard write logic assumes they're zero.
		AND(bits, R(colorReg), notStencilMask);
	}
	finishes.push_back(J(true));

	tableValues[GE_LOGIC_SET] = GetCodePointer();
	if (stencilReg != INVALID_REG && maskReg != INVALID_REG) {
		OR(32, R(colorReg), R(stencilReg));
		OR(bits, R(colorReg), notStencilMask);
		finishes.push_back(J(true));
	} else if (stencilReg != INVALID_REG) {
		// Set bits directly in stencilReg, and then put in memory.
		OR(bits, R(stencilReg), notStencilMask);
		MOV(bits, MatR(colorOff), R(stencilReg));
		skipStandardWrites_.push_back(J(true));
	} else if (maskReg != INVALID_REG) {
		// OR in the bits we're allowed to write (won't be any stencil.)
		NOT(32, R(maskReg));
		OR(bits, MatR(colorOff), R(maskReg));
		skipStandardWrites_.push_back(J(true));
	} else {
		OR(bits, MatR(colorOff), notStencilMask);
		skipStandardWrites_.push_back(J(true));
	}

	const u8 *tablePtr = GetCodePointer();
	for (int i = 0; i < 16; ++i) {
		Write64((uintptr_t)tableValues[i]);
	}

	SetJumpTarget(skipTable);
	LEA(64, temp1Reg, M(tablePtr));
	JMPptr(MComplex(temp1Reg, logicOpReg, 8, 0));

	for (FixupBranch &fixup : finishes)
		SetJumpTarget(fixup);

	regCache_.Unlock(colorOff, RegCache::GEN_COLOR_OFF);
	regCache_.Release(logicOpReg, RegCache::GEN_TEMP4);
	regCache_.Release(temp1Reg, RegCache::GEN_TEMP5);
	if (stencilReg != INVALID_REG)
		regCache_.Unlock(stencilReg, RegCache::GEN_STENCIL);

	return true;
}

bool PixelJitCache::Jit_ConvertTo565(const PixelFuncID &id, RegCache::Reg colorReg, RegCache::Reg temp1Reg, RegCache::Reg temp2Reg) {
	Describe("ConvertTo565");
	// Assemble the 565 color, starting with R...
	MOV(32, R(temp1Reg), R(colorReg));
	SHR(32, R(temp1Reg), Imm8(3));
	AND(16, R(temp1Reg), Imm16(0x1F << 0));

	// For G, move right 5 (because the top 6 are offset by 10.)
	MOV(32, R(temp2Reg), R(colorReg));
	SHR(32, R(temp2Reg), Imm8(5));
	AND(16, R(temp2Reg), Imm16(0x3F << 5));
	OR(32, R(temp1Reg), R(temp2Reg));

	// And finally B, move right 8 (top 5 are offset by 19.)
	SHR(32, R(colorReg), Imm8(8));
	AND(16, R(colorReg), Imm16(0x1F << 11));
	OR(32, R(colorReg), R(temp1Reg));

	return true;
}

bool PixelJitCache::Jit_ConvertTo5551(const PixelFuncID &id, RegCache::Reg colorReg, RegCache::Reg temp1Reg, RegCache::Reg temp2Reg, bool keepAlpha) {
	Describe("ConvertTo5551");
	// This is R, pretty simple.
	MOV(32, R(temp1Reg), R(colorReg));
	SHR(32, R(temp1Reg), Imm8(3));
	AND(16, R(temp1Reg), Imm16(0x1F << 0));

	// G moves right 6, to match the top 5 at 11.
	MOV(32, R(temp2Reg), R(colorReg));
	SHR(32, R(temp2Reg), Imm8(6));
	AND(16, R(temp2Reg), Imm16(0x1F << 5));
	OR(32, R(temp1Reg), R(temp2Reg));

	if (keepAlpha) {
		// Grab A into tempReg2 before handling B.
		MOV(32, R(temp2Reg), R(colorReg));
		SHR(32, R(temp2Reg), Imm8(31));
		SHL(32, R(temp2Reg), Imm8(15));
	}

	// B moves right 9, to match the top 5 at 19.
	SHR(32, R(colorReg), Imm8(9));
	AND(16, R(colorReg), Imm16(0x1F << 10));
	OR(32, R(colorReg), R(temp1Reg));

	if (keepAlpha)
		OR(32, R(colorReg), R(temp2Reg));

	return true;
}

bool PixelJitCache::Jit_ConvertTo4444(const PixelFuncID &id, RegCache::Reg colorReg, RegCache::Reg temp1Reg, RegCache::Reg temp2Reg, bool keepAlpha) {
	Describe("ConvertTo4444");
	// Shift and mask out R.
	MOV(32, R(temp1Reg), R(colorReg));
	SHR(32, R(temp1Reg), Imm8(4));
	AND(16, R(temp1Reg), Imm16(0xF << 0));

	// Shift G into position and mask.
	MOV(32, R(temp2Reg), R(colorReg));
	SHR(32, R(temp2Reg), Imm8(8));
	AND(16, R(temp2Reg), Imm16(0xF << 4));
	OR(32, R(temp1Reg), R(temp2Reg));

	if (keepAlpha) {
		// Grab A into tempReg2 before handling B.
		MOV(32, R(temp2Reg), R(colorReg));
		SHR(32, R(temp2Reg), Imm8(28));
		SHL(32, R(temp2Reg), Imm8(12));
	}

	// B moves right 12, to match the top 4 at 20.
	SHR(32, R(colorReg), Imm8(12));
	AND(16, R(colorReg), Imm16(0xF << 8));
	OR(32, R(colorReg), R(temp1Reg));

	if (keepAlpha)
		OR(32, R(colorReg), R(temp2Reg));

	return true;
}

bool PixelJitCache::Jit_ConvertFrom565(const PixelFuncID &id, RegCache::Reg colorReg, RegCache::Reg temp1Reg, RegCache::Reg temp2Reg) {
	Describe("ConvertFrom565");
	// Filter out red only into temp1.
	MOV(32, R(temp1Reg), R(colorReg));
	AND(16, R(temp1Reg), Imm16(0x1F << 0));
	// Move it left to the top of the 8 bits.
	SHL(32, R(temp1Reg), Imm8(3));

	// Now we bring in blue, since it's also 5 like red.
	MOV(32, R(temp2Reg), R(colorReg));
	AND(16, R(temp2Reg), Imm16(0x1F << 11));
	// Shift blue into place, 8 left (at 19), and merge back to temp1.
	SHL(32, R(temp2Reg), Imm8(8));
	OR(32, R(temp1Reg), R(temp2Reg));

	// Make a copy back in temp2, and shift left 1 so we can swizzle together with G.
	OR(32, R(temp2Reg), R(temp1Reg));
	SHL(32, R(temp2Reg), Imm8(1));

	// We go to green last because it's the different one.  Put it in place.
	AND(16, R(colorReg), Imm16(0x3F << 5));
	SHL(32, R(colorReg), Imm8(5));
	// Combine with temp2 (for swizzling), then merge in temp1 (R+B pre-swizzle.)
	OR(32, R(temp2Reg), R(colorReg));
	OR(32, R(colorReg), R(temp1Reg));

	// Now shift and mask temp2 for swizzle.
	SHR(32, R(temp2Reg), Imm8(6));
	AND(32, R(temp2Reg), Imm32(0x00070307));
	// And then OR that in too.  We're done.
	OR(32, R(colorReg), R(temp2Reg));

	return true;
}

bool PixelJitCache::Jit_ConvertFrom5551(const PixelFuncID &id, RegCache::Reg colorReg, RegCache::Reg temp1Reg, RegCache::Reg temp2Reg, bool keepAlpha) {
	Describe("ConvertFrom5551");
	// Filter out red only into temp1.
	MOV(32, R(temp1Reg), R(colorReg));
	AND(16, R(temp1Reg), Imm16(0x1F << 0));
	// Move it left to the top of the 8 bits.
	SHL(32, R(temp1Reg), Imm8(3));

	// Add in green and shift into place (top bits.)
	MOV(32, R(temp2Reg), R(colorReg));
	AND(16, R(temp2Reg), Imm16(0x1F << 5));
	SHL(32, R(temp2Reg), Imm8(6));
	OR(32, R(temp1Reg), R(temp2Reg));

	if (keepAlpha) {
		// Now take blue and alpha together.
		AND(16, R(colorReg), Imm16(0x8000 | (0x1F << 10)));
		// We move all the way left, then sign extend right to expand alpha.
		SHL(32, R(colorReg), Imm8(16));
		SAR(32, R(colorReg), Imm8(7));
	} else {
		AND(16, R(colorReg), Imm16(0x1F << 10));
		SHL(32, R(colorReg), Imm8(9));
	}

	// Combine both together, we still need to swizzle.
	OR(32, R(colorReg), R(temp1Reg));
	OR(32, R(temp1Reg), R(colorReg));
	// Now for swizzle, we'll mask carefully to avoid overflow.
	SHR(32, R(temp1Reg), Imm8(5));
	AND(32, R(temp1Reg), Imm32(0x00070707));

	// Then finally merge in the swizzle bits.
	OR(32, R(colorReg), R(temp1Reg));
	return true;
}

bool PixelJitCache::Jit_ConvertFrom4444(const PixelFuncID &id, RegCache::Reg colorReg, RegCache::Reg temp1Reg, RegCache::Reg temp2Reg, bool keepAlpha) {
	Describe("ConvertFrom4444");
	// Move red into position within temp1.
	MOV(32, R(temp1Reg), R(colorReg));
	AND(16, R(temp1Reg), Imm16(0xF << 0));
	SHL(32, R(temp1Reg), Imm8(4));

	// Green is just as simple.
	MOV(32, R(temp2Reg), R(colorReg));
	AND(16, R(temp2Reg), Imm16(0xF << 4));
	SHL(32, R(temp2Reg), Imm8(8));
	OR(32, R(temp1Reg), R(temp2Reg));

	// Blue isn't last this time, but it's next.
	MOV(32, R(temp2Reg), R(colorReg));
	AND(16, R(temp2Reg), Imm16(0xF << 8));
	SHL(32, R(temp2Reg), Imm8(12));
	OR(32, R(temp1Reg), R(temp2Reg));

	if (keepAlpha) {
		// Last but not least, alpha.
		AND(16, R(colorReg), Imm16(0xF << 12));
		SHL(32, R(colorReg), Imm8(16));
		OR(32, R(colorReg), R(temp1Reg));

		// Copy to temp1 again for swizzling.
		OR(32, R(temp1Reg), R(colorReg));
	} else {
		// Overwrite colorReg (we need temp1 as a copy anyway.)
		MOV(32, R(colorReg), R(temp1Reg));
	}

	// Masking isn't necessary here since everything is 4 wide.
	SHR(32, R(temp1Reg), Imm8(4));
	OR(32, R(colorReg), R(temp1Reg));
	return true;
}

};

#endif
