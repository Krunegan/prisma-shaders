// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: MIT

// ---- Pass inputs --------------------------------------------------------
uniform sampler2D colorTex;      // scene colour as it was before this pass
uniform vec2 targetSize;         // pixels of what this pass writes (the scene or a buffer)
uniform sampler2D depthTex;      // scene depth

float toNdcDepth(float depth)
{
#if PRISMA_DEPTH_ZERO_TO_ONE
	return depth;
#else
	return depth * 2.0 - 1.0;
#endif
}

bool isSky(float depth) { return depth >= 1.0; }

// The scene as it was when Luanti finished drawing the sky, before blocks,
// entities and particles. Particles and translucent entities don't write
// depth, so over the sky they still read isSky(); skyOverlay() is what they
// added on top of it. A pass that paints its own sky adds it back:
//     color = mySky(dir) + skyOverlay(texcoord);
// Only meaningful while colorTex still holds Luanti's sky (the first pass
// that changes the sky).
uniform sampler2D skyTex;
vec3 skyOverlay(vec2 uv)
{
	return texture(colorTex, uv).rgb - texture(skyTex, uv).rgb;
}

vec3 viewPosition(vec2 uv, float depth)
{
	vec4 p = prisma.projectionMatrixInverse * vec4(uv * 2.0 - 1.0, toNdcDepth(depth), 1.0);
	return p.xyz / p.w;
}

vec3 playerPosition(vec2 uv, float depth)
{
	return (prisma.viewMatrixInverse * vec4(viewPosition(uv, depth), 1.0)).xyz;
}

// Direction of the camera ray through `uv`, in world axes.
vec3 eyeDirection(vec2 uv)
{
	return normalize(mat3(prisma.viewMatrixInverse) * viewPosition(uv, 1.0));
}

// ---- Prisma's own lighting data (pack.toml: gbuffer = true; the shadow map: shadows.glsl) ----
uniform sampler2D gbufferTex;    // rgb: world normal * 0.5 + 0.5, a: share of sunlight

// World normal of the surface at `uv`; zero for plants, sky and unknown.
vec3 gbufferNormal(vec2 uv)
{
	vec3 n = texture(gbufferTex, uv).rgb * 2.0 - 1.0;
	return dot(n, n) > 0.04 ? normalize(n) : vec3(0.0);
}

// How much of the surface's light comes from the sky (1) rather than from
// torches and other light sources (0): Luanti's 1 - nightRatio.
float gbufferSunlight(vec2 uv)
{
	return texture(gbufferTex, uv).a;
}

// gbufferLightTex (gbuffer = true): r = sky light, g = light of torches and
// glowing nodes, b = 1 on liquid surfaces, a = 1 where something was drawn.
uniform sampler2D gbufferLightTex;

// Luanti's day light bank at `uv`, 0..1: 1 straight under the open sky
// (sunlight that falls without obstacles), less under roofs and in caves.
// Doesn't change with the time of day. 1 on entities.
float gbufferSkyLight(vec2 uv)
{
	return texture(gbufferLightTex, uv).r;
}

// Light of torches, lamps and glowing nodes at `uv`, 0..1 (Luanti's night bank).
float gbufferBlockLight(vec2 uv)
{
	return texture(gbufferLightTex, uv).g;
}

// True if the nearest thing at `uv` is the surface of a liquid (water...).
// Luanti writes the depth of liquids: depthTex holds that surface.
bool gbufferLiquid(vec2 uv)
{
	return texture(gbufferLightTex, uv).b > 0.5;
}

// True if the G-buffer has the surface at `uv`: false on the sky and on map
// blocks past the pack's gbuffer_distance (their normal, light and material
// read as nothing). Liquid surfaces are always there.
bool gbufferCovered(vec2 uv)
{
	return texture(gbufferLightTex, uv).a > 0.5;
}

// gbufferMaterialTex (gbuffer = true): what kind of thing is at each pixel,
// from the MATERIAL_TYPE Luanti draws it with. r = id / 255.
uniform sampler2D gbufferMaterialTex;
const int PRISMA_MAT_NONE = 0;          // nothing drawn (sky)
const int PRISMA_MAT_SOLID = 1;         // ordinary nodes
const int PRISMA_MAT_LEAVES = 2;        // waving leaves (allfaces nodes with waving set)
const int PRISMA_MAT_PLANTS = 3;        // waving plants (grass, flowers... with waving set)
const int PRISMA_MAT_LIQUID = 4;        // surface of a translucent liquid (water)
const int PRISMA_MAT_LIQUID_OPAQUE = 5; // opaque liquids: lava, and water with translucent_liquids off
const int PRISMA_MAT_ENTITY = 6;        // entities: players, mobs, dropped items...

int gbufferMaterial(vec2 uv)
{
	return int(texture(gbufferMaterialTex, uv).r * 255.0 + 0.5);
}

// ---- Prisma's overhead map (pack.toml: [overhead]) -----------------------
uniform sampler2D overheadTex;   // depth of the world seen from straight above

// Player-space height (y) of the highest surface (blocks, plants, leaves;
// not liquids or entities) in the column of `playerPos`; -1e4 if there is
// none or the column is outside the map.
float overheadHeight(vec3 playerPos)
{
	if (!prisma.overheadAvailable)
		return -1e4;
	vec3 c = (prisma.overheadMatrix * vec4(playerPos, 1.0)).xyz;
	if (any(lessThan(c.xy, vec2(0.0))) || any(greaterThan(c.xy, vec2(1.0))))
		return -1e4;
	float d = texture(overheadTex, c.xy).r;
	if (d >= 1.0)
		return -1e4;
	return playerPos.y + (c.z - d) * prisma.overheadDepthRange;
}

// 1 if nothing is above `playerPos` (the rain reaches it), 0 if something
// covers it. `margin` (nodes) softens the step; points on the highest
// surface itself count as uncovered.
float overheadExposure(vec3 playerPos, float margin)
{
	return 1.0 - smoothstep(0.05, 0.05 + margin, overheadHeight(playerPos) - playerPos.y);
}

