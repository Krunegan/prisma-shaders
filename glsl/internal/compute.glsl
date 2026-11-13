// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: MIT

// ---- Compute pass ----
// The buffer of #prisma target, read (imageLoad) and written (imageStore)
// in place. Its sampler has the same texture: read it through prismaImage.
layout(binding = 0, @IMAGE_FORMAT@) uniform restrict image2D prismaImage;
// The pixel of the target this invocation works on, and its centre as in a .frag
#define prismaPixel ivec2(gl_GlobalInvocationID.xy)
#define texcoord ((vec2(gl_GlobalInvocationID.xy) + 0.5) / targetSize)
