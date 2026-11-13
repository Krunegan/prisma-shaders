<!-- SPDX-FileCopyrightText: 2026 Flay Krunegan -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Shader pack development

This document describes the pack format, GLSL interfaces, and validation tools
for Prisma API 1. It is intended for shader pack authors.

## Pack structure

A pack is a directory under `shaderpacks/`:

```text
MyPack/
  pack.toml
  passes/                 # Full-screen fragment or compute shaders
  game/<shader>/          # Luanti shader extensions or replacements
  include/                # Files shared by this pack
  sounds/                 # Optional audio assets
  screenshots/            # Up to 5 PNG images of the pack, for the launcher
```

The launcher shows a pack's screenshots on its page, and on "Download
packs" before the pack is downloaded (the first one in the list, all of
them in the pack's details): PNG files directly in `screenshots/`, in name
order, at most five. Each is at most 4 MB, between 1280 x 720 and
3840 x 2160, and between 4:3 and 21:9 in shape. The lists show the middle
of each, cut to 16:9; the viewer shows it whole. Name the one that best
shows the pack so that it sorts first.

Passes run in alphabetical order. Numbered filenames are a common way to make
the order explicit. In developer mode (see "Validation and debugging"),
saving a pack file while Luanti is running triggers a reload. If a shader fails
to compile, Prisma keeps the last working version.

Do not add a GLSL `#version` directive. Prisma supplies the version and API
declarations. Local includes use quotes; the shared Prisma library uses angle
brackets:

```glsl
#include "common.glsl"
#include <prisma/color.glsl>
```

Quoted includes are resolved relative to the including file, the pack root,
and the pack's `include/` directory. Includes inside block comments are
ignored.

## `pack.toml`

The manifest declares pack metadata, API compatibility, options, buffers,
and optional Prisma or Luanti features.

```toml
api = 1
name = "Example"
description = "A concise description of the pack."
author = "Author"
version = "1.0"

gbuffer = true
hide = ["clouds", "shadows"]

[luanti]
enable_post_processing = true
enable_dynamic_shadows = false

[shadows]
resolution = 2048
distance = 64

[overhead]
resolution = 1024
distance = 64

[buffers.glow]
scale = 0.5
format = "rgba16f"
filter = "linear"
mipmaps = false

[options.fog_density]
label = "Fog density"
type = "float"
default = 0.01
min = 0.0
max = 0.05
step = 0.001
group = "Atmosphere"
description = "Controls the density of the distance fog."
```

`pack.toml` uses a subset of TOML:

- `key = value`, where a value is a string (`"..."` or `'...'`), a number, `true`
  or `false`, or a list of those (`[...]`, which may span several lines);
- `[table]` and `[table.subtable]` headers, and dotted keys such as
  `options.fog.label = "Fog"`;
- `#` comments.

Inline tables (`{...}`), arrays of tables (`[[...]]`), multi-line strings
(`"""..."""`), quoted keys, and dates are not supported, and Prisma's error
names the construct. A key or a table given twice is an error. A table with no
keys still counts: `[buffers.glow]` alone declares a buffer with the default
settings.

Every pack needs a `pack.toml`, and `api` is required in it. `api` is the
Prisma API version the pack is written for; the current version is 1. Prisma
rejects a pack without `api` and a pack that requires a newer API.

The optional `[luanti]` section uses Luanti's `minetest.conf` setting names.
Prisma starts the game with the pack's requested values in a separate config
file. The player's settings are retained. The pack's settings take effect on
the next game start. Set `apply_luanti_settings=0` under `[prisma]` in
`prisma.ini` to disable applying them.

A pack may only change Luanti's rendering settings. Any other key, or a value
of the wrong type, is an error, and the pack is not used:

| Group | Settings |
|---|---|
| Fog and clouds | `enable_fog`, `directional_colored_fog`, `fog_start`, `enable_3d_clouds`, `soft_clouds` |
| Liquids and waving nodes | `translucent_liquids`, `enable_waving_leaves`, `enable_waving_plants`, `enable_waving_water`, `water_wave_height`, `water_wave_length`, `water_wave_speed` |
| Dynamic shadows | `enable_dynamic_shadows`, `shadow_strength_gamma`, `shadow_map_max_distance`, `shadow_map_texture_size`, `shadow_map_texture_32bit`, `shadow_filters`, `shadow_map_color`, `shadow_soft_radius`, `shadow_sky_body_orbit_tilt`, `shadow_poisson_filter`, `shadow_update_frames` |
| Post-processing | `enable_post_processing`, `post_processing_texture_bits`, `tone_mapping`, `enable_auto_exposure`, `exposure_compensation`, `debanding`, `enable_bloom`, `enable_volumetric_lighting`, `enable_translucent_foliage`, `enable_water_reflections` |
| Lighting | `smooth_lighting`, `lighting_alpha`, `lighting_beta`, `lighting_boost`, `lighting_boost_center`, `lighting_boost_spread` |

