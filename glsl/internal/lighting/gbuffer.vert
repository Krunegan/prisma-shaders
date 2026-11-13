// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: MIT

in vec3 inPosition;
in vec3 inNormal;
in vec4 inColor;
in vec2 inTexCoord;
uniform mat4 uMVP;
uniform mat3 uNormalMatrix;
out vec2 vTexCoord;
out vec3 vNormal;
out vec4 vLight;
#ifdef ARRAY
in uint inLayer;
flat out uint vLayer;
#endif
#ifdef SKINNED
in vec4 inWeights;
in uvec4 inJointIDs;
layout(std140) uniform JointMatrices {
	mat4 joints[1024];
};
#endif
void main()
{
	vTexCoord = inTexCoord;
#ifdef ARRAY
	vLayer = inLayer;
#endif
	vec4 pos = vec4(inPosition, 1.0);
	vec3 normal = inNormal;
#ifdef SKINNED
	if (inWeights != vec4(0.0)) {
		mat4 skin = inWeights.x * joints[inJointIDs.x] + inWeights.y * joints[inJointIDs.y] +
			inWeights.z * joints[inJointIDs.z] + inWeights.w * joints[inJointIDs.w];
		pos = vec4((skin * pos).xyz, 1.0);
		normal = (skin * vec4(normal, 0.0)).xyz;
	}
#endif
	vNormal = uNormalMatrix * normal;
	// Luanti's vertex colour (encode_light in its mapblock_mesh.cpp):
	// rgb = half the light level, the larger of its two light banks;
	// a = share of the day bank (sunlight) in it.
	vLight = inColor;
	gl_Position = uMVP * pos;
}
