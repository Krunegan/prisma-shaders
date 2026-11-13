// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: MIT
#version 150
out vec2 texcoord;
void main()
{
	// Fullscreen triangle, no vertex buffer needed.
	vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
	texcoord = p;
	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
