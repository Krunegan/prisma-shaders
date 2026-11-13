// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: MIT
/*
	<prisma/color.glsl>: color conversions and measurements.

	The scene travels between passes in approximate sRGB, as in Luanti: to mix
	light it is converted to linear with prismaToLinear(), and returned at the end with
	prismaToSrgb(). It only uses GLSL, so it works in passes and in game/.
*/
#ifndef PRISMA_COLOR_GLSL
#define PRISMA_COLOR_GLSL

// approximate sRGB (gamma 2.2, like Luanti) -> linear
vec3 prismaToLinear(vec3 c)
{
	return pow(max(c, vec3(0.0)), vec3(2.2));
}

// Linear -> approximate sRGB
vec3 prismaToSrgb(vec3 c)
{
	return pow(max(c, vec3(0.0)), vec3(1.0 / 2.2));
}

// Luminance (Rec. 709), the one Luanti uses
float prismaLuminance(vec3 c)
{
	return dot(c, vec3(0.2125, 0.7154, 0.0721));
}

// 0 = grey, 1 = same, > 1 = more saturated
vec3 prismaSaturation(vec3 c, float amount)
{
	return mix(vec3(prismaLuminance(c)), c, amount);
}

float prismaMaxOf(vec3 c)
{
	return max(c.r, max(c.g, c.b));
}

#endif
