// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: MIT

in vec2 vTexCoord;
uniform float uAlphaRef;
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
}
