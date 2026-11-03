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

## Open: stage textures misaligned on Vulkan (NVIDIA RTX 2060, Linux)

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

### Ideas not yet tried
- Instrument the translated vertex shader to write the computed `LDR`
  address and loaded value to a debug buffer, read it back, and compare with
  the guest memory at the address real hardware would use.
- Compare the SPIR-V generated for one stage shader under the Vulkan target
  with the SPIR-V/GLSL generated for the OpenGL target.
