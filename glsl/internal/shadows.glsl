// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: MIT

// ---- Prisma's shadow map (pack.toml: [shadows]) ----------------------------
// In every pass, and in the fragment stage of Luanti's shaders a pack
// changes (game/). There it is the map drawn at the end of the previous
// frame, with prisma.shadowMatrix already moved to this frame's camera.
uniform sampler2D shadowTex;     // depth of the world seen from the sun / moon
uniform sampler2D shadowTexPrev; // the same from the previous sun direction (see shadowVisibility)

// Player-space position -> shadow map (xy texture coordinates, z depth).
vec3 shadowCoord(vec3 playerPos)
{
	return (prisma.shadowMatrix * vec4(playerPos, 1.0)).xyz;
}

// 1 if `z` is not behind the depth at `st` (bilinear between texels).
float prismaShadowTap(sampler2D shadowTex, vec2 st, float z)
{
	vec2 size = prisma.shadowMapSize;
	vec2 f = st * size - 0.5;
	vec2 i = floor(f);
	vec2 w = f - i;
	ivec2 c = ivec2(i);
	ivec2 hi = ivec2(size) - 1;
	float a = step(z, texelFetch(shadowTex, clamp(c, ivec2(0), hi), 0).r);
	float b = step(z, texelFetch(shadowTex, clamp(c + ivec2(1, 0), ivec2(0), hi), 0).r);
	float d = step(z, texelFetch(shadowTex, clamp(c + ivec2(0, 1), ivec2(0), hi), 0).r);
	float e = step(z, texelFetch(shadowTex, clamp(c + ivec2(1, 1), ivec2(0), hi), 0).r);
	return mix(mix(a, b, w.x), mix(d, e, w.x), w.y);
}

float prismaShadowLookup(sampler2D shadowTex, mat4 shadowMatrix, vec3 playerPos, vec3 normal, float softness)
{
	float texel = 2.0 * prisma.shadowDistance / prisma.shadowMapSize.x;
	vec3 L = prisma.lightDirection;
	bool hasNormal = dot(normal, normal) > 0.01;
	float cosL = hasNormal ? clamp(dot(normal, L), 0.05, 1.0) : 0.5;
	float sinL = sqrt(1.0 - cosL * cosL);
	vec3 p = playerPos + (hasNormal ? normal * texel * (1.0 + 1.5 * sinL) : L * texel * 2.0);
	vec3 sc = (shadowMatrix * vec4(p, 1.0)).xyz;
	if (sc.z >= 1.0 || any(lessThan(sc.xy, vec2(0.0))) || any(greaterThan(sc.xy, vec2(1.0))))
		return 1.0;
	float z = sc.z - texel * (1.0 + 2.0 * min(sinL / cosL, 4.0)) / prisma.shadowDepthRange;
	float lit;
	if (softness <= 0.0) {
		lit = prismaShadowTap(shadowTex, sc.xy, z);
	} else {
		// Vogel disk, rotated per pixel
		float angle = 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
		vec2 radius = softness / prisma.shadowMapSize;
		float sum = 0.0;
		for (int k = 0; k < 12; k++) {
			float r = sqrt((float(k) + 0.5) / 12.0);
			float t = float(k) * 2.4 + angle;
			sum += prismaShadowTap(shadowTex, sc.xy + vec2(cos(t), sin(t)) * r * radius, z);
		}
		lit = sum / 12.0;
	}
	vec2 edge = abs(sc.xy * 2.0 - 1.0);
	return mix(lit, 1.0, smoothstep(0.85, 1.0, max(edge.x, edge.y)));
}

// How much direct light from the sun (the moon at night) reaches
// `playerPos`: 1 = lit, 0 = in shadow. `normal` (world, may be zero) moves
// the lookup off the surface to avoid self-shadowing; `softness` is the blur
// radius in texels (0 = hard edges). Fades out towards the edge of the map.
// Blends the maps of the previous and latest sun direction, so shadows
// glide as the sun moves instead of shimmering.
float shadowVisibility(vec3 playerPos, vec3 normal, float softness)
{
	if (!prisma.shadowAvailable)
		return 1.0;
	float b = prisma.shadowBlend;
	float latest = b > 0.001 ? prismaShadowLookup(shadowTex, prisma.shadowMatrix, playerPos, normal, softness) : 0.0;
	if (b >= 0.999)
		return latest;
	float prev = prismaShadowLookup(shadowTexPrev, prisma.shadowMatrixPrev, playerPos, normal, softness);
	return mix(prev, latest, b);
}
