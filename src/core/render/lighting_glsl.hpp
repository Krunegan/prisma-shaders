// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
	Shaders of Prisma's own lighting (lighting.cpp): the shadow map casters
	and the G-buffer. Compiled with "#version 150" and, as needed,
	"#define ARRAY 1" (array textures) and "#define SKINNED 1" (animated
	meshes) in front. The sources are in glsl/internal/lighting/, embedded at
	build time; plain strings so prisma_check can test them.
*/

#include "glsl_embedded.hpp"

namespace prisma::lighting::glsl {

inline constexpr const char *CASTER_VS = embedded::lighting_caster_vert;

// Same alpha test as Luanti's shadow pass: only fairly opaque texels cast.
inline constexpr const char *CASTER_FS = embedded::lighting_caster_frag;

inline constexpr const char *GBUFFER_VS = embedded::lighting_gbuffer_vert;

inline constexpr const char *GBUFFER_FS = embedded::lighting_gbuffer_frag;

} // namespace prisma::lighting::glsl
