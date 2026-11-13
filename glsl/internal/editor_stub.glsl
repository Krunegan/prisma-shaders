// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: MIT
/*
	prisma_api.glsl — for the editor, not for Prisma.

	What Prisma puts in front of each pass of the pack @PACK@ (API @API@, its options and
	its buffers), so that an editor with GLSL checking knows
	`prisma`, `options`, colorTex, gbufferNormal()... A pass includes it with

	    #include "prisma_api.glsl"

	and Prisma skips that #include: in the game nothing changes. Generate it with
	`prisma_check --stub @PACK@`; regenerate it when the options
	or the buffers in pack.toml change.
*/
#ifndef PRISMA
#define PRISMA_API @API@
#define PRISMA_COMPUTE 1
#define PRISMA_DEPTH_ZERO_TO_ONE 0
// In a pass with #prisma chain, Prisma defines these in each step
#ifndef PRISMA_CHAIN_SOURCE
#define PRISMA_CHAIN_SOURCE colorTex
#define PRISMA_CHAIN_TARGET colorTex
#define PRISMA_CHAIN_STEP 0
#define PRISMA_CHAIN_STEPS 1
#endif
@DECLARATIONS@// In a .frag
in vec2 texcoord;
out vec4 fragColor;
#endif // PRISMA
