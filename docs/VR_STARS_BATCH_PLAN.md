# The night sky stars as a few draws (plan, 2026-11-20)

Built 2026-11-20 (interp.cpp `stars_batch`, `interp_vr_star_set_color`; weather_quads.cpp `hook_02581060`), not yet
run: the compare mode's headset run comes first. The offline unit test of step "Tests" was not written: the builder
reads the game's own tables and constants from guest memory, so the compare mode in the running game is its check. The plan for taking the night sky stars off both the game thread and the render thread in VR, from
the function profiles of the Forsaken Fortress at night (stage MajyuE, weapons test, Capped 90, PS VR2).

## Why

At night in the Forsaken Fortress the logic frame misses 90 by about 1 ms (9.2-9.5 ms of game-thread work plus
2.3-2.65 ms waiting for the render thread, against 11.1 ms). The stars are the largest single part we can remove:

| Thread | Stars' cost | Where it goes |
|---|---|---|
| Game | 13.1% of the thread before culling (about a quarter of its work), 9.6% with culling | `dKyr_drawStar` (02574144) through our hook: per star a projection set up and made, two triangles written and flushed, then two draws issued in `dKankyo_star_Packet::setColor` (02581060) |
| Render | `weather_flush` 11.4% (about 1.5 ms a frame) | the weather quads' run merging (gx2_core.cpp "weather quads"): about 4,200 quads a frame, each with its own vertex buffer and fetch shader copy, copied into a ring and merged into runs of at most 256 (about 18 runs and 206 single draws a frame) |

Since 2026-11-13 (acd955e9) VR keeps every star in the sky (about 2,000 a frame); culling to the headset's view
(2026-11-20, `WWHD_VR_STARS_CULL`) leaves out 56-58% of them but only took the game thread's share from 13.1% to
9.6%: the per-star work before the cull test (the projection set up for each star) stays.

## The game's star drawing (verified decomp, `d_kankyo_rain_drawStar.cpp`, `d_kankyo_wether.cpp`)

