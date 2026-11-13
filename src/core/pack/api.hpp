// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
	The data Prisma gives to shaders, shared by pack passes and by the game
	shaders a pack replaces or extends. Everything lives in one uniform struct,
	`prisma`, so it never collides with Luanti's own uniform names
	(cameraPosition, fogColor...) and an #include works the same everywhere.

	Keep in sync with docs/shaderpacks.md.
*/
#include "core/hooks/gl_defs.hpp"
#include "core/tracking/state.hpp"
#include "common/version.hpp"

namespace prisma::api {

constexpr int VERSION = PRISMA_API_VERSION;

// GLSL: struct PrismaState + `uniform PrismaState prisma;` + helpers that do
// not depend on the scene textures.
extern const char *const GLSL;
// GLSL: the shadow map's samplers and lookups (shadowVisibility...). For
// passes and for the fragment stage of game shaders (it uses gl_FragCoord).
extern const char *const SHADOW_GLSL;

// Texture units the shadow maps are bound to for game shaders: Luanti's
// own materials use the first few
constexpr int SHADOW_UNIT = 14, SHADOW_PREV_UNIT = 15;

enum Field {
	F_viewMatrix,
	F_viewMatrixInverse,
	F_projectionMatrix,
	F_projectionMatrixInverse,
	F_prevViewMatrix,
	F_prevProjectionMatrix,
	F_cameraPosition,
	F_prevCameraPosition,
	F_cameraOffset,
	F_sunAvailable,
	F_sunDirection,
	F_moonDirection,
	F_lightDirection,
	F_timeOfDay,
	F_skyHidden,
	F_dayLight,
	F_fogColor,
	F_fogDistance,
	F_fogShading,
	F_cloudColor,
	F_liquidColor,
	F_nearPlane,
	F_farPlane,
	F_shadowAvailable,
	F_shadowMatrix,
	F_shadowMatrixPrev,
	F_shadowBlend,
	F_shadowDistance,
	F_shadowDepthRange,
	F_shadowMapSize,
	F_overheadAvailable,
	F_overheadMatrix,
	F_overheadDistance,
	F_overheadDepthRange,
	F_overheadMapSize,
	F_viewSize,
	F_texelSize,
	F_frameTime,
	F_frameTimeCounter,
	F_animationTimer,
	F_frameCounter,
	F_COUNT
};

struct Locations {
	GLint loc[F_COUNT];
	bool any = false; // false if the program does not use `prisma` at all

	// Uses Prisma's raw GL functions; needs a current context.
	void query(GLuint program);
};

// Uploads every field the program uses. `program` must be the current one.
void upload(const Locations &l, const GameState &s);

} // namespace prisma::api
