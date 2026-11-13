// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: MIT
/*
	<prisma/noise.glsl>: noise and pseudo-random numbers.

	  * prismaIGN(): interleaved gradient noise (Jimenez), to rotate the
	    samples of each pixel (soft shadows, rays, clouds...).
	    prismaIGNAnimated() changes every frame, for effects that are
	    averaged over time.
	  * prismaHash*(): numbers from 0 to 1 from a position (hashes by
	    Dave Hoskins); the number in the name is how many inputs and outputs there are.
	  * prismaValueNoise(): smooth noise from 0 to 1 (in 2D and 3D) and
	    prismaFbm(): several octaves added together.

	prismaIGNAnimated() uses prisma.frameCounter; the rest is only GLSL.
*/
#ifndef PRISMA_NOISE_GLSL
#define PRISMA_NOISE_GLSL

float prismaIGN(vec2 pixel)
{
	return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

float prismaIGNAnimated(vec2 pixel)
{
	return fract(prismaIGN(pixel) + float(prisma.frameCounter % 64) * 0.61803399);
}

float prismaHash11(float p)
{
	p = fract(p * 0.1031);
	p *= p + 33.33;
	p *= p + p;
	return fract(p);
}

float prismaHash12(vec2 p)
{
	vec3 q = fract(vec3(p.xyx) * 0.1031);
	q += dot(q, q.yzx + 33.33);
	return fract((q.x + q.y) * q.z);
}

float prismaHash13(vec3 p)
{
	p = fract(p * 0.1031);
	p += dot(p, p.zyx + 31.32);
	return fract((p.x + p.y) * p.z);
}

vec2 prismaHash22(vec2 p)
{
	vec3 q = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
	q += dot(q, q.yzx + 33.33);
	return fract((q.xx + q.yz) * q.zy);
}

float prismaValueNoise(vec2 x)
{
	vec2 i = floor(x);
	vec2 f = fract(x);
	f = f * f * (3.0 - 2.0 * f);
	return mix(mix(prismaHash12(i), prismaHash12(i + vec2(1.0, 0.0)), f.x),
		mix(prismaHash12(i + vec2(0.0, 1.0)), prismaHash12(i + vec2(1.0, 1.0)), f.x), f.y);
}

float prismaValueNoise(vec3 x)
{
	vec3 i = floor(x);
	vec3 f = fract(x);
	f = f * f * (3.0 - 2.0 * f);
	float a = mix(mix(prismaHash13(i), prismaHash13(i + vec3(1.0, 0.0, 0.0)), f.x),
		mix(prismaHash13(i + vec3(0.0, 1.0, 0.0)), prismaHash13(i + vec3(1.0, 1.0, 0.0)), f.x), f.y);
	float b = mix(mix(prismaHash13(i + vec3(0.0, 0.0, 1.0)), prismaHash13(i + vec3(1.0, 0.0, 1.0)), f.x),
		mix(prismaHash13(i + vec3(0.0, 1.0, 1.0)), prismaHash13(i + vec3(1.0, 1.0, 1.0)), f.x), f.y);
	return mix(a, b, f.z);
}

// Fractal noise, 0..1: `octaves` layers, each at double the frequency and
// half the weight
float prismaFbm(vec2 p, int octaves)
{
	const mat2 rot = mat2(0.80, 0.60, -0.60, 0.80);
	float f = 0.0;
	float w = 0.5;
	float total = 0.0;
	for (int i = 0; i < octaves; i++) {
		f += w * prismaValueNoise(p);
		total += w;
		p = rot * p * 2.03;
		w *= 0.5;
	}
	return f / max(total, 1e-6);
}

float prismaFbm(vec3 p, int octaves)
{
	float f = 0.0;
	float w = 0.5;
	float total = 0.0;
	for (int i = 0; i < octaves; i++) {
		f += w * prismaValueNoise(p);
		total += w;
		p = p * 2.03 + vec3(17.1, 31.7, 7.3);
		w *= 0.5;
	}
	return f / max(total, 1e-6);
}

#endif