- `dKankyo_star_Packet::draw` (in the frame's draw lists, every pass) calls `dKyr_drawStar(view, texture)`.
- `dKyr_drawStar`: one billboard triangle around the camera's eye from the view's inverse rotation, the packet's
  rotation and a size that shrinks with the field of view; then for each of `mEffectNum` stars: its colour kind
  (stars 0-15: the constellations, `ConstellationPositions`; the rest on a spiral from `SinCosTable`, a size
  from its index and the packet's twinkle `mSin`), its position projected, kept when inside the flat screen and
  away from the moon, two mirrored triangles written into the star's own double-buffered geometry, flushed, and
  `mDrawn[i]` set; at the end `dKankyo_star_Packet::render` (0258115C).
- `render`: depth off, blending on (alpha mode 3), matrices set for 8 renderers (one per colour kind), the
  weather shader bound, then `setColor(i)` for each colour: the renderer bound, and for every drawn star of that
  colour two `DrawIndexed` of 3 indices, each half with its own vertex buffer (element + 0x158, stride 0x4A8).

## The plan

The same pictures, from the game's own shader, render state, renderers and order; only the per-star work and the
per-star draws change. In VR only (`WWHD_VR_STARS_SKY` on); flat play and `WWHD_VR_STARS_BATCH=0` keep the game's own.

1. **Read first (offline, no headset):** the star geometry's layout from the decomp headers and
   `prepare_weather_pair`: attribute buffer 0 (positions: 3 vertices, its stride), whether the texture
   coordinates are a second, shared attribute buffer or per vertex, the fetch shader the pair uses, the index
   buffer (`weatherTriangleIndices`), and what each colour's renderer carries (its colour constants).
2. **Native star builder:** our code does `dKyr_drawStar`'s loop in plain C++ with the game's single-precision
   operations in the decomp's order (positions, sizes, twinkle, colour kinds), the cull test first (a star outside
   the view costs one dot product), and writes the kept stars' triangles for each colour into one buffer of ours
   in guest memory (a ring of several frames, so no frame overwrites vertices the renderer has not read; one flush
   a colour). The moon's gap (0.0 degrees in the logs since 2026-11-13) is computed here too.
3. **Batched draws:** `render`'s set-up runs as the game's; in place of `setColor`'s per-star draws, each colour
   is one draw: its renderer bound, attribute buffer 0 pointed at that colour's part of our buffer, and all its
   triangles drawn at once (an index buffer 0..N of ours, or a non-indexed draw if the fetch shader allows).
   8 draws a frame in place of about 1,700-4,000; the weather run merging no longer sees the stars.
4. **Compare mode** (`WWHD_VR_STARS_CHECK=1`): the game's loop also runs, and every kept star's vertices are
   compared with ours; the log gives the largest difference (it must be 0) and any star kept by one and not
   the other. The game's pictures are drawn in this mode.
5. **Proof in the log, every 10 s:** stars kept and culled, draws issued, the builder's ms a frame on the game
   thread, and the weather quads the renderer still merges.

### Step 1 findings (2026-11-20, from the decomp, offline)

| What | Found | Where |
|---|---|---|
| Stars | at most 2,000 (`mGeometry[2000]`, `mActive[2000]` at 0xD4, `mColorIndex[2000]` at 0x49A608); `mEffectNum` (0xCC) a frame from `mStarCount` (`dKyr_star_move`) | `d_kankyo_wether.h`, `d_kankyo_rain.cpp` |
| A star's geometry | `WeatherStarGeometry` (0x968 bytes): 4 elements = [buffer side 0/1] x [half 0/1], `mBufferIndex` (the side the game writes this frame; the draw reads the other) | `initializeGeometry` (d_kankyo_wether.cpp 3407) |
| Its vertices | each element 3 vertices in 0x24 bytes: positions only, 12 bytes each (`prepare_weather_element(..., 3, 0x24)`); no per-vertex texture coordinates; one attribute layout for all stars (`weatherStarAttributes`, 10050220) | same |
| Its draw | per half: `weatherBindGeometryMaterial(element + 0x158)` (027BFE5C: attribute buffer 0 and the element's copy of the fetch shader), then `GX2DrawIndexedEx` (C0006178) with `weatherTrianglePrimitive`, 3, `weatherTriangleIndexType`, `weatherTriangleIndices` (104B4A88 / 104B4A84 / 104B4A8C), base vertex 0, 1 instance | `setColor` (1914) |
| Colours | 8 renderers, each its colour in `mExtendedTexture.mParameters.mPrimaryColor` from `weatherStarPalette` (10050060); `setColor(i)` binds renderer `i` (027FE118) and draws that colour's stars in star order | `initializeGeometry`, `setColor` |
| Render state | `render` (0258115C): depth off, blending on (alpha mode 3), the 8 renderers' matrices, the "star" weather material's shader, then `setColor(0..7)` | `render` |

What this means for steps 2-3:
- Joining stars is already proven: the renderer's weather run merging draws star halves together by copying only
  attribute buffer 0's contents one after another and stitching the indices (gx2_core.cpp, `weather_flush`), with
  the first half's fetch shader. A batch of ours is the same thing done once, on the game thread, before the draws.
- `weatherBindGeometryMaterial` is itself the profile's third game function by self time (0.8-1.2%): one call a
  star half today, one a colour after.
- The kept stars' triangles go into one buffer in star order and half order within a colour, so overlapping
  stars blend in the same order as now.
- Our buffer needs the renderer to see each frame's writes as the game's flush does (the same invalidate the
  game's `flushBuffer` makes), and a ring of several frames, as `weather_flush`'s own ring has.
- Still to read when building: the three index globals' values (expected 0, 1, 2 as a triangle list of 16-bit
  indices: logged once to confirm), and that every star element's fetch shader bytes are the same (the renderer's
  merging already checks this per run; the builder logs it once).

**Why not a shader of our own (all stars in one instanced draw):** it would need a new pipeline inside the game's
own render pass and a copy of the weather shader's colours and blending, and what it saves over step 3 is small:
step 2 leaves plain arithmetic for about 1,000 stars on the game thread and step 3 leaves 8 draws.

## What it can and cannot do

- Upper bound, from the profiles: most of the stars' 9.6% of the game thread (about 3 ms a game step) and most
  of `weather_flush`'s 11.4% of the render thread (about 1.5 ms a frame, the stars being about 4,000 of its 4,200
  quads). Measured after, not promised.
- Not touched: the game's objects (`fapGm_Execute`, about 21% of the game thread), model drawing, collision, and
  the renderer's own per-draw cost (`draw_stereo`, about 49% of the render thread).

## Tests

Offline: the builder's arithmetic against the decomp for fixed inputs (a unit test: positions, sizes and colour
kinds for all star indices and a range of `mSin`, `mRot` and field of view). In the headset, once: compare mode on,
the weapons test at Capped 90 in the Forsaken Fortress at night, the log's difference 0, then the stars looked at
(all directions, near the moon), the frame times compared with the culling run.
