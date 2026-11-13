// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: MIT
/*
	<prisma/luanti.glsl>: formulas from Luanti's shaders, so that a pack
	can undo or imitate what Luanti already did (its fog, the strength of
	its shadows by time of day, the color of the sun through the atmosphere).
	It only uses `prisma`, so it works in passes and in game/.
*/
#ifndef PRISMA_LUANTI_GLSL
#define PRISMA_LUANTI_GLSL

// How much fog Luanti put at `distance` nodes from the camera: 1 = none,
// 0 = fog only (as in its nodes_shader).
float prismaLuantiFogClarity(float distance)
{
	return clamp(prisma.fogShading - prisma.fogShading * distance / prisma.fogDistance, 0.0, 1.0);
}

// The tint Luanti gives its fog for that clarity
vec3 prismaLuantiFogTint(vec3 fog, float clarity)
{
	float m = max(max(fog.r, fog.g), fog.b);
	if (m < 0.0000001)
		m = 1.0;
	return fog * pow(fog / m, vec3(2.0 * clarity));
}

// The strength of the shadows by time of day, like adj_shadow_strength of
// Luanti: the moon casts half, and when the body changes the shadows fade
// so they do not jump suddenly.
float prismaLuantiShadowStrength(float strength)
{
	float t = prisma.timeOfDay;
	if (t < 0.0)
		return strength;
	if (t < 0.2)
		return strength * 0.5 * (1.0 - smoothstep(0.18, 0.2, t));
	if (t >= 0.8)
		return strength * 0.5 * smoothstep(0.8, 0.83, t);
	return strength * smoothstep(0.2, 0.25, t) * (1.0 - smoothstep(0.7, 0.8, t));
}

// The light of a body in `toLight` after going through the atmosphere
// (getDirectLightScatteringAtGround in Luanti): white up high, orange and
// red near the horizon.
vec3 prismaLuantiScatteringAtGround(vec3 toLight)
{
	const vec3 beta = vec3(3.3362176e-01, 8.75378289198826e-01, 1.95342379700656) * 1e-5;
	return exp(-beta * 15000.0 / (1e-5 + max(toLight.y, 0.0)));
}

#endif