Settings that are on or off take `true` or `false`; the others take a number.

`requires` lists what the pack needs from the player's Luanti settings. The
only accepted value is `"shadows"`: the pack relies on Luanti's dynamic
shadows, and the in-game menu warns the player when they are off. Any other
value is an error.

```toml
requires = ["shadows"]
```

`[shadows]`, `[overhead]`, and `[buffers.<name>]` declare Prisma resources.
The numeric settings `resolution` and `distance`, and `gbuffer_distance`
(see "G-buffer"), may also reference an option by name, for example
`distance = "shadow_distance"`. A choice option gives the number of its
chosen value: its text read as a number (`values = ["1024", "2048"]`), or,
when the values are names, the matching entry of the option's `numbers`
list. Prisma warns when a referenced choice has neither.

```toml
gbuffer_distance = "profile"

[options.profile]
type = "choice"
values = ["potato", "low", "high"]
numbers = [64, 0, 0]   # what each value stands for where a number is read
```

### Options

Supported types are `float`, `int`, `bool`, and `choice`. Use `label`,
`group`, and `description` for the in-game controls. Numeric options may set
`default`, `min`, `max`, and `step`. Choice options declare `values` and a
matching `default`, and may declare `numbers`, one number for each value,
for the settings above that read a number from the option.

Scalar options are exposed as fields on the `options` uniform, for example
`options.fog_density`. Choices and options with `define = true` are emitted as
uppercase GLSL defines and trigger recompilation when changed. Choice values
are represented by their index; generated constants use the option name and
choice value, such as `QUALITY_HIGH`.

Prisma saves the player's values by option id, and choices by the name of the
value, so reordering `values` keeps the player's selection. A saved value that
no longer fits the option (a renamed or removed choice value, a number outside
`min` and `max`) is discarded and the default is used.

## Fragment passes

A file under `passes/*.frag` is a full-screen pass. Prisma runs it after
Luanti draws the world and before Luanti's post-processing. Each pass reads
the current scene and writes its output to the scene unless it selects a pack
buffer with `#prisma target`.

The standard inputs are:

| Name | Type | Description |
|---|---|---|
| `texcoord` | `vec2` | Pixel center in normalized coordinates |
| `fragColor` | `vec4` | Pass output |
| `colorTex` | `sampler2D` | Current scene |
| `depthTex` | `sampler2D` | Scene depth; 1.0 denotes the sky |
| `skyTex` | `sampler2D` | Scene immediately after the sky was drawn |
| `targetSize` | `vec2` | Dimensions of the current output |
| `prisma` | uniform struct | Game, camera, lighting, and frame data |
| `options` | uniform struct | Scalar pack options |

Additional samplers are available when the corresponding feature is enabled:
`shadowTex` and `shadowTexPrev` for shadows, `gbufferTex`,
`gbufferLightTex`, and `gbufferMaterialTex` for the G-buffer, and
`overheadTex` for the overhead map.

Helper functions include:

```glsl
bool isSky(float depth);
float toNdcDepth(float depth);        // depth texture value -> NDC depth
vec3 viewPosition(vec2 uv, float depth);
vec3 playerPosition(vec2 uv, float depth);
vec3 eyeDirection(vec2 uv);
vec3 skyOverlay(vec2 uv);
float shadowVisibility(vec3 playerPos, vec3 normal, float softness);
vec3 shadowCoord(vec3 playerPos);
vec3 gbufferNormal(vec2 uv);
float gbufferSunlight(vec2 uv);
float gbufferSkyLight(vec2 uv);
float gbufferBlockLight(vec2 uv);
bool gbufferLiquid(vec2 uv);
bool gbufferCovered(vec2 uv);
int gbufferMaterial(vec2 uv);
float overheadHeight(vec3 playerPos);
float overheadExposure(vec3 playerPos, float margin);
```

