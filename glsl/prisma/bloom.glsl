// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: MIT
/*
	<prisma/bloom.glsl>: glow (bloom) like Luanti's. The image is repeatedly
	halved with a 13-tap filter (Jorge Jimenez, Call of Duty: Advanced
	Warfare), then enlarged again, adding each level. Two (#prisma chain)
	passes are enough:

	    // pack.toml: [buffers.glow1] scale = 0.5, [buffers.glow2] scale = 0.25...

	    // passes/50_bloom_down.frag
	    #prisma chain colorTex glow1 glow2 glow3 glow4 glow5
	    #include <prisma/bloom.glsl>
	    void main()
	    {
	    // The first step reads the scene, which is in sRGB
	    	fragColor = vec4(prismaBloomDown(PRISMA_CHAIN_SOURCE, texcoord, PRISMA_CHAIN_STEP == 0), 1.0);
	    }

	    // passes/60_bloom_up.frag
	    #prisma chain glow5 glow4 glow3 glow2 glow1
	    #include <prisma/bloom.glsl>
	    void main()
	    {
	    // Each level (the existing level plus the next one), enlarged
	    	fragColor = vec4(prismaBloomUp(PRISMA_CHAIN_TARGET, PRISMA_CHAIN_SOURCE, texcoord), 1.0);
	    }

	The glow ends up in glow1, in linear color. This library only uses GLSL.
*/
#ifndef PRISMA_BLOOM_GLSL
#define PRISMA_BLOOM_GLSL

vec3 prismaBloomSample(sampler2D t, vec2 uv, bool srgb)
{
	vec3 c = texture(t, uv).rgb;
	return srgb ? pow(max(c, vec3(0.0)), vec3(2.2)) : c;
}

// Average of `src` around `uv`, to write it into a buffer half
// the size. `srgb`: the source is the scene (it must be converted to linear).
vec3 prismaBloomDown(sampler2D src, vec2 uv, bool srgb)
{
	vec2 tx = 1.0 / vec2(textureSize(src, 0));
	vec3 a = prismaBloomSample(src, uv + vec2(-2.0, -2.0) * tx, srgb);
	vec3 b = prismaBloomSample(src, uv + vec2(0.0, -2.0) * tx, srgb);
	vec3 c = prismaBloomSample(src, uv + vec2(2.0, -2.0) * tx, srgb);
	vec3 d = prismaBloomSample(src, uv + vec2(-2.0, 0.0) * tx, srgb);
	vec3 e = prismaBloomSample(src, uv, srgb);
	vec3 f = prismaBloomSample(src, uv + vec2(2.0, 0.0) * tx, srgb);
	vec3 g = prismaBloomSample(src, uv + vec2(-2.0, 2.0) * tx, srgb);
	vec3 h = prismaBloomSample(src, uv + vec2(0.0, 2.0) * tx, srgb);
	vec3 i = prismaBloomSample(src, uv + vec2(2.0, 2.0) * tx, srgb);
	vec3 j = prismaBloomSample(src, uv + vec2(-1.0, -1.0) * tx, srgb);
	vec3 k = prismaBloomSample(src, uv + vec2(1.0, -1.0) * tx, srgb);
	vec3 l = prismaBloomSample(src, uv + vec2(-1.0, 1.0) * tx, srgb);
	vec3 m = prismaBloomSample(src, uv + vec2(1.0, 1.0) * tx, srgb);
	vec3 color = e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
	return max(color, vec3(1e-4));
}

// `base`: this level; `small`: the next one (smaller), which gets enlarged
// with a 9-tap tent filter, and added.
vec3 prismaBloomUp(sampler2D base, sampler2D small, vec2 uv)
{
	vec2 tx = 1.0 / vec2(textureSize(small, 0));
	vec3 a = texture(small, uv + vec2(-1.0, -1.0) * tx).rgb;
	vec3 b = texture(small, uv + vec2(0.0, -1.0) * tx).rgb;
	vec3 c = texture(small, uv + vec2(1.0, -1.0) * tx).rgb;
	vec3 d = texture(small, uv + vec2(-1.0, 0.0) * tx).rgb;
	vec3 e = texture(small, uv).rgb;
	vec3 f = texture(small, uv + vec2(1.0, 0.0) * tx).rgb;
	vec3 g = texture(small, uv + vec2(-1.0, 1.0) * tx).rgb;
	vec3 h = texture(small, uv + vec2(0.0, 1.0) * tx).rgb;
	vec3 i = texture(small, uv + vec2(1.0, 1.0) * tx).rgb;
	vec3 up = (a + c + g + i) * 0.0625 + (b + d + f + h) * 0.125 + e * 0.25;
	return max(texture(base, uv).rgb + up, vec3(1e-4));
}

// 9-tap Gaussian blur along `axis` (in texels of
// `src`): one pass with (1, 0) and another with (0, 1) blur in both directions.
vec3 prismaBlur9(sampler2D src, vec2 uv, vec2 axis)
{
	vec2 stride = axis * 1.6 / vec2(textureSize(src, 0));
	vec3 sum = texture(src, uv).rgb * 0.2270;
	sum += (texture(src, uv + stride).rgb + texture(src, uv - stride).rgb) * 0.1945;
	sum += (texture(src, uv + stride * 2.0).rgb + texture(src, uv - stride * 2.0).rgb) * 0.1216;
	sum += (texture(src, uv + stride * 3.0).rgb + texture(src, uv - stride * 3.0).rgb) * 0.0540;
	sum += (texture(src, uv + stride * 4.0).rgb + texture(src, uv - stride * 4.0).rgb) * 0.0162;
	return sum;
}

#endif
