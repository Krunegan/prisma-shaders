// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: MIT
/*
	<prisma/tonemap.glsl>: tone curves to take light (HDR) to the screen.

	  * prismaFilmicToneMap(): Luanti's (Hable / Uncharted 2, with its
	    parameters). Takes in and returns approximate sRGB, like the scene.
	  * prismaAcesToneMap(): Narkowicz's fitted ACES. Linear -> linear 0..1.
	  * prismaReinhard(): the simplest. Linear -> linear 0..1.
*/
#ifndef PRISMA_TONEMAP_GLSL
#define PRISMA_TONEMAP_GLSL

vec3 prismaUncharted2(vec3 x)
{
	return ((x * (0.22 * x + 0.03) + 0.002) / (x * (0.22 * x + 0.3) + 0.06)) - 0.03333;
}

vec3 prismaFilmicToneMap(vec3 srgb)
{
	vec3 c = pow(max(srgb, vec3(0.0)), vec3(2.2));
	c = prismaUncharted2(5.5 * c) * 1.036015346;
	return pow(max(c, vec3(0.0)), vec3(1.0 / 1.6));
}

vec3 prismaAcesToneMap(vec3 x)
{
	x = max(x, vec3(0.0));
	return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

vec3 prismaReinhard(vec3 x)
{
	x = max(x, vec3(0.0));
	return x / (1.0 + x);
}

#endif
