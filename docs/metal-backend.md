# The Metal backend

On a Mac the game renders with Metal; OpenGL is still there, under **Video ▸
Renderer** in the menu bar or `RECOMP_BACKEND=opengl` in the environment, and
is what the Mac falls back to if Metal cannot start. Linux uses OpenGL. The
iPhone and iPad build is Metal only (see [ios.md](ios.md)).

## Why

OpenGL on macOS is deprecated, frozen at 4.1, and implemented on top of Metal
anyway; iOS has none. And on a Mac the OpenGL path has to read every finished
frame back to the CPU and hand it to the window as pixels, because a
layer-backed OpenGL view will not present what another thread drew (see the
note at the top of `nv2a_window_mac.m`). Metal draws the frame into the
window's `CAMetalLayer` from the render thread directly: no read-back, no copy.

## How it is put together

```
nv2a_gl.c          the NV2A: pushbuffer methods, surfaces, texture decode,
                   vertex fetch, the program and texture caches, every
                   bring-up switch -- shared by both backends
      |
nv2a_backend.h     Nv2aGfx: the device -- targets, textures, programs,
                   uniforms by name, state, vertices, draws, clears, the flip
      |
      +-- nv2a_gfx_gl.c        OpenGL 3.3 core (Linux, macOS)
      +-- nv2a_metal/nv2a_metal.m   Metal (macOS, iOS)
```

