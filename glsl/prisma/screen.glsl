// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: MIT
/*
	<prisma/screen.glsl>: from world directions and positions to the
	screen, in this frame and the previous one (to reproject:
	temporal accumulation, motion blur...). It only uses `prisma`,
	so it works in passes and in game/.
*/
#ifndef PRISMA_SCREEN_GLSL
#define PRISMA_SCREEN_GLSL

// World direction (sun, moon...) -> screen coordinates (0..1);
// z = 1 if it is in front of the camera, -1 if it is behind.
vec3 prismaOnScreen(vec3 dir)
{
	vec4 clip = prisma.projectionMatrix * vec4(mat3(prisma.viewMatrix) * dir, 0.0);
	if (clip.w <= 0.0)
		return vec3(0.5, 0.5, -1.0);
	return vec3(clip.xy / clip.w * 0.5 + 0.5, 1.0);
}

// The same with the camera of the previous frame (only the rotation: valid for what
// is very far away, like the sky).
vec3 prismaOnPreviousScreen(vec3 dir)
{
	vec4 clip = prisma.prevProjectionMatrix * vec4(mat3(prisma.prevViewMatrix) * dir, 0.0);
	if (clip.w <= 0.0)
		return vec3(0.5, 0.5, -1.0);
	return vec3(clip.xy / clip.w * 0.5 + 0.5, 1.0);
}

// Where a point in the player's space was on screen (0..1) in the
// previous frame, taking into account that the camera has moved.
// z = 1 if it was in front of the camera, -1 if not.
vec3 prismaPreviousScreenPosition(vec3 playerPos)
{
	vec3 prevPlayer = playerPos + (prisma.cameraPosition - prisma.prevCameraPosition);
	vec4 clip = prisma.prevProjectionMatrix * (prisma.prevViewMatrix * vec4(prevPlayer, 1.0));
	if (clip.w <= 0.0)
		return vec3(0.5, 0.5, -1.0);
	return vec3(clip.xy / clip.w * 0.5 + 0.5, 1.0);
}

#endif
