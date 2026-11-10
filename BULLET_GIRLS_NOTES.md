# Bullet Girls (PCSG00361) on Vita3K — investigation notes

Notes kept alongside the Vita3K-BG fork. The fixes themselves live on the
`bullet-girls-fixes` branch; the full history of the Vulkan investigation
(test switches, diagnostics, applied upstream PRs) is preserved on the
`vulkan-investigation` branch.

## Fixed: audio cuts out ~1:45 into a mission, game hangs at mission end

`sceAudiodecPartlyDecode` wrote the requested samples at
`pcm_data + offset * bytes_per_sample` instead of at the start of the game's
output buffer. libatrac asks for a non-zero offset at music loop points that
aren't superframe-aligned, so the write overflowed the buffer by
`offset * bytes_per_sample` and overwrote the sound thread's audio port IDs.
The thread then looped forever on `SCE_AUDIO_OUT_ERROR_INVALID_PORT`, all audio
stopped, and the game hung when it later waited on that thread.

Confirmed fixed on three stages, including letting the timer run out.

## Fixed: triangular cuts in walls/floors at certain camera distances (both renderers)

Symptom: a triangle-shaped section of a wall (stage 3) or ground (stage 4)
showed a different, plain surface at some camera distances and angles, while
the transparent dust/flower overlay on top stayed visible. Getting closer made
it go away.

Found with a RenderDoc capture (OpenGL). Pixel history at the cut: the brick
wall draw failed the depth test (its depth 0.972) against a gray slab drawn
earlier behind it (0.922); the overlay is drawn with depth writes off, so it
still showed. The failing brick triangle had one vertex just in front of the
camera (w ≈ 0.01) but outside the near plane.

Cause: the vertex shader finalize in `spirv_recompiler.cpp` computed
`max(z/w * z_scale + z_offset, 0) * w`. That per-vertex clamp moves the depth
of any vertex past the near plane, and because depth is interpolated across
the triangle, every pixel of the triangle got a too-far depth. Once the
vertex went behind the camera (w < 0) the triangle was clipped normally,
hence the cut disappearing up close.

Fix: keep depth linear in clip space (`z * z_scale + z_offset * w`, remapped
to −w..w for OpenGL) and clamp per pixel instead (`GL_DEPTH_CLAMP`; Vulkan
`depthClampEnable` when the device supports it). Verified by shader
replacement in the captured frame, then in game on stages 3 and 4. The shader
cache version was bumped to 19 so old cached shaders are rebuilt.

Ruled out along the way: z-fighting/invariance, mipmaps/LOD bias, resolution
scale, anisotropic filtering, depth bias.

## Fixed: stage textures misaligned on Vulkan (NVIDIA RTX 2060, Linux; rare)

Rare and setup-dependent: other users' Vulkan screenshots didn't show it.
Fixed by zeroing register banks and guarding indexed reads (see "Finding
from RenderDoc captures" below); confirmed in game.

### Symptom
- Stage geometry only; characters, HUD and menus are fine.
- The correct textures are used, but their texture coordinates are wrong per
  vertex: some corners line up, others are offset, tilted or scrambled.
- Some surfaces change every frame (large errors), others every 1–3 s (small
  errors). Some stage-3 ground textures appear to rotate with the camera.
- The OpenGL renderer draws the same stages correctly.
- The Vulkan validation layer reports nothing for the stage draws.

### What the stage vertex shaders do
From the game's shader log (Vita3K shaderlog dumps):

- Each vertex carries a U16 index. The shader converts it, computes
  `index * 16 + K` with `IMAD2` (K is a GXP literal in an SA register,
  bit pattern `0x00010020`), and does an 8-byte `LDR` from
  `uniform buffer 3 + that value`. Vita3K subtracts `0x10000` for
  register-indexed loads, so the effective address is
  `buffer3 + 32 + index * 16`.
- The loaded `vec2` is added to the vertex's own texcoord and written to the
  texcoord output. So each vertex looks up its own UV offset from a table.
- Buffer 3 is declared as 40 words (160 bytes). The same shaders also load a
  per-vertex 4x4 matrix from buffer 1 using another literal stride (`0x40`).

The bad values almost certainly come from that per-vertex UV-offset lookup.

### Ruled out (each tested on the user's machine with no change)
- Texture cache contents / re-uploads (logged: stage textures are stable)
- Render targets substituted for textures (only 4 legitimate full-screen ones)
- Texture descriptor set reuse (forced fresh descriptors per draw)
- Page-protection vs. hashing for texture change detection
- Depth not cleared for macroblocks on the full-screen fallback path
  (a real gap, fixed on the investigation branch, but not the cause)
- RelaxedPrecision / half-precision texture coordinates (all stripped)
- Fragment texcoord linkage (upstream PR #4299 applied)
- GXP literal bits flushed as float denormals (PR #4299's literal change,
  plus exact integer constants for integer reads of literal SA registers,
  with the shader cache version bumped so shaders regenerated)
- Ring buffers overwriting in-flight data (upstream PR #4248 applied)
- "Texture viewport" UV adjustment (disabled via High Accuracy)
- Vertex attribute format translation (compared line by line with OpenGL)
- Memory mapping mode (all modes, including Disabled)
- Stale GPU copy past the declared 160 bytes of buffer 3 (synced 8 KiB)

### Finding from RenderDoc captures (Vulkan + OpenGL, stage 3)
The stage shaders seen in these captures (inputs `aXYZ aCOL aUV aTanm`) don't
use the `LDR` path above; they read the UV offset with an indexed
secondary-attribute read:

- `idx = (aTanm.x << 1) + 42`, then two reads of `sa[idx * 2 + 14]` and
  `sa[idx * 2 + 15]` (word offsets), added to `aUV` for the texcoord output.
- `aTanm.x` ranges 0–31 on stage 3, so the word offset is 98–223, while Vita3K
  only loads 22 vec4 (88 words) into `sa` and the array holds 32 vec4
  (128 words). Every one of these reads hits unloaded registers, and many go
  past the end of the array.
- On OpenGL (replayed) every output UV equals `aUV` exactly, i.e. those reads
  return 0. On NVIDIA Vulkan, private arrays aren't zeroed and out-of-range
  reads return whatever is there, hence offsets that change per frame.

Fix (confirmed in game): register banks are zero-initialised, and indexed reads clamp
the index into range and return 0 when it was out of range. Shader cache
version bumped to 20. Open question: whether `idx * 2` is the right scale for
indexed SA access (with `idx + 14` the reads would land exactly on uniform
buffer 0); OpenGL looks correct with zeros, so the scale was left alone.

### Ideas not yet tried
- Instrument the translated vertex shader to write the computed `LDR`
  address and loaded value to a debug buffer, read it back, and compare with
  the guest memory at the address real hardware would use.
- Compare the SPIR-V generated for one stage shader under the Vulkan target
  with the SPIR-V/GLSL generated for the OpenGL target.
