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

#pragma once

#include "ppsspp_config.h"

#include <unordered_map>
#include "GPU/Math3D.h"
#include "GPU/Software/FuncId.h"
#include "GPU/Software/RasterizerRegCache.h"

namespace Sampler {

// Our std::unordered_map argument will ignore the alignment attribute, but that doesn't matter.
// We'll still have and want it for the actual function call, to keep the args in vector registers.
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wignored-attributes"
#endif

typedef Rasterizer::Vec4IntResult(SOFTRAST_CALL *FetchFunc)(int u, int v, const u8 *tptr, int bufw, int level);
FetchFunc GetFetchFunc(SamplerID id);

typedef Rasterizer::Vec4IntResult (SOFTRAST_CALL *NearestFunc)(float s, float t, int x, int y, Rasterizer::Vec4IntArg prim_color, const u8 *const *tptr, const int *bufw, int level, int levelFrac);
NearestFunc GetNearestFunc(SamplerID id);

typedef Rasterizer::Vec4IntResult (SOFTRAST_CALL *LinearFunc)(float s, float t, int x, int y, Rasterizer::Vec4IntArg prim_color, const u8 *const *tptr, const int *bufw, int level, int levelFrac);
LinearFunc GetLinearFunc(SamplerID id);

struct Funcs {
	NearestFunc nearest;
	LinearFunc linear;
};
static inline Funcs GetFuncs() {
	Funcs f;
	SamplerID id;
	ComputeSamplerID(&id);
	f.nearest = GetNearestFunc(id);
	f.linear = GetLinearFunc(id);
	return f;
}

void Init();
void Shutdown();

bool DescribeCodePtr(const u8 *ptr, std::string &name);

class SamplerJitCache : public Rasterizer::CodeBlock {
public:
	SamplerJitCache();

	// Returns a pointer to the code to run.
	NearestFunc GetNearest(const SamplerID &id);
	LinearFunc GetLinear(const SamplerID &id);
	FetchFunc GetFetch(const SamplerID &id);
	void Clear();

	std::string DescribeCodePtr(const u8 *ptr);

private:
	FetchFunc CompileFetch(const SamplerID &id);
	NearestFunc CompileNearest(const SamplerID &id);
	LinearFunc CompileLinear(const SamplerID &id);

	void Describe(const std::string &message);

	Rasterizer::RegCache::Reg GetZeroVec();
	Rasterizer::RegCache::Reg GetGState();

	void WriteConstantPool(const SamplerID &id);

	bool Jit_ReadTextureFormat(const SamplerID &id);
	bool Jit_GetTexData(const SamplerID &id, int bitsPerTexel);
	bool Jit_GetTexDataSwizzled(const SamplerID &id, int bitsPerTexel);
	bool Jit_GetTexDataSwizzled4(const SamplerID &id);
	bool Jit_Decode5650();
	bool Jit_Decode5551();
	bool Jit_Decode4444();
	bool Jit_TransformClutIndex(const SamplerID &id, int bitsPerIndex);
	bool Jit_ReadClutColor(const SamplerID &id);
	bool Jit_GetDXT1Color(const SamplerID &id, int blockSize, int alpha);
	bool Jit_ApplyDXTAlpha(const SamplerID &id);
	bool Jit_GetTexelCoords(const SamplerID &id);

	bool Jit_GetTexelCoordsQuad(const SamplerID &id);
	bool Jit_PrepareDataOffsets(const SamplerID &id, Rasterizer::RegCache::Reg uReg, Rasterizer::RegCache::Reg vReg, bool level1);
	bool Jit_PrepareDataDirectOffsets(const SamplerID &id, Rasterizer::RegCache::Reg uReg, Rasterizer::RegCache::Reg vReg, bool level1, int bitsPerTexel);
	bool Jit_PrepareDataSwizzledOffsets(const SamplerID &id, Rasterizer::RegCache::Reg uReg, Rasterizer::RegCache::Reg vReg, bool level1, int bitsPerTexel);
	bool Jit_FetchQuad(const SamplerID &id, bool level1);
	bool Jit_GetDataQuad(const SamplerID &id, bool level1, int bitsPerTexel);
	bool Jit_TransformClutIndexQuad(const SamplerID &id, int bitsPerIndex);
	bool Jit_ReadClutQuad(const SamplerID &id, bool level1);
	bool Jit_BlendQuad(const SamplerID &id, bool level1);
	bool Jit_DecodeQuad(const SamplerID &id, bool level1);
	bool Jit_Decode5650Quad(const SamplerID &id, Rasterizer::RegCache::Reg quadReg);
	bool Jit_Decode5551Quad(const SamplerID &id, Rasterizer::RegCache::Reg quadReg);
	bool Jit_Decode4444Quad(const SamplerID &id, Rasterizer::RegCache::Reg quadReg);

	bool Jit_ApplyTextureFunc(const SamplerID &id);

#if PPSSPP_ARCH(ARM64)
	Arm64Gen::ARM64FloatEmitter fp;
#elif PPSSPP_ARCH(AMD64) || PPSSPP_ARCH(X86)
	int stackArgPos_ = 0;
	int stackFracUV1Offset_ = 0;
#endif

	const u8 *constWidth256f_ = nullptr;
	const u8 *constHeight256f_ = nullptr;
	const u8 *constWidthMinus1i_ = nullptr;
	const u8 *constHeightMinus1i_ = nullptr;
	const u8 *constUNext_ = nullptr;
	const u8 *constVNext_ = nullptr;
	const u8 *constOnes32_ = nullptr;
	const u8 *constOnes16_ = nullptr;
	const u8 *const10All16_ = nullptr;
	const u8 *const10Low_ = nullptr;
	const u8 *const10All8_ = nullptr;
	const u8 *const5551Swizzle_ = nullptr;
	const u8 *const5650Swizzle_ = nullptr;

	std::unordered_map<SamplerID, NearestFunc> cache_;
	std::unordered_map<SamplerID, const u8 *> addresses_;
	std::unordered_map<const u8 *, std::string> descriptions_;
	Rasterizer::RegCache regCache_;
};

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

};
