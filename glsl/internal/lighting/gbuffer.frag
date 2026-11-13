// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: MIT

in vec2 vTexCoord;
in vec3 vNormal;
in vec4 vLight;
uniform float uAlphaRef;
uniform bool uHasNormal;
uniform bool uHasColor;
uniform int uMaterial;  // geometry::Material (4 = liquid)
out vec4 fragData;     // gbufferTex
out vec4 fragLight;    // gbufferLightTex
out vec4 fragMaterial; // gbufferMaterialTex
#ifdef ARRAY
uniform sampler2DArray uTexture;
flat in uint vLayer;
#else
uniform sampler2D uTexture;
#endif
void main()
{
#ifdef ARRAY
	float a = texture(uTexture, vec3(vTexCoord, float(vLayer))).a;
#else
	float a = texture(uTexture, vTexCoord).a;
#endif
	if (a < uAlphaRef)
		discard;
	// Plants have no normal in Luanti: stored as 0.5 (a zero vector)
	vec3 n = uHasNormal && dot(vNormal, vNormal) > 1e-8 ? normalize(vNormal) : vec3(0.0);
	fragData = vec4(n * 0.5 + 0.5, uHasColor ? vLight.a : 1.0);
	// The light banks: when the day bank is the larger one, the level is
	// the day bank (sky light, 1 = straight under the open sky) and the
	// night bank (torches, glowing nodes) is the part that isn't sunlight.
	// Entities have no vertex light.
	float level = clamp(max(max(vLight.r, vLight.g), vLight.b) * 2.0, 0.0, 1.0);
	float sky = vLight.a > 0.002 ? level : 0.0;
	float block = level * (1.0 - vLight.a);
	fragLight = uHasColor ? vec4(sky, block, uMaterial == 4 ? 1.0 : 0.0, 1.0) : vec4(1.0, 0.0, 0.0, 1.0);
	fragMaterial = vec4(float(uMaterial) / 255.0, 0.0, 0.0, 1.0);
}