Check `prisma.shadowAvailable` and `prisma.overheadAvailable` before sampling
their maps. Enable `gbuffer = true` before using G-buffer textures. The shadow
map's samplers and lookups are also available in Luanti's shaders (see "Luanti
shader changes").

`skyOverlay(uv)` returns particles and translucent content drawn over
Luanti's sky. Add it to a replacement sky in the first pass that changes the
scene; later passes no longer have the original sky available.

## Prisma game data

Distances and positions use nodes. Player space uses world axes relative to
the camera; convert it to absolute world coordinates by adding
`prisma.cameraPosition`. View space uses Luanti's left-handed coordinates,
with +Z forward.

The `prisma` uniform exposes the following fields:

| Field | Type | Description |
|---|---|---|
| `viewMatrix`, `viewMatrixInverse` | `mat4` | Player-space and view-space transforms |
| `projectionMatrix`, `projectionMatrixInverse` | `mat4` | View-space and clip-space transforms |
| `prevViewMatrix`, `prevProjectionMatrix` | `mat4` | Previous-frame transforms |
| `cameraPosition`, `prevCameraPosition` | `vec3` | Current and previous absolute camera positions |
| `cameraOffset` | `vec3` | Luanti render-space origin in nodes |
| `sunAvailable` | `bool` | Whether a sun direction is available |
| `sunDirection`, `moonDirection`, `lightDirection` | `vec3` | Unit vectors in world space |
| `timeOfDay` | `float` | 0..1, where 0.5 is noon; -1 if unknown |
| `skyHidden` | `bool` | The game has hidden the sky (another dimension, a realm underground): Luanti draws no sun, moon or stars. `sunAvailable` is then false. See below |
| `dayLight`, `fogColor` | `vec3` | Luanti daylight and fog colors |
| `fogDistance` | `float` | Luanti fog distance in nodes |
| `fogShading` | `float` | Luanti `fogShadingParameter`: `1 / (1 - fog_start)` |
| `cloudColor` | `vec4` | Luanti cloud color; alpha is 0 if unknown |
| `liquidColor` | `vec4` | Luanti's `post_effect_color`; alpha is 0 outside liquids |
| `nearPlane`, `farPlane` | `float` | Camera clip planes |
| `shadowAvailable` | `bool` | Whether the Prisma shadow map is available |
| `shadowMatrix`, `shadowMatrixPrev` | `mat4` | Current and previous shadow-map transforms |
| `shadowBlend` | `float` | Blend weight between shadow directions |
| `shadowDistance`, `shadowDepthRange` | `float` | Shadow map extent and depth range in nodes |
| `shadowMapSize` | `vec2` | Shadow map dimensions in texels |
| `overheadAvailable` | `bool` | Whether the overhead map is available |
| `overheadMatrix` | `mat4` | Player-space to overhead-map transform |
| `overheadDistance`, `overheadDepthRange` | `float` | Overhead map extent and depth range in nodes |
| `overheadMapSize` | `vec2` | Overhead map dimensions in texels |
| `viewSize`, `texelSize` | `vec2` | Scene dimensions in pixels and their inverse |
| `frameTime`, `frameTimeCounter`, `animationTimer` | `float` | Frame duration and elapsed timers in seconds |
| `frameCounter` | `int` | Frame number |

`skyHidden` becomes true a few seconds after Luanti stops drawing its sun
and moon while the camera is away from its cloud layer (by default 120 to 136
nodes up, where Luanti draws no sky either). A mod does that with
`set_sun`, `set_moon` and `set_stars` (`visible = false`), usually together
with a plain `set_sky` and `override_day_night_ratio`. There is no sky to
draw there: a pack should leave out its clouds, sun, moon and stars, and can
take the colour of the place from `fogColor`. It becomes false as soon as
the sky shows again.

`prismaWorldFromLuanti(vec3)` converts Luanti render-space positions (BS
units relative to `cameraOffset`) to absolute positions in nodes.

## Custom lighting

### Shadow map

Declare `[shadows]` in `pack.toml` to request a depth map rendered from the
sun, or the moon at night. The map covers a square around the camera with a
side length of twice `distance` nodes. Leaves, plants, entities, and nodes
cast shadows according to their material and geometry.

Prisma blends maps for the previous and current light directions to reduce
shimmer as the sun moves. Use `shadowVisibility()` for the standard lookup.
Code that samples `shadowTex` directly should also account for
`shadowTexPrev` and `prisma.shadowMatrixPrev`. The pieces `shadowVisibility()`
is built from are available too:

```glsl
// Visibility (0..1) of playerPos in one map: shadowTex with
// prisma.shadowMatrix, or shadowTexPrev with prisma.shadowMatrixPrev
float prismaShadowLookup(sampler2D shadowTex, mat4 shadowMatrix, vec3 playerPos, vec3 normal, float softness);
// 1 if depth z (0..1) is not behind the map at st, filtered between texels
float prismaShadowTap(sampler2D shadowTex, vec2 st, float z);
```

Under a sky the game hides (`skyHidden`) there is no sun to cast the map
from. A pack can bring its own light there: `hidden_sky_light = [x, y, z]`
in `[shadows]` is a direction towards it in world axes, above the horizon
(`y > 0`). While the sky is hidden, Prisma draws the map from that direction
and passes get it as `lightDirection` and `sunDirection` (`moonDirection` is
the opposite); `sunAvailable` stays false. Without the key, there is no
shadow map under a hidden sky.

```toml
[shadows]
hidden_sky_light = [0.9, 0.1, 0.4]   # a light low on the horizon
```

Set `enabled = false` in `[shadows]` to keep the section but not request the
map; the same works in `[overhead]`.

### G-buffer

Set `gbuffer = true` to request per-pixel surface data. The G-buffer provides
world normals and the fraction of light from the sky. Its additional light
texture separates skylight and block light and marks liquid surfaces.

| Texture | Channels |
|---|---|
| `gbufferTex` | `rgb`: encoded world normal; `a`: fraction of light from the sky |
| `gbufferLightTex` | `r`: skylight; `g`: block light; `b`: liquid surface flag; `a`: geometry coverage |
| `gbufferMaterialTex` | `r`: material ID divided by 255 |

`gbufferMaterial(uv)` returns one of these constants:

| Constant | Meaning |
|---|---|
| `PRISMA_MAT_NONE` | Sky |
| `PRISMA_MAT_SOLID` | Regular opaque node |
| `PRISMA_MAT_LEAVES` | Waving leaves |
| `PRISMA_MAT_PLANTS` | Waving plant-like nodes |
| `PRISMA_MAT_LIQUID` | Translucent liquid surface |
| `PRISMA_MAT_LIQUID_OPAQUE` | Opaque liquid: lava, and also water when the player turns off Luanti's `translucent_liquids` (lava lights itself: `gbufferBlockLight` near 1) |
| `PRISMA_MAT_ENTITY` | Entity |

Materials are assigned from Luanti's draw type and material flags. Transparent
non-liquid materials do not replace the background in the G-buffer.

Prisma draws the G-buffer by drawing again the map blocks Luanti drew, so it
costs about as much as the map itself. `gbuffer_distance = N` (nodes, or the
name of an option) leaves out the map blocks farther than that; 0, the
default, keeps all of them. Liquid surfaces and entities are always drawn.
Past the distance the G-buffer reads as on the sky: `gbufferNormal` is zero,
`gbufferSunlight` 1, the light banks and the material 0, and
`gbufferCovered(uv)` is false; a pack can work
out a normal from the depth buffer there, where its fog usually hides the
difference.

```toml
gbuffer = true
gbuffer_distance = 64
```

### Overhead map

Declare `[overhead]` to request a depth map viewed from above. It can identify
whether a position is covered, for effects such as rain, snow, or roof-aware
lighting. Nodes, plants, and leaves contribute; entities and liquids do not.
The map is refreshed as the camera moves and periodically while stationary.

### Hiding Luanti effects

The optional `hide` list disables effects that the pack replaces. Supported
values are `clouds`, `shadows`, `bloom`, `volumetric_light`, `exposure`,
`tonemapping`, and `underwater`. Prisma restores the settings when the pack
is disabled.

## Buffers and pass chains

Declare a buffer in `pack.toml`:

```toml
[buffers.glow]
scale = 0.5
format = "rgba16f"
filter = "linear"
mipmaps = true
```

A buffer is available to every pass as a sampler with the same name.
Buffer and option names must be GLSL identifiers that are not GLSL keywords
or reserved words (such as `out`, `input`, `half` or `fixed`), do not start
with `gl_`, and contain no `__`. Buffer
contents persist between frames and start at zero. A pass can read the prior
contents while writing new contents, which supports temporal effects. Packs
may declare up to 11 buffers.

Specify the output buffer in the pass file itself, usually at the top:

```glsl
#prisma target glow
```

The scene remains available as `colorTex`. `texcoord` and `targetSize` refer
to the selected output.

With `mipmaps = true`, Prisma regenerates the buffer's mip levels after each
write. Read a selected level with `textureLod()`.

Use `#prisma chain` to run one pass over multiple buffers:

```glsl
#prisma chain colorTex glow1 glow2 glow3
```

The first item is the source; each following item is a pack buffer and output
for one step. Chain passes cannot also use `#prisma target`. Prisma defines
`PRISMA_CHAIN_SOURCE`, `PRISMA_CHAIN_TARGET`, `PRISMA_CHAIN_STEP`, and
`PRISMA_CHAIN_STEPS` for each step.

## Compute passes

A `passes/*.comp` file runs in alphabetical order and writes to a pack buffer.
Compute passes require OpenGL 4.3. Prisma provides `prismaImage` as an
`image2D` for the target buffer, `prismaPixel` as the integer pixel
coordinate, and `texcoord` as its normalized center.

Declare the target with `#prisma target`. Compute passes cannot write directly
to the scene or use `#prisma chain`. Work groups default to 8×8; custom sizes
may be set with a GLSL layout declaration. Guard against invocations beyond
the buffer dimensions.

Provide a fragment pass with `#prisma fallback` to support systems without
OpenGL 4.3. Prisma uses the fallback only when compute shaders are
unavailable. `PRISMA_COMPUTE` indicates whether compute shaders are active.
`PRISMA_API` contains the selected API version, and `PRISMA_DEPTH_ZERO_TO_ONE`
indicates the active depth range.

## Shared GLSL library

Include library files with angle brackets. Available modules are:

| File | Functions |
|---|---|
| `color.glsl` | `prismaToLinear`, `prismaToSrgb`, `prismaLuminance`, `prismaSaturation`, `prismaMaxOf` |
| `tonemap.glsl` | `prismaFilmicToneMap`, `prismaAcesToneMap`, `prismaReinhard`, `prismaUncharted2` |
| `noise.glsl` | `prismaIGN`, `prismaIGNAnimated`, `prismaHash11`, `prismaHash12`, `prismaHash13`, `prismaHash22`, `prismaValueNoise`, `prismaFbm` |
| `bloom.glsl` | `prismaBloomSample`, `prismaBloomDown`, `prismaBloomUp`, `prismaBlur9` |
| `screen.glsl` | `prismaOnScreen`, `prismaOnPreviousScreen`, `prismaPreviousScreenPosition` |
| `luanti.glsl` | `prismaLuantiFogClarity`, `prismaLuantiFogTint`, `prismaLuantiShadowStrength`, `prismaLuantiScatteringAtGround` |

For example, include `<prisma/color.glsl>`. Public functions use the
`prisma` prefix.

## Luanti shader changes

Files under `game/<shader>/` extend or replace files in Luanti's
`client/shaders/`. The directory name must match Luanti's shader path.

- `extend_vertex.glsl` and `extend_fragment.glsl` append code to the original
  shader. Define `prisma_vertex()` or `prisma_fragment(inout vec4 color)` to
  add a hook after the original main function.
- `opengl_vertex.glsl` and `opengl_fragment.glsl` replace the original file.
- `patch_vertex.glsl` and `patch_fragment.glsl` insert code before or after
  a matching source line with `#prisma before` or `#prisma after`.

The `prisma` and `options` uniforms are available in these shaders. With
`[shadows]`, the fragment stage also gets `shadowTex`, `shadowTexPrev`,
`shadowVisibility()`, `prismaShadowLookup()` and `prismaShadowTap()`: Luanti
draws the world before Prisma draws this frame's map, so they read the map of
the previous frame, with `prisma.shadowMatrix` already moved to this frame's
camera. Lighting can then be done in the same draw as the world, without a
G-buffer:

```glsl
// game/nodes_shader/patch_fragment.glsl
#prisma after "vec4 col = vec4(base.rgb * varColor.rgb, 1.0);"
	vec3 player = prismaWorldFromLuanti(worldPosition) - prisma.cameraPosition;
	col.rgb *= mix(0.5, 1.0, shadowVisibility(player, vNormal, 1.5));
```

Prefer extensions when possible; replacement shaders must be kept in sync
with Luanti. Patches depend on exact source text and are skipped if the
anchor is not found. Luanti compiles material variants with different
preprocessor definitions. If a hook is conditional, provide an empty
definition for other variants so they continue to compile.

The offline checker cannot validate `game/` changes. Validate them in Luanti
and, in developer mode, inspect the generated source under `logs/shaders/`.

## Audio

Set `sound_buffer` to a declared buffer. Each entry under `[sounds.<name>]`
references an audio file and a control column in that buffer. Supported file
formats are Ogg Vorbis (`.ogg`) and WAV (`.wav`).

```toml
sound_buffer = "audio"

[buffers.audio]
size = [2, 2]
format = "rgba32f"
filter = "nearest"

[sounds.rain]
file = "sounds/rain.ogg"
loop = true
control = 1

[sounds.thunder]
files = ["sounds/thunder_1.ogg", "sounds/thunder_2.ogg"]
control = 2
voices = 3
```

Each sound uses two texels in its control column:

| Row | Channel | Value |
|---|---|---|
| 0 | `r` | Volume: 0 is silent; 1 is the normal level |
| 0 | `g` | Pitch: 0 is normal playback rate |
| 0 | `b` | Trigger for non-looped sounds; play when the value changes to a positive value |
| 0 | `a` | Pan: -1 is left; 1 is right |
| 1 | `r` | Muffle: 0 is clear; 1 applies a low-pass filter |

Looped sounds follow the buffer values continuously. Non-looped sounds play
once when triggered. The optional `volume` value scales the output, and
`voices` sets the number of simultaneous instances for non-looped sounds
(1–16). Audio fades when the game is paused or the pack is disabled.

## Sharing a pack

Prisma's launcher downloads packs from the repository `prisma-packs`, where
each pack is reviewed before it is published. A pack is submitted with a
pull request that adds its folder under `packs/`, exactly as it is
installed. The repository's README states the full requirements; in short:

- `pack.toml` sets `name`, `version`, `author`, `description` and
  `license`, the last one an SPDX identifier such as `CC-BY-4.0`. Prisma
  does not read `license`; the repository and the launcher do. Each new
  version is a new pull request with a higher `version`.
- The licence allows the pack to be copied and redistributed unchanged, and
  its full text is in the pack as `LICENSE.txt` or `LICENSE.md`.
- The pack holds only pack data: `pack.toml`, GLSL (`.frag`, `.comp`,
  `.glsl`), OGG and WAV, text (`.txt`, `.md`), and up to five PNG
  screenshots in `screenshots/`, each between 1280 x 720 and 3840 x 2160
  and between 4:3 and 21:9. No hidden files, no other images. Folder names
  use letters, digits, `_` and `-`.
- `prisma_check` compiles the pack's passes without errors.

The repository's script, `make_packs_index.py`, checks these requirements,
makes a thumbnail of each screenshot, and writes the index the launcher
reads.

The launcher installs a pack into the packs folder under the folder name it
has in `packs/`, with a `.prisma-download` file that marks it as
downloaded; only such packs can be updated or removed from the launcher. A
pack of the same name that was not downloaded is never replaced.

## Validation and debugging

Run `prisma_check <pack>` to compile a pack's passes and supported option
variants without opening Luanti. `prisma_check --stub <pack>` writes
`prisma_api.glsl`, the declarations a pass sees, for an editor's code
completion. Run `prisma_check --internal` to validate
Prisma's internal shaders. Compute passes are checked when the active driver
supports OpenGL 4.3.

The in-game Debug page provides pass timings, a texture viewer, per-pixel
values, and the data supplied to passes. Use it to inspect buffers, depth,
G-buffer channels, and Prisma uniforms.

Set `developer_mode = 1` under `[debug]` in `prisma.ini` while writing a pack.
Developer mode adds:

- reload on save: Prisma checks the pack's folder twice a second and reloads
  it when a file changes;
- every shader Luanti compiles, as Prisma changed it, in `logs/shaders/`;
- the "Trace one frame" button in the in-game menu, which writes everything
  Luanti draws in one frame to `logs/frame_trace_NN.txt` (one trace is also
  taken a few seconds after the world appears);
- the list of GL functions the game asks for, in `logs/gl_procs.txt`.

Players do not need it; without it, Prisma skips this work.