The interface is pitched low on purpose. An earlier, higher one ("here is a
program descriptor, cache it yourself") was never adopted by the OpenGL side,
because everything worth having in the renderer would have had to be written
twice. Now the front half keeps all of it and a backend only does what its API
does differently. `nv2a_gfx_gl.c` is the OpenGL code that used to be inline in
`nv2a_gl.c`, moved and not changed; through it the game renders the same
pixels it always did (checked frame by frame against the previous build).

## Shaders: one generator, two dialects

`nv2a_vsh.c` and `nv2a_psh.c` generate one *body* per shader and wrap it per
dialect. The bodies are the same text in both -- every instruction, every
combiner stage, the viewport epilogue, fog, lighting, bump mapping, the alpha
test. An earlier Metal emitter written beside the GLSL one fell weeks behind it
(no fog, no lighting, no bump mapping) without anything noticing; this
arrangement cannot drift.

| | GLSL 330 | Metal |
|---|---|---|
| vector types | built in | `typedef float4 vec4;` and friends |
| uniforms | plain `uniform`s, looked up by name | one struct per stage (`Nv2aVU`, `Nv2aFU`) generated from the table in `nv2a_uniforms.c`, and a `#define` per member so the body reads them by their GLSL names |
| constants `c[192]` | `uniform vec4 c[192]` | a buffer of its own, re-sent only when it changes |
| vertex inputs | `layout(location=N) in vec4 vN` | pulled: one buffer of packed floats and a table of offsets (`Nv2aAttrTable`) -- no pipeline per vertex layout |
| attributes with no array | generic attribute values | the same values, kept by the backend, in the table |
| texture fetch | `NV2A_TEX(t, uv)` = `texture(t, uv)` | `t.sample(t_s, uv)` |
| `mod` | built in (floors) | spelled out (Metal's `fmod` truncates) |
| depth from the fragment stage (w-buffer) | `gl_FragDepth` | a `[[depth(any)]]` output |
| clip depth | z in [-w, w] | z in [0, w], like the console -- `NV2A_CLIP_Z01` |

The uniform structs and the offsets the backend writes at come from the same
table, by the Metal Shading Language's layout rules (a `float3` takes sixteen
bytes), so the two sides of the buffer cannot disagree.

## The traps

Each of these costs a day if missed, and the symptom never names the cause.

- **Clip depth.** OpenGL's clip volume runs z from -w to w; Metal's and the
  console's from 0 to w. The GLSL epilogue adds a remap the Metal one must not
  have. Backwards, everything renders in the right place with half the depth
  range collapsed -- it reads as z-fighting, not as a coordinate bug.
- **Which way up.** OpenGL keeps a render target bottom row first, Metal top
  row first, and the shaders put the top of the screen at clip y = +1 in both.
  So sampling a surface turns t over on OpenGL only (`rt_bottom_up`),
  scissor and clear rectangles arrive top-down and only OpenGL flips them, and
  read-backs come out bottom-up from both because every consumer was written
  against `glReadPixels`.
- **A depth test that is off does not write.** OpenGL's rule; Metal has none,
  and "always pass" with writes on would write.
- **Blend state and the colour mask are part of the pipeline object.** The
  Metal backend keeps pipelines keyed on (program, blend, mask) and
  depth-stencil objects keyed on all of their state.
- **Sampling nothing reads (0, 0, 0, 1)** on OpenGL; Metal promises nothing, so
  an unbound stage gets a 1x1 texture of exactly that.
- **No triangle fans, no line loops** in Metal: drawn indexed, from a fan index
  list built once.
- **A clear is a load action** at the start of a pass (free on Apple GPUs)
  and a triangle anywhere else; a sub-rectangle clear is always a triangle.
- **New textures are not zeroed.** A new target is cleared when it is made, so
  a surface sampled before it is drawn into reads as nothing, not noise.

## What was checked

On an M2 Pro, against the OpenGL renderer, same build, same scripted run:

- The boot sequence (logos, notices) is pixel-identical: every pixel of every
  frame.
- The title screen and Dogenzaka (the tutorial, its dialogue boxes, the
  graffiti markers, shadows, a boost dash, spraying a tag) match; the only
  differences are animation timing between two runs.
- A run through to Dogenzaka under Metal's API validation layer
  (`MTL_DEBUG_LAYER=1`): over a million draws, no validation errors.
- 60 frames a second throughout, in a window and headless; in a window every
  frame reaches the screen (see "The window").
- The window through being resized, switched between 4:3 and 16:9,
  minimised and restored mid-game: the drawable follows the window each time
  and the picture with it.
- Twenty minutes running: memory flat (about 140 MB resident once the boot
  videos are done, 167 textures alive throughout), no crash or stall, 99.8%
  of frames a sixtieth of a second apart.

The CPU the renderer itself spends submitting draws drops to about a third
(vertex upload plus draw calls: 13.5% of the frame on OpenGL, 4% on Metal).

## The window

The flip does not draw into the window. It copies the finished frame into one
of three slots (a GPU copy) and notes which slot is the newest; a thread of its
own -- the presenter -- waits for a new frame, then for the display to hand it a
drawable, and shows the newest frame there is, letterboxed. Nothing else waits
for the display:

- The boot and the loading screens run uncapped, hundreds of frames a second.
  The presenter shows the newest one at each refresh and lets the rest go, as
  the OpenGL window does.
- At a steady sixty every frame is shown. The first version presented from the
  render thread and dropped a frame whenever two were still on their way to
  the screen; in a window a frame takes two to three refreshes to get there, so
  it dropped one frame in three -- forty a second, unevenly, minute after
  minute in a player's log.
- On a display faster than sixty (a MacBook Pro's runs at 120) a frame is held
  on screen for at least one and a half refreshes, so each gets the two it
  should rather than one or three depending on where it landed
  (`presentDrawable:afterMinimumDuration:`; `RECOMP_MTL_PACE=<ms>` overrides,
  0 turns it off).

The report says how it went: frames shown, frames replaced by a newer one
before the display had room, and how long each frame stayed on screen by the
display's own timestamps. Measured in a window on a 120 Hz MacBook Pro: 98% of
frames up for exactly a sixtieth of a second, none dropped.

## Debugging switches that apply to both

`RECOMP_GL_DUMP_FB`, `JSRF_TRACE_AT`, `RECOMP_GL_DRAWID`, `RECOMP_GL_NOCULL`,
`RECOMP_GL_NOBLEND`, `RECOMP_GL_ZVIEW`, `RECOMP_GL_DUMP_MSL=<dir>` (every
program's Metal translation, from either backend) and the rest of the
`RECOMP_GL_*` set work on Metal too. A few are OpenGL-only because they are
built from OpenGL objects: the occlusion-query probe (`[SAMPLES]`), the GL
error check, the depth read-back (`RECOMP_GL_DUMP_DEPTH`). Metal-only:
`RECOMP_MTL_FASTMATH=1` (fast-math shader compilation, off by default) and
`MTL_DEBUG_LAYER=1` (Apple's validation layer).

## What is left

- Pipelines are built the first time a (program, blend) pair is seen, on the
  render thread, as OpenGL compiles its programs. A binary archive
  (`MTLBinaryArchive`) would make the second run free of even that.
- Metal 4's command allocators and argument tables would cut the per-draw cost
  further; nothing here depends on them.
- iOS: done -- the same backend, the app's own window and controls
  ([ios.md](ios.md)).
