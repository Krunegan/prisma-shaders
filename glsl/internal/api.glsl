// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: MIT
// ---- Prisma API 1 -------------------------------------------------------
// Units are nodes. Player space = world axes with the origin at the camera.
// View space is Luanti's: left-handed, +Z forward.
struct PrismaState {
	mat4 viewMatrix;              // player -> view
	mat4 viewMatrixInverse;
	mat4 projectionMatrix;        // view -> clip
	mat4 projectionMatrixInverse;
	mat4 prevViewMatrix;          // previous frame
	mat4 prevProjectionMatrix;
	vec3 cameraPosition;          // absolute world position of the camera
	vec3 prevCameraPosition;
	vec3 cameraOffset;            // Luanti's render origin, in nodes

	bool sunAvailable;            // false if the sun can't be found (e.g. no sky)
	vec3 sunDirection;            // unit vector towards the sun (world)
	vec3 moonDirection;
	vec3 lightDirection;          // towards the body casting the shadows
	float timeOfDay;              // 0..1 (0.5 = noon), -1 if unknown
	bool skyHidden;               // the game has hidden the sky (another dimension, a realm
	                              // underground): no sun, moon or stars; sunAvailable is false

	vec3 dayLight;                // Luanti's day light colour
	vec3 fogColor;
	float fogDistance;            // nodes
	float fogShading;             // Luanti's fogShadingParameter (1 / (1 - fog_start))
	vec4 cloudColor;              // colour of Luanti's clouds now; a = 0 if unknown
	vec4 liquidColor;             // the camera is in a liquid: the colour Luanti tints the screen with
	                              // (post_effect_color); a = 0 out of liquids
	float nearPlane;
	float farPlane;

	// Prisma's own shadow map (pack.toml [shadows]); passes sample it with
	// shadowVisibility(), see docs/shaderpacks.md
	bool shadowAvailable;
	mat4 shadowMatrix;            // player space -> shadow map (xy texture coords, z depth; 0..1)
	mat4 shadowMatrixPrev;        // same for shadowTexPrev, the map at the previous sun direction
	float shadowBlend;            // how much of shadowTex (vs shadowTexPrev) to use
	float shadowDistance;         // nodes from the centre of the map to its edge
	float shadowDepthRange;       // nodes between depth 0 and depth 1
	vec2 shadowMapSize;           // texels

	// Prisma's overhead map (pack.toml [overhead]): the world from straight
	// above; passes read it with overheadHeight(), see docs/shaderpacks.md
	bool overheadAvailable;
	mat4 overheadMatrix;          // player space -> overhead map (xy texture coords, z depth; 0..1)
	float overheadDistance;       // nodes from the centre of the map to its edge
	float overheadDepthRange;     // nodes between depth 0 (above) and depth 1 (below)
	vec2 overheadMapSize;         // texels

	vec2 viewSize;               // pixels
	vec2 texelSize;               // 1 / viewSize
	float frameTime;              // seconds since the previous frame
	float frameTimeCounter;       // seconds since start
	float animationTimer;         // Luanti's animation timer
	int frameCounter;
};
uniform PrismaState prisma;

// Luanti's shaders work in "render space": BS units (10 per node), relative
// to the camera offset. This turns such a position (e.g. the `worldPosition`
// varying of nodes_shader) into an absolute world position in nodes.
vec3 prismaWorldFromLuanti(vec3 renderPos)
{
	return renderPos * 0.1 + prisma.cameraOffset;
}
// -------------------------------------------------------------------------
