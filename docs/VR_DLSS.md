# NVIDIA DLSS and DLSS 5 Neural Rendering in the Vulkan VR renderer (2026-11-01)

What NVIDIA and the community offer, what this renderer can feed them, what is built (stage D1), and what is left.
Everything here was checked against the repositories, the driver on the test PC and runs of the game; where
something could not be checked it says so.

## Summary

- **DLSS Super Resolution and DLAA: available on our Vulkan device.** Confirmed on the RTX 5080 (driver 621.01):
  NGX starts on the game's own `VkDevice` and reports `SuperSampling.Available = 1`, feature init `0x00000001`. It
  needs NVIDIA's `nvngx_dlss.dll` next to the program and four device extensions (enabled now on NVIDIA GPUs).
- **They are not offered yet:** the renderer has no motion vectors and no projection jitter. Without them DLSS
  ghosts everything that moves; the settings do not show a choice that would look wrong.
- **DLSS 5 Neural Rendering: not possible with public components today.** The driver knows the feature
  (`DLSSNR.*` capability queries) but reports it not found (`0xBAD00004`): its model, `nvngx_dlssnr.dll`, is in no
  official NVIDIA package (DLSS SDK 310.9.16, Streamline 2.14.16, driver 621.01 and its downloaded models were all
  checked), and its API is not public (NGX feature 18 is `Reserved18` in the SDK headers).
- **Nothing on screen changed:** the frame-4200 picture is byte-identical with NGX started and the extensions on.

## 1. Official NVIDIA sources reviewed

| Source | Version checked | What it has for us |
|---|---|---|
| [NVIDIA/DLSS](https://github.com/NVIDIA/DLSS) | v310.9.16 (2026-10-16) | NGX headers, `nvngx_dlss.dll` (SR/DLAA), `nvngx_dlssd.dll` (Ray Reconstruction), `nvngx_dlssg.dll` (Frame Generation); Windows x64/arm64, Linux x86_64/aarch64 (`libnvsdk_ngx.a`, `libnvidia-ngx-dlss.so.310.9.16`). Vulkan API in `nvsdk_ngx_vk.h`. Feature 18 is `NVSDK_NGX_Feature_Reserved18`; the Linux loader object does contain the string `DLSSNR.Hint.Render.Preset`. |
| [NVIDIA-RTX/Streamline](https://github.com/NVIDIA-RTX/Streamline) | v2.14.16 (2026-10-16) | Declares `kFeatureDLSS_NR = 1004` (reported on NVIDIA's forum), but its 511-file package has no NR plugin, header or `nvngx_dlssnr.dll`. Streamline is Windows-only. |
| [nvpro-samples/vk_streamline](https://github.com/nvpro-samples/vk_streamline) | read about | Streamline on Vulkan; a `USE_DLSSNR` switch that needs a beta NGX SDK (per forum reports). |
| [NVIDIAImageScaling](https://github.com/NVIDIA/NVIDIAImageScaling) | read about | NIS: spatial, any GPU, Vulkan shaders, MIT. |
| [Omniverse ovrtx DLSS-NR](https://nvidia-omniverse.github.io/ovrtx/sensors/cameras/render_modes/dlss_nr.html) | read | NVIDIA's own DLSS 5 use: experimental, Windows, Blackwell, driver 615.64+, inputs image + depth + motion vectors + history, LDR output; no public API. |
| NVIDIA developer forum | read | Several developers asking for DLSS 5 SDK access (Sept-Oct 2026); no public answer. |

**License (RTX SDKs license):** the SDK's binaries may be distributed inside an application with "material
additional functionality"; the SDK may not be made subject to an open-source license. This project is MPL-2.0, so
**no NVIDIA header, library or DLL is in the repository**: the module declares the handful of entry points it calls
and loads the driver's NGX core at run time. `nvngx_dlss.dll` is put next to the program by whoever builds a
release (not committed; see "Packaging").

## 2. Community projects reviewed (source and READMEs; no binaries run)

| Project | API path | What it does for VR | License | Usable here |
|---|---|---|---|---|
| [tig3rmast3r/OptiScaler_DLSSNR_VR](https://github.com/tig3rmast3r/OptiScaler_DLSSNR_VR) (pushed 2026-10-26) | D3D12 only (`DlssNrFeature_Dx12.cpp`) | One NR feature, resources and history per eye: the game's two alternating DLSS handles are told apart. | GPL-3.0 | Technique only (independent per-eye features, done here independently). No code: GPL. |
| [Dagherbou/OptiScaler_DLSSNR](https://github.com/Dagherbou/OptiScaler_DLSSNR) (parent) | D3D12 and **native Vulkan** (`DlssNrFeature_Vk.cpp`) | Proves the NR model accepts Vulkan resources on the game's device (color, depth, motion vectors, optional exposure). | GPL-3.0 | Not as is: it loads `nvngx_dlssnr.dll` directly through a forwarder DLL built to pass the model's caller check, and drives a parameter block found by probing. |
| [lowpoplabs/nr-bridge](https://github.com/lowpoplabs/nr-bridge) | OpenXR layer, own D3D12 device | Takes both eye images at `xrEndFrame`, motion vectors from eye poses (rotation only), writes back. Work scale and fovea tiers to afford it. | MIT | Ideas only: pose-only vectors and a second device are not acceptable in our path. |
| [numsu/dlss-bridge](https://github.com/numsu/dlss-bridge) | intercepts the game's NGX call, private D3D12 session, same or second GPU | Linux through Proton validated; native Linux planned. | not stated | No: needs a game that already calls DLSS, and adds a device. |
| [NIGos/dlss5-bridge](https://github.com/NIGos/dlss5-bridge) | ReShade add-on, Vulkan to D3D12 shared textures | Optical-flow motion vectors for games without DLSS (lower quality). One pipeline bubble a frame, two validation errors by design. Warns of a malicious unofficial installer site. | MIT | No (cross-API, ReShade). |
| [jlrouzies-fr/DLSS5-Feeder](https://github.com/jlrouzies-fr/DLSS5-Feeder) | private D3D12 device | Synthetic DLAA contract from ReShade depth/motion. | — | No. |
| OpenDLSS-NR ([maanHimself/OpenDLSS-NR](https://github.com/maanHimself/OpenDLSS-NR)) | Vulkan compute + PTX | Reimplements the NR network; NVIDIA's weights not included. 7.8 ms at 1080p, 29.3 ms at 4K on an RTX 4070 SUPER (network only). | — | No (needs NVIDIA's weights from elsewhere); its timings are the best public cost estimate. |
| [maanHimself/OpenDLSS-NR](https://github.com/maanHimself/OpenDLSS-NR) (README read) | Vulkan: GLSL cooperative-matrix FP8 kernels, PTX fast route | The 71-block network of DLSS-NR 310.8.0, bit-exact; inputs: an LDR proxy, three noise lanes, the previous output reprojected (motion vectors), five scalars; output an RGB residual and a blend logit, same size (not an upscaler). Needs NVIDIA Ada+ (`VK_NV_cooperative_matrix2`, `VK_NV_cuda_kernel_launch`, `VK_EXT_shader_float8`). | MIT | No: "Nothing in this repository produces" the weights; they come from NVIDIA's model. |
| [danielblnc/DLSS-NR-on-AMD](https://github.com/danielblnc/DLSS-NR-on-AMD) v0.8.0 | D3D12 only (Vulkan, Linux planned), HIP kernels | NVIDIA's networks reimplemented for RX 6000-9000; the user's own `nvngx_dlssnr.dll` 310.8+; 90-100 fps at 1080p on an RX 9070 XT. | see repo | No: needs the model, D3D12 only, and DLSS's model on another vendor's GPU is outside NVIDIA's license. |
| [A-ENTROPY/magpie-dlss5-amd](https://github.com/A-ENTROPY/magpie-dlss5-amd), DLSSNR-SYCL-Bridge, dlssnr-amd-lab | AMD backends for the NR model | Research forks of the same idea. | GPL-3.0 / MIT / none | No (same reasons). |
| [mauri870/dlssnr-pytorch](https://github.com/mauri870/dlssnr-pytorch) | PyTorch | A reference implementation of the network. | MIT | No (offline, needs the weights). |
| [NapXDD/addon-dlssnr-linux](https://github.com/NapXDD/addon-dlssnr-linux) | ReShade add-on on Linux | The model through Proton/ReShade. | GPL-3.0 | No. |
| `dlssnr-patcher`, modded DLLs, installers | — | Patch the model for older GPUs. A copy of the model offered for this project (from a ReShade/RenoDX mod folder) failed Windows' signature check (`HashMismatch`) and was not installed. | — | No: modifying or redistributing NVIDIA's binary. |

## 3. Comparison

| | DLSS SR | DLAA | DLSS 5 NR | NIS | Streamline | OpenXR NR layer | OptiScaler VR techniques |
|---|---|---|---|---|---|---|---|
| Purpose | temporal AI upscale | AI anti-aliasing at 1:1 | generative relighting/detail | spatial upscale | framework | post-process headset images | per-eye NR in injected games |
| Vulkan | yes (NGX VK) | yes | model accepts VK (community-proven), no public API | yes | yes | no (D3D12) | VK in parent fork |
| Linux | yes (SDK .so) | yes | unknown | yes | no | no | Proton only |
| Windows | yes | yes | yes | yes | yes | yes | yes |
| VR stereo | per-eye features | per-eye | per-eye (community) | per eye, stateless | per-viewport | yes | yes |
| Multiview | scene unaffected; DLSS runs per eye after it | same | same | same | same | n/a | n/a |
| GPU | RTX 20+ | RTX 20+ | RTX 50 (officially) | any | RTX | RTX 50 | RTX 50 |
| Driver | 470+ (reported by NGX here) | same | 615.64+ (Omniverse) | any | per feature | 591.86 verified | 616.56+ |
| Runtime parts | `nvngx_dlss.dll` + driver NGX core | same | `nvngx_dlssnr.dll` (not public) | shader only | sl.*.dll | NR model | NR model + forwarder |
| Motion vectors | required, all motion | required | required | no | — | pose-only | game's |
| Depth | required | required | required | no | — | no | game's |
| History | per eye, jitter needed | per eye | per eye | none | — | per eye | per eye |
| Extra memory | ~2 eyes x output-sized internals (tens of MB) | same | model ~140-160 MB + per eye | none | — | — | — |
| Cost (est.) | ~1-2 ms per eye at 2736x2790 on an RTX 5080 (not measured) | more than SR | ~8-12 ms per eye at full eye size (extrapolated; over the 11.1 ms frame) | <0.5 ms | — | — | — |
| License | RTX SDK (binary redistributable) | same | none granted | MIT | MIT + binaries | MIT | GPL-3.0 |
| Maturity | production | production | experimental, unofficial | production | production | experimental | experimental |
| In-renderer | yes | yes | yes once public | yes | yes (Windows) | external layer | injection |

**Recommendation:** native NGX inside our Vulkan renderer, per eye, at the point where each eye's picture is copied
to its OpenXR swapchain. DLSS SR/DLAA first; DLSS 5 NR as a separate toggle the day NVIDIA publishes its API and
model (the same per-eye features, created through the driver's NGX core, with no forwarder). No second device,
no cross-API copy, no OpenXR layer, no ReShade, no GPL code.

**[Kizzuwatnaa/DLSS5-Autopilot](https://github.com/Kizzuwatnaa/DLSS5-Autopilot)** (MIT, pushed 2026-11-01, reviewed
from its README and `core/sources.py`, `core/components.py`): an installer that picks one of eight injection routes per
game and fetches the parts at run time; not a renderer. What it does for Vulkan games: NIGos' bridge (the game's DLSS
mirrored onto a private D3D12 session) or the feeder (a DLAA contract from ReShade's depth and shader motion vectors,
also on a private D3D12 device); no native Vulkan neural rendering. Its DLSS 5 model comes from `RankFTW/rhi-repo`, a
third-party GitHub account re-hosting it (RTX 50: "310.8.0, NVIDIA's own"; RTX 40/30/20: community re-targeted
builds); only SR, RR and FG come from NVIDIA's repository. VR: its OpenXR route (ReShade's OpenXR layer) is "unverified
with a headset". Nothing to take as code; three things worth taking as ideas:
- **On driver 616.64 and newer the driver's NGX runtime serves neural rendering itself** (its routes reach it through
  the renodx add-on): the official path this renderer uses (the driver's core, feature 18), no forwarder.
- **A model-resolution dial (25-100 %, cost falls with the square):** at 2736x2790 per eye this is what can bring NR
  near the 11.1 ms frame (D5: run NR on a reduced copy of each eye, the result upscaled back), a `dlss5.*` setting.
- **Checking the model DLL against the GPU before using it** (it reads the DLL's CUDA fatbin records for the card's
  architecture): a clear "this copy of the model does not run on this card" instead of an NGX failure code, for any
  copy a player puts beside the game (D5).

**AMD GPUs:** DLSS of any kind (SR, DLAA, NR) is NVIDIA-only under NVIDIA's terms; the AMD projects above all need
NVIDIA's model and none runs on Vulkan. The fitting choice for AMD is AMD's own FidelityFX upscaling (FSR 3.1: open
source, Vulkan; FSR 4's ML model on RX 9000): AI upscaling, not neural rendering, on the same per-eye stage once motion
vectors exist (stage D4). The native renderer stays the default on AMD.

## 4. The renderer, as audited

- **Devices:** `backend.cpp` creates the instance and device; in VR the OpenXR runtime creates both
  (`xrCreateVulkanInstanceKHR` / `xrCreateVulkanDeviceKHR`, `vr_vulkan.cpp` `create_instance` / `create_device`)
  from our lists plus its own. Vulkan 1.3 requested.
- **Multiview:** eye targets are the two layers of one image; replayable draws go to both with view mask `0b11`
  (`docs/VR_MULTIVIEW.md`). Unaffected by DLSS, which runs after the scene, on each eye's finished picture.
- **Eye pictures:** the game's TV scan-out per eye (`R.tv.scan` and `stereoRight.scan`) at the internal
  resolution (render scale, dynamic resolution, the Quest's 0.75x), blitted to the eye's swapchain image (2736x2790
  on the PS VR2 here) in `vrvk::record_eyes`. **This blit is where DLSS goes:** the input is the scan (internal
  size), the output the swapchain size; NR fits there too (it works on the final LDR picture, as in Omniverse).
- **Depth:** each eye's depth exists in the game's own depth targets at the internal size (Vulkan 0..1, not
  reversed); the scan-out is a copy of the colour target after the game's post-processing.
- **Motion vectors: none.** The game draws at 30 Hz logic and the in-between frames re-run its draw code with
  blended actor matrices (`interp_fx.cpp`). Every transform reaches the renderer as GX2 uniform constants written
  anew each frame; nothing ties a draw to its previous frame.
- **Jitter: none.** Each eye's projection is shifted on the CPU (`stereo::project_eye`), which is where a jitter
  would also go.
- **TAA / upscaling:** none of our own; the game's bloom and filters run in its own shaders.
- **Threads:** the render thread records; scene drawing threads (S6) record draws; submission on the render
  thread; NGX evaluate would be recorded on the render thread at the eye copy.

## 5. Built now (stage D1)

- `runtime/src/gfx/vulkan/ngx_core.h`: the decisions, NGX-free and tested: modes and their render scales, which
  choices may be shown (Off only until the driver has the feature **and** the renderer gives depth, motion vectors
  and jitter), the DLSS 5 toggle kept apart from the modes, the status line with reasons, the parameter object's
  virtual slots (MSVC layout on Windows, Itanium on Linux), and the per-eye feature set (one feature and history per
  eye; remade on size or mode change; a failed create falls back to native for both eyes; released at shutdown;
  forgotten, not released, after a lost device).
- `runtime/src/gfx/vulkan/ngx_vk.cpp`: the probe. Finds the driver's NGX core (`_nvngx.dll` from the driver's
  registry keys; `libnvidia-ngx.so.1` on Linux), `NVSDK_NGX_VULKAN_Init_Ext2` on the game's device,
  `GetCapabilityParameters`, reads `SuperSampling.*` and `DLSSNR.*`, logs, and keeps NGX started. Not on Android or
  macOS. `WWHD_NGX=0` turns all of it off.
- `backend.cpp`: on NVIDIA GPUs offering them, `VK_NVX_binary_import`, `VK_NVX_image_view_handle`,
  `VK_KHR_push_descriptor` and `bufferDeviceAddress` are enabled (without them DLSS's own init fails, `0xBAD00002`).
- The performance report has the line, e.g. `NVIDIA DLSS on NVIDIA GeForce RTX 5080 (driver 621.01): Super
  Resolution / DLAA: available; DLSS 5 Neural Rendering: not available (0xBAD00004); not offered yet: the renderer
  has no motion vectors, projection jitter; active: Off`.

**Found on the way:** a shutdown of NGX right after its initialisation crashed once in the driver (a write through
a null pointer inside `_nvngx.dll`); NGX starts an update check of its own at init. NGX now stays started.

## 5b. Stage D2a done: draws matched across frames

`motion_match.h` (tested by `motion_match_test`) keys a draw by its shaders, first vertex buffer, indices, counts and
primitive, and matches this frame's draws of a key to the previous frame's in order. `WWHD_VK_MOTION_STATS=1` logs, every
600 frames, how the 3D draws (a vertex block holds the camera's projection) match. Desktop, `busy` state, 2,400 frames:

| window | draws / 3D | matched by the full key | without the vertex buffer in the key | without the indices | without both |
|---|---|---|---|---|---|
| 1 | 510 / 322 | 34.4% | 99.4% | 34.4% | 99.4% |
| 2 | 301 / 258 | 59.0% | 99.6% | 59.0% | 99.6% |
| 3 | 853 / 508 | 63.2% | 98.3% | 63.2% | 98.3% |
| 4 | 1,228 / 747 | 45.8% | 99.4% | 45.8% | 99.4% |

The sea state gives the same picture. **What it means:** shaders, indices, counts and order identify about 99% of 3D
draws from one frame to the next; the vertex buffer's address moves every frame for about half of them (the game
writes their vertex data anew each frame). So stage D2b keys draws without the vertex buffer and, for each matched
draw, keeps the previous frame's vertex data and uniform blocks: the previous clip position is the previous transforms
applied to the previous vertices. This one per-draw path covers the camera, the head and moving objects; no separate
camera reprojection is needed. Only 0-5% of matched draws have unchanged transforms (the view is in every draw's
matrices), so every 3D draw needs its previous transforms.

**D2b, next:** the translator emits a variant of each 3D draw's vertex shader that also runs on the previous frame's
uniform blocks and vertex buffer (a second binding of each) and writes `prev clip - current clip` to a motion target
added to the scene passes (both eyes, multiview included); colour output unchanged byte for byte; draws with no match
write zero motion and are counted.

## 5c. Stage D2b, step 1 done: the motion variant of every shader, built and compiled (not used yet)

`motion_glsl.h/.cpp` (tested by `motion_glsl_test`) rewrites a translated shader the way the multiview variant does:
declarations only, plus a new `main` around the translator's.

- **Vertex:** every uniform block (guest buffers `uniformBlockVS<i>`, the support block `ufBlock`) becomes an array of
  the current frame's data and the previous frame's, chosen by `wwhd_mframe`; every vertex input gets a second input
  at location + 16 for the previous frame's vertex buffer. The translator's main runs on the previous frame's data
  (its `gl_Position` kept), then on the current frame's (every output as the original writes it). Both clip
  positions go to the pixel shader at locations 30 and 31. On a multiview draw the arrays hold four, `[frame * 2 +
  gl_ViewIndex]`, on top of the multiview variant's eye textures.
- **Pixel:** the translator's main, then `wwhd_motion = (prev.xy / prev.w - cur.xy / cur.w) * 0.5` at the location
  after the shader's colour outputs; a discarded pixel writes none.

`WWHD_VK_MOTION_VALIDATE=1` builds and compiles each new shader's variants. Desktop, `busy` state, multiview on:
**1,300 variants compiled, 0 failed** (650 shaders, single view and multiview). The first try failed half of them (a
stray semicolon after the vertex-input macros); the test checks for it now. The picture is unchanged.

**Found for step 2:** the device allows 15 dynamic uniform buffers a set. A draw's blocks doubled (and doubled again
for multiview) can pass that: the previous frame's data needs its own non-dynamic binding (one buffer, offsets in a
push constant) rather than a second dynamic block each.

**Step 2:** bind it. Per matched draw (motion_match.h, keyed without the vertex buffer), keep the previous frame's
uniform bytes per eye and its vertex data; bind them as the variant's previous-frame elements; add a motion target
(R16G16 float, per eye) to the scene passes and the motion output to their pipelines; unmatched draws and draws not
in the scene write zero. Colour output byte-identical.

## 5d. Stage D2b, step 2a done: the previous frame's store, and motion work only with DLSS chosen

- **Only with DLSS chosen:** every piece of motion-vector work (the variant validation, the match counting, the store
  to come) runs only while `dlss` is not `off` or `dlss5` is 1 in `settings.ini` (`ngx_vk::motion_wanted`); with both
  off, `WWHD_VK_MOTION_VALIDATE` and `WWHD_VK_MOTION_STATS` do nothing. Checked both ways on the desktop: none of their
  lines with the player's settings (DLSS off); with `dlss5=1` in a copy, 500 variants compiled, 0 failed, and the
  match lines as before.
- **The store** (`motion_store.h`, tested by `motion_store_test`): CPU copies of each draw's bytes per eye (uniform
  blocks and support blocks of both stages, vertex buffers), because upload arenas are reused every frame. The render
  thread numbers the frame's draws in game order and keys them (without the vertex buffer); a drawing thread fills only
  its own draw's slot, so recording takes no lock; `prepare_lookups` matches the frame against the previous one before
  the draws are recorded, so each finds its previous data while it is drawn. A 64 MiB budget, first draws kept.

**Step 2b done (uniform data; vertex data still to come):** the store reuses the scene split's draw number
(`t_drawIndex`, set on the render thread in `scene_split_before_draw` and handed to the drawing threads); the render
thread keys each draw right after it is numbered (a held run's pieces each), and the store finds the previous frame's
draw at once (the k-th draw of a key, as the match does). The uniform upload (`draw.cpp`, the `uniform` lambda) copies
every block and support block it uploads into the draw's slot for its stage and eye; the store turns over at the scene's
frame end (`scene_split_frame_end`, after the drawing threads have joined). `[motion] store:` every 600 frames.
Desktop, `busy` state, DLSS 5 chosen (a settings copy): 264-905 draws a frame, **98.8-99.8% found their previous
frame's draw**, 0.30-0.90 MiB kept, nothing over the budget or past a frame's room. Render thread 5.64 ms a frame
against 5.16 with DLSS off (after a block copy replaced a byte loop, which cost 1.15 ms; the runs differ in their
draws, 974 against 826 median); 86.3 fps against 86.0. With DLSS off: no motion line, picture byte-identical.
**2b finished (vertex data):** at the vertex binding each draw's buffers go into its slot too: one the buffer cache
serves (a mesh that stays where it is) by its address and size only, one uploaded fresh (written again every frame,
its address moving) as a copy of the bytes the draw reads. The pixel stage's uniforms are no longer kept: the motion
variant computes the previous position in the vertex shader only. Desktop, `busy` state, same route both ways: with
DLSS chosen 266-890 draws a frame, 98.8-99.8% found their previous frame's draw, 0.28-0.83 MiB kept; render thread
5.52 ms against 5.51 with DLSS off, 86.1 fps against 85.3 (no measurable cost); picture byte-identical.

**Step 2c, first part done: every pipeline's motion twin, built (not drawn with yet).** `motion_pipeline.h/.cpp`, the
multiview twins' way (`multiview_pipeline.h`): the guest pipeline's state with `add_motion_inputs` (every vertex input
again at location and binding + 16) and `add_motion_target` (an R16G16 float attachment, never blended, after the
colour attachments and after the pixel shader's own outputs, a gap filled with unused `VK_FORMAT_UNDEFINED`
attachments), both stages' motion variants, the vertex stage's uniform bindings with two descriptors (the layout's
dynamic uniform buffers checked against the device's 15), built on a thread of its own. `WWHD_VK_MOTION_PIPELINES=1`
with a DLSS feature chosen builds one per new pipeline; shader text is kept while a DLSS feature is chosen. Desktop,
`busy` state: **400 built, 0 failed** (the first try failed 29%: depth-only passes' pixel shaders still declare location
0, which the motion output took; the test covers it now). `pipeline_state_test` covers both additions. Picture
byte-identical.

**Step 2c, second part: the motion pass instead of a motion attachment in the game's passes.** Adding a motion target
to the game's own scene passes would make every pipeline in those passes a twin (dynamic rendering needs each
pipeline's colour attachments to match the pass's) and would need passes restarted whenever a twin is not built yet.
Instead the motion vectors get **a pass of their own per eye after the scene**: the scene's depth-writing draws drawn
again with their twins, into the motion target only, the scene's depth tested EQUAL and never written. Only the visible
surface writes motion; cut-out pixels (foliage) are right without the game's pixel shader, because where a pixel was
discarded the scene's depth is the background's, not the draw's. The twin's vertex shader runs the translator's code
unchanged on the current frame's data, which gives the scene draw's depth exactly (the translator declares
gl_Position invariant). The game's passes are never changed, so the picture stays identical by construction.
The twins are now that: `make_motion_only` (one R16G16 float attachment, depth EQUAL, no depth writes, no stencil),
the motion variant's vertex shader, and one shared pixel shader (`motion_only_pixel`); a layout of the vertex stage's
set only. Desktop: **342 built, 0 failed, 58 without depth** (2D and post passes: nothing for the motion pass).
Picture byte-identical.

**Step 2d, first part done: the motion pass draws eye 0's motion vectors.** `motion_pass.h/.cpp` with
`WWHD_VK_MOTION_PASS=1` (and the twins, `WWHD_VK_MOTION_PIPELINES=1`, and a DLSS feature chosen). After each eye-0
draw that tests and writes depth, `motion_packet` (`draw.cpp`) builds a packet from the draw's replay record: its ready
twin, the vertex stage's set written again with both frames' elements (the previous frame's bytes from the store
uploaded again; the current frame's where there are none), the vertex buffers and the previous frame's at + 16 (a
cached mesh at the same address: the same buffer; copied bytes: uploaded again), the index buffer, counts, viewport
and scissor. At the frame's end (`swap`, before the store turns over) `motion_pass::record` picks the depth most packets
were drawn against, clears an R16G16 float target of its size, and draws the packets with the scene's depth tested
EQUAL. `WWHD_VK_MOTION_DUMP=<frame>` writes the target as a PNG and logs its statistics.
Desktop, `busy` state: 288 draws a frame in the busiest window (151 with the previous frame's data); frame 2000:
70.8% of pixels moving, mean 0.00015 of the picture, largest 0.0013; the picture shows Link (his hat, head and shield
moving most, his idle animation), the swaying palms, the boat and the shore line, static ground near zero: per-object
motion, not the camera's alone. **The colour picture is byte-identical with the motion pass on** (`return` state, 413
to 445 motion draws a frame). Left: eye 1 (and multiview draws), batched runs, why about 40% of packets have no
previous data, and the pass's cost.

**Step 2d, second part: full coverage, a race fixed, implausible motion dropped.**
- **Why packets lacked the previous frame's data:** almost all were cached meshes whose address moved since the
  previous frame (the game alternating between vertex buffers; 113-140 of 162-292 motion draws). The store now copies a
  cached mesh's bytes when its address differs from its previous frame's: **287 of 288 motion draws a frame have the
  previous frame's data**.
- **A race:** the sun atlas's and eye 1's threads run the draw code too and carry no draw number of their own; they
  wrote another draw's slot beside its own thread (latent since 2b; it crashed in memcpy once more bytes were copied).
  Only the render thread and the scene's drawing threads record now (`motion_records`); two runs clean.
- **Implausible motion is written as zero** (`motion_only_pixel`): the previous position behind the camera, not a number,
  or more than a quarter of the picture (`kMaxMotion`) in one frame. The horizon's sea strip, which the game rebuilds
  around the camera (vertex n of the previous frame is another point), had shown motion of up to 3.8 pictures; largest
  now 0.16, mean 0.00008, one small patch of it left within the limit.

**Step 2d, third part: both eyes.** Eye 1's packets use eye 0's record and data, with the eye-shifted rows of its blocks
(the record's EyeUniforms) made eye 1's in both frames' bytes (`apply_eye_rows`; the rows' unshifted values are the
camera's projection, the same in both frames): a multiview draw adds eye 1's packet against eye 1's layer of the depth
(`mv.depthTwin`), and eye 1's replay of eye 0's draw adds its own. Desktop stereo uses multiview draws too (824 draws
a frame for both eyes). `busy` state: 2 passes a frame, 575 motion draws, 573 with the previous frame's data; frame
2000: eye 0 69.0% of pixels moving, mean 0.00005, eye 1 69.2%, mean 0.00006, largest 0.0017 each. Colour picture
byte-identical with both passes on.

**All of it only with DLSS chosen (checked).** The twin pipelines, the motion pass, the store's recording and turnover,
the draw keying, the variant validation, the match counting and keeping shader text for the twins all test the DLSS
choice (`ngx_vk::motion_wanted`). Desktop with DLSS off and `WWHD_VK_MOTION_PIPELINES`, `_PASS`, `_VALIDATE`, `_STATS`
and `_DUMP` all set: no motion line in the log, no twin pipeline, no motion target written.

**The motion pass's cost** (desktop, `busy` state, DLSS chosen, the same route with and without the pass): render thread
6.49 ms a frame against 5.65 (about 0.85 ms: 575 draws recorded again at the frame's end, and the packets built on the
drawing threads), 84.9 fps against 86.1 (one window at 64 fps while twins were still compiling). To come down later: the
pass recorded on a thread of its own, packets without previous data skipping the descriptor rewrite.

**Stage D4, first part done: DLSS runs in the renderer (DLAA, both eyes, desktop).** `WWHD_VK_DLSS_EVAL=1` (with the
motion pass and a DLSS feature chosen). The probe keeps the driver core's run entry points (AllocateParameters,
CreateFeature1, EvaluateFeature, ReleaseFeature) when it ran on the game's device; `ngx_vk::evaluate` sets the
parameter object by slot, makes each eye's Super Resolution feature when its sizes or mode change, and evaluates it.
After each eye's motion pass, `run_dlss` (motion_pass.cpp) gives it the eye's picture (this frame's scan: the one being
built, which `swap` shows right after), the scene depth (a depth-aspect view), the motion target (`MV.Scale` = the
picture's size, `MVLowRes`) and a storage image the same size, then copies that back into the scan. Found on the way:
- a render target's own views may be made for attachment use only on NVIDIA (`attachment_view_usage.h`); DLSS samples
  its inputs, so it gets its own plain views of all three (a crash in the driver from `nvngx_dlss.dll` before);
- NGX records into the command buffer itself while the record worker replays into the same command buffer on its own
  thread: the worker is drained first (`drain_record_commands`), else the driver crashed;
- the scan shown is swapped in by `swap` after the motion pass: writing the shown one wrote the previous frame's.
Result (`return` reference, frame 4200, DLAA): 2.00 evaluations a frame, both eyes; about 22% of pixels changed
(largest channel difference 174-182, edges), the picture clean and correct. No jitter yet (D3), so DLAA cannot do its
full anti-aliasing; no upscaling yet (the internal resolution is still the output's).

**Stage D3 done: jitter.** With `WWHD_VK_DLSS_EVAL=1` each frame's scene draws are moved by a sub-pixel offset
(Halton 2, 3 over 16 frames, -0.5..0.5 pixels), added to their **viewport**, not the projection: the eye shift rewrites
only the projection's first row, and in packed support blocks its second row (y) can be another entry, while a viewport
offset shifts every vertex exactly as a jittered projection would, and Vulkan viewports take fractions. Only
depth-tested draws into an eye's screen-size target (HUD and 2D passes stay still); eye 1's replay keeps eye 0's
viewport and the motion pass each draw's, so both frames' positions in the motion vectors carry their jitter
(`MVJittered`), and the evaluate gets the frame's offset. `return` reference with jitter: 2.00 evaluations a frame,
the picture clean (edges sharp, no doubling). The jitter's sign as DLSS reads it is unverified on a still frame; motion
in the headset will show it (a wrong sign blurs and shimmers).

**Stage D4, second part done: DLAA from the settings.** `renderer_inputs()` now reports depth, motion vectors and
jitter (not upscaling yet), so the settings offer **Off and DLAA**; the upscaling modes stay hidden until DLSS's output
has its own path to the eye (the internal resolution is still the output's). Choosing DLAA (`dlss=dlaa`, in the
launcher's Graphics tab or settings.ini) turns everything on by itself: the twins, the motion pass, the jitter and the
evaluate (`ngx_vk::active_mode`); the `WWHD_VK_MOTION_PIPELINES`, `_MOTION_PASS` and `_DLSS_EVAL` switches remain
for tests (=0 turns each off). The status names the active mode (`active: DLAA`), `dlss-status.ini` lists
`offered=off,dlaa`. Desktop with only `dlss=dlaa` in a settings copy: the motion pass on, both eyes' DLAA features made,
evaluated every frame. `ngx_test` updated (no-motion and today's inputs, the offered modes, the active mode).

**Stage D4, third part done: upscaling (Quality, Balanced, Performance).** A mode's internal resolution is a dynamic
resolution level (`render_factor`: Quality 0.75, Balanced 0.625, Performance 0.5; Ultra Performance's third has no
level and is not offered), set as a DLSS factor that takes precedence over the menu's (`dynres::set_dlss`: the
settings load after the probe and set the menu's factor back to 1). The scene, its depth and the motion target are drawn
at it; with a DLSS upscaling mode the TV's and eyes' scans are made at the full internal resolution (`copy_to_scan`;
the resample fills them first), DLSS reads the colour buffer the scan was resampled from (`Screen::scanSource`) and
writes the scan at full size. The settings and launcher offer Off, DLAA, Quality, Balanced, Performance.
Desktop, `dlss=performance`: `[dynres] fixed 0.5x of the 3x internal resolution (3D view 1920x1080)`, both eyes
`DLSS Performance 1920x1080 to 3840x2160`, evaluated every frame; the picture clean and sharp (palm fronds, outlines,
the shield's detail, the HUD text). Desktop fps unchanged (83.7: the desktop runs are limited by the render thread, not
the GPU); the GPU saving shows where the GPU limits, in the headset. **The motion pass is needed for these modes too:**
motion vectors are a required input of DLSS Super Resolution and DLAA (DLSS 4 included), which align earlier frames with
them; without them everything moving (and the whole view as the head turns) trails.

**The DLSS step's cost cut, and the mod HUD kept.** Timers (`[motion] cost`) split the motion work: packets built
0.54 ms a frame (over the drawing threads), the pass recorded 1.10 ms, of which 0.79 ms was DLSS: almost all of it the
render thread waiting for the record worker to catch up with the frame (the drain NGX needed). The command stream now
carries a call (`PodCommands::call`, opcode 30; `DeferredCommands::call`; `record::call`): fn(cmd, a copy of its bytes)
run in order by whichever thread records the command buffer, so the worker itself runs NGX and nothing is drained.
After: the pass recorded 0.31 ms, DLSS 0.004 ms on the render thread; two runs clean, both eyes every frame. DLSS's
output is not bit-exact from run to run (two identical runs: 135,522 pixels differ by at most 8).
Found on the way: DLSS overwrote the mod HUD overlays drawn into the scan after the resample (climb stamina, boost bar);
while DLSS runs they are drawn after its picture (`run_dlss`), not in `copy_to_scan`. DLSS-off picture byte-identical.

**Packet building trimmed (fixes 1-3), and the jitter flag.** (1) A binding whose previous-frame bytes equal this
frame's (uniform blocks, copied vertex buffers) uses the current element as the previous one, nothing uploaded. (2) A
draw with nothing changed since the previous frame is left out of the pass: its motion is zero, which the cleared target
holds (`[motion] still draws`). (3) A multiview draw's eye 1 packet reuses eye 0's previous-frame uploads where nothing is
eye-shifted (`MotionShare`), and takes eye 0's still verdict. Counted on the desktop run, camera moving: of ~690 uniform
blocks a frame only ~41 are unchanged (the game bakes the view into its per-draw matrices), eye 1 reuses ~121 uploads,
and almost no draw is still; the packet timer stayed within run-to-run noise (0.54 before, 0.45-0.69 after). The gain is
for a still camera (frozen-state run: ~250 still draws a frame left out). Checked: DLSS-off picture byte-identical;
eye 0's motion image byte-identical with and without the still skip. Eye 1 differed at first: a shared block was not
compared, so a moving draw could be judged still; fixed (eye 1 never judges still itself when sharing), not re-run.
`MVJittered` is now off: the motion vectors are the difference of two vertex-shader clip positions and the jitter is the
viewport's, applied after, so they hold none (the flag tells DLSS to remove jitter that is not there). Unverified in the
headset.

**Next: D3 (jitter) and D4 (evaluate), together** (jitter without DLSS only makes the picture shimmer), behind
`WWHD_VK_DLSS_EVAL=1` until checked, DLSS chosen:
- **D3:** a Halton(2,3) sub-pixel offset per frame, the same for both eyes' frame, added to the camera projection where
  `project_eye` shifts it (x: `p[2] -= 2 jx / width`, y: `p[6] -= 2 jy / height`, width and height the eye target's),
  on scene draws only; the motion pass then sees both frames' jitter in its positions, so the evaluate passes
  `MVJittered` (NVSDK_NGX_DLSS_Feature_Flags_MVJittered).
- **D4:** NGX evaluate per eye at the eye copy (`vrvk::record_eyes`; the desktop's present), on the real command buffer:
  the renderer records through a deferred recorder replayed on a worker thread, and NGX records into the command buffer
  itself, so the evaluate is an owned action (`DeferredCommands::emit_owned`), run once in order at replay. Inputs:
  the eye's scan (colour after the game's post-processing), its scene depth, its motion target (scaled to pixels of the
  render size), the jitter, a reset on cuts (`EyeFeatures::cut`); the output an image at the swapchain's size, then the
  copy. Features per eye (`ngx_core.h EyeFeatures`), created through the driver's core with the parameter object set by
  slot (`param_slot`, MSVC layout on Windows).
- Then the settings offer the modes (`renderer_inputs()` from what is on), the launcher shows them, and a headset run.

**Step 2c-2e (next):**
1. **2c** the previous frame's data bound: one non-dynamic uniform buffer per draw with a push-constant offset (the
   15 dynamic buffers limit), a second vertex binding per buffer at + 16; the motion variant's pipelines.
3. **2d** the motion pass: during the scene, each depth-writing draw into the eye's scene depth recorded as a packet
   (its twin, the vertex set with both frames' elements and dynamic offsets, both frames' vertex buffers, its index
   buffer and counts, viewport and scissor); at the eye copy, per eye, a motion target (R16G16 float, the eye's size)
   cleared to zero and the packets drawn into it against the scene's depth.
4. **2e** checks: the colour pictures byte-identical with motion on; the motion target dumped and checked on known
   movements (head turn only, Link walking, the boat).

## 6. Checks

| Kind | What | Result |
|---|---|---|
| Automated | `motion_store_test` (ctest `motion_store`): per-draw and per-eye slots, lookups through the match, new draws, the budget | passes |
| Automated | `motion_glsl_test` (ctest `motion_glsl`): blocks per frame and view, inputs at + 16, main run twice, pixel output, refusals | passes |
| Desktop run | `WWHD_VK_MOTION_VALIDATE=1`, multiview on: every shader's motion variants | 1,300 compiled, 0 failed |
| Automated | `motion_match_test` (ctest `motion_match`): unique and repeated keys, order changes, new and gone draws | passes |
| Automated | `ngx_test` (ctest `ngx`): settings keys and the launcher's status file; unavailable, AMD, old driver, init failure, create failure, per-eye resources, independent eyes, resets, resolution and mode changes, release, lost device, native fallback | passes |
| Build | Windows (llvm-mingw) `wwhd`, `ngx_test` | builds; Linux and Android not built here (the probe is `#if`'d out on Android) |
| Desktop run | `--no-vr`, sea state, 400 frames, x3 | no DLL: SR `0xBAD00004`; with `nvngx_dlss.dll`: `0xBAD00002`; plus extensions: **SR available**; NR `0xBAD00004` throughout |
| Picture | frame 4200, `return` state, both eyes | byte-identical to the reference (`9dad4301ddbe79ad` / `7565d33d869d9e15`) |
| Automated | `motion_6dof_test` (ctest `motion_6dof`): stationary points under x, y, z, yaw, pitch, roll and combinations; parallax by depth, none at infinity; rotation equal to nr-bridge's rotation-only reprojection at infinity; eye shift equal to cameras at the eyes; the y sign; background, cut and scene-change decisions | passes |
| Headset | — | not run |

## 7. Stages left

1. **D2. Motion vectors.** Per draw, the previous displayed frame's transforms: identify a draw across frames
   (shaders, vertex and index buffers, order within that key), keep its uniform blocks, and have the translator
   emit a variant that also computes the previous clip position and writes the difference to a motion target in
   the scene passes. Exactness: the colour output must stay byte-identical (the motion target is extra). This is
   the large part.
2. **D3. Jitter.** A Halton sub-pixel offset per eye in `project_eye`, only while DLSS is on (DLAA/SR), passed to
   the evaluate call; frames without DLSS unchanged.
3. **D4. Evaluate.** Per eye at `record_eyes`: scan colour, the eye's depth and motion vectors in, an output image at
   swapchain size, then the copy. History reset on stage changes, cuts and teleports (`EyeFeatures::cut`). Settings
   rows in the VR menu (only the offered choices) and GPU timing.
4. **D5. DLSS 5 NR**: feature 18 through the driver's NGX core on the same per-eye features, its own toggle,
   measured against the 11.1 ms budget (likely only at a reduced work size). **The call is written (2026-11-01):**
   with `dlss5=1` the motion pass runs feature 18 per eye on the picture at the render size, with its depth and
   motion, before Super Resolution, which then reads DLSS 5's output (DLAA when no DLSS mode is chosen):
   `ngx_vk::evaluate_nr`, `motion_pass.cpp` `run_dlss`.
   - The `dlss5.*` settings go to the feature: preset, style, intensity, the tone and structure strengths, the
     automatic mask and iterations. The parameter names are the ones from section 10, since no NVIDIA header has them.
   - The driver decides whether it serves the feature. Nothing here loads, patches or checks the model.
   - If the feature cannot be made, the log says `[ngx] DLSS 5: the feature could not be made (0x...)`, DLSS 5 stays
     off for that run and Super Resolution goes on without it. On driver 621.01 the probe reports `0xBAD00004`.
   - Not yet run on a PC that serves it.

Frame Generation is out of scope (VR latency needs its own study).

## 7b. Settings and the launcher

`settings.ini`: `dlss` (`off`, `dlaa`, `quality`, `balanced`, `performance`, `ultra`) and `dlss5` (0/1). The game reads
them at its probe and runs Off for anything not offered (the status line names the choice: `active: Off (chosen: DLSS
Quality)`). After the probe it writes `dlss-status.ini` next to `settings.ini` (GPU, driver, what the driver offers,
which choices are offered, the status line). The VR launcher's Graphics tab has an "NVIDIA DLSS" section: the status,
a "DLSS mode" list with only the offered choices enabled, and a separate "DLSS 5 Neural Rendering" toggle, enabled only
when offered; each with the reason. Before the game's first start on a PC it says so.

## 8. Packaging

`nvngx_dlss.dll` (310.9.16, signed by NVIDIA, 58,956,912 bytes) from the SDK's `lib/Windows_x86_64/rel` next to
`wwhd.exe`; on Linux `libnvidia-ngx-dlss.so.310.9.16` next to the program. Not in the repository. A release that
ships it carries NVIDIA's license text (`nvngx_dlss.license.txt` in Streamline).

## 9. Headset test plan (when stage D4 exists; ask before each run)

At `WWHD_VR_FRAME_RATE=full` (Capped 90), heavy and light views from the play-run route, each mode against native:
average fps and the share of windows at 88+, DLSS GPU time per eye, internal and output size, memory. Look for:
the same sharpness in both eyes, no ghosting behind Link, enemies, the boat and water, no shimmer on grass, stable
image during fast turns, leaning and walking around; wrist HUD and controllers unchanged.

## 10. nr-bridge reviewed, and the 6DoF motion (2026-11-01)

**What nr-bridge is.** github.com/lowpoplabs/nr-bridge: MIT, 3 commits; its source was read in full, nothing built or
run. It is an OpenXR API layer (and an OpenVR proxy) for games it cannot see into (BONELAB, BONEWORKS). At each
`xrEndFrame` it:
1. copies both eye images to its own D3D12 device;
2. builds screen-space motion vectors from the head's rotation since the last frame;
3. runs NGX feature 18 (DLSS 5 Neural Rendering) on a downscaled copy;
4. adds the model's change back onto the full-resolution frame.

| Part | nr-bridge | Here |
|---|---|---|
| The model | `nvngx_dlssnr.dll` from the RenoDX Discord (in no NVIDIA package). When the driver refuses feature 18 (`OutOfDate`), it loads the model through a forwarder DLL named to get past the model's caller check | not taken: our spec forbids unofficial models, and the forwarder is a bypass. Feature 18 stays FeatureNotFound on driver 621.01, so NR stays off |
| Motion vectors | per pixel from the head's orientation and field of view, current vs previous; **translation taken as zero** (its README: "mild ghosting on very near objects"); no object motion | per vertex from the game's own vertex shader on both frames' data: head rotation **and translation**, locomotion, Link and every animated object, at each pixel's real depth (section 5d on) |
| Depth | a constant 0.5 (Unity gives it none) | the scene's real depth |
| Eyes | both eyes side by side in one work texture, one model evaluate | one DLSS feature per eye (`EyeFeatures`) with separate histories; each eye's own projection, depth and motion |
| History reset | on a pose reset or a settings change | added now: view cuts and new scenes (below) |
| Work scale, residual transfer, two-tier fovea, multi-pass | post-processing around the NR evaluate: downscale, run, add back the difference | not built. They exist to make the NR model affordable and need the model to tune or check; nothing in DLSS SR/DLAA uses them. Noted for when NVIDIA ships NR |
| Integration | D3D12 device beside the game, shared textures and fences | inside our Vulkan renderer: no copy between APIs, no second device, NGX on the record worker in the frame's own command stream (stage D4) |

nr-bridge also uses NR parameter names that are reverse-engineered, not taken from any NVIDIA header. They are kept
here as a record, not used:
- `DLSSNR.Color`, `.Depth`, `.MVec`, `.Output`, `.Width/.Height`, `.Reset`;
- `.Intensity`, `.Style`, `.LocalStructureStrength`, `.LocalToneStrength`, `.SkinStructureStrength`, `.UseAutoMask`;
- `.Hint.Render.Preset`, `.UICorrection`, the subrects, `MVecScale`.

**Why our motion is already full 6DoF.**
- The head's whole pose is the drawn camera: position and orientation from `xrLocateViews` at the frame's predicted
  display time, both eyes from the same call. `CameraOverride` writes it as the game camera's look-at every frame, and
  the game builds its view and projection from it (VR_CAMERA_AUDIT.md section 1; it is the only writer).
- So every draw's matrices hold the head pose. The motion pass runs each draw's vertex shader twice: once with the
  previous frame's blocks (the store keeps them per draw) and once with this frame's.
- The difference of the two clip positions is the exact screen motion of that vertex, for any change of camera and
  object together:
  - translation parallax falls with depth by itself (it is a projection, not a screen-space offset);
  - points at infinity get rotation only;
  - Link walking, the boat, enemies and animation all appear as what they are.
- No separate pose history is needed, because the previous frame's real matrices are kept.
- Eye 1 uses the same blocks with its eye shift (`project_eye`) in both frames.
- `motion_6dof_test` checks this. The eye shift equals cameras placed at the eyes. At infinity the result equals
  nr-bridge's rotation-only formula, which the test re-implements independently.
- No tracking is smoothed or delayed, and no camera writer was added.

**Changed now:**
- **Background motion.**
  - The problem: pixels no depth-writing draw covers kept zero motion. Under a head turn that is wrong, and it is where
    DLSS smears the sky.
  - The change: draws that test depth without writing it (the sky, the sea, effects) now go into the pass too, if they
    have a 3D camera (a perspective projection in their vertex data: `motion::camera_draw`). Their viewport depth is
    pinned to the value the depth was cleared to (`Surface::clearedDepth`, set at the clear). The pass's EQUAL test then
    lets them through only where no geometry wrote depth.
  - Their motion is their own vertex shader's, so it is exact for whatever the sky does. They are drawn after the
    geometry, in draw order.
  - Limit: draws with the depth test off have no depth attachment and no motion pipeline, so they stay at zero. Whether
    the game's sky is one of those needs a run; the `[motion] background draws` count shows it.
  - `WWHD_VK_MOTION_BACKGROUND=0` turns it off.
- **History resets** (DLSS's `Reset`, never set before).
  - In the game:
    - a jump of the drawn view in one frame: over 25 degrees or 3 m (a recentre, a snap turn, an event camera taking
      over, a teleport; `motion::view_jump` in `CameraOverride`);
    - the interpolation's camera cuts (`step_is_cut`).
  - These call `render::note_view_cut`. The cut rides with that frame's swap (word 2, bit 1), so the reset lands on
    exactly the frame that cut.
  - A new scene resets too: most of an eye's draws have no previous match (`motion::scene_changed`).
  - Both eyes reset together, but their histories stay separate.
  - Only while DLSS runs, in any mode (`render::view_cuts_wanted`, set after the probe): with DLSS off the game does
    not look for cuts. The sky motion is part of the motion pass, which runs only with DLSS too.
- **Motion's y sign.**
  - The motion target holds half the clip-space difference. The game draws with a viewport of negative height (its y
    scale is negative), so clip-space y points up while the picture's y points down. `MV.Scale.Y` was +height.
  - The scale now carries the viewport's sign, taken per eye from the packets' viewports
    (`motion::viewport_flips_y`), and the log says which it found. If the viewports were positive, nothing changes.
  - Unverified until a DLSS run.

**Not done, and why:**
- DLSS 5 NR itself, and nr-bridge's work scale, residual blend and fovea: no official model (section 2).
- Background draws with the depth test off: no motion pipeline (see above).
- Batched draws: still no motion, as before.
- The jitter's sign: not verified.

**Platforms:**
- DLSS runs on Windows with NVIDIA RTX only (the NGX core).
- On AMD, Intel and the Quest's own renderer, DLSS reports unavailable and none of the motion pass runs
  (`motion_wanted`).
- Quest 3 over Link or Air Link, and PS VR2 on PC, run on the PC's GPU like any PCVR headset, through the same OpenXR
  path. Neither has been tested with DLSS.
- Linux has NGX code paths but is not built or tested here.

**Enable and test:**
- Set `dlss=dlaa` (or `quality`, `balanced`, `performance`) in `settings.ini`, or use the launcher's Graphics tab.
- Put `nvngx_dlss.dll` (310.9.16, from NVIDIA's SDK) next to `wwhd.exe`.
- The log's `[motion]` lines every 600 frames show the background draws and the history resets; the y sign is logged
  once per eye.

**Still needs the headset** (ask before each run, Capped 90):
1. head turns against the sky and sea: no smear;
2. leaning and walking around near objects: no ghosting (translation);
3. snap turn and recentre: no ghost of the old view for a frame;
4. vertical head motion: confirm the y sign (a wrong sign shows as vertical smearing);
5. both eyes equally sharp and stable;
6. fps and the share of time at 90, against DLSS off.

## DLSS Frame Generation: never used

The game creates only NGX feature 1 (Super Resolution / DLAA) and feature 18 (DLSS 5 Neural Rendering, when a driver
offers it); `ngx_vk.cpp` has no other `create` call and never loads `nvngx_dlssg` (Frame Generation). DLSS Frame
Generation interpolates between two finished frames (a frame of latency) and NVIDIA does not support it in VR. VR's
in-between frames come from the headset's runtime: frame synthesis on standalone headsets, Motion Smoothing in SteamVR
on a PC (docs/VR_FRAME_SYNTHESIS.md).

## 11. DLSS in the plain picture (2026-11-01)

Without 3D output (`--no-stereo`, Play Flat), each draw went straight to `draw()`. It skipped the three things the 3D
path does for the motion pass: numbering the draw, keying it in the motion store, and recording its shaders. The
motion pass then had nothing to draw, so neither DLSS 4 nor DLSS 5 ran in the plain picture. The plain path now does
all three (`motion_key_draw`), only while the motion pass runs.

Flat run, saved state, `dlss5=1 dlss=dlaa`, with NVIDIA's signed `nvngx_dlssnr.dll` 310.9.16 next to `wwhd.exe`:
- the driver offers DLSS 5 (`DLSSNR available 1`, needs driver 615, this one is 621.01);
- `DLSS 5 eye 0: 3840x2160 made`, DLAA made after it, both evaluated every frame, no errors;
- the motion pass draws about 500 draws a frame (99.8% matched to the previous frame), plus about 215 background draws;
- the y flip was detected (motion scale 3840 x -2160);
- 81 to 84 fps on the desktop, the same as without DLSS (the desktop is limited by the render thread, so DLSS 5's GPU
  cost does not show here);
- the picture is correct.

Not measured: DLSS 5's GPU time, and the headset.

## 12. DLSS out of the menus (2026-11-01)

No menu offers DLSS any longer: F1 > Graphics has no DLSS 5 section, F1 > VR and the launcher's VR upscaling list no
DLSS 4, and the launcher's Graphics tab has no NVIDIA DLSS section. VR upscaling's Auto no longer starts DLAA on an
NVIDIA PC, and a saved `vr.upscaler=dlss` runs as Auto. The renderer's DLSS code stays: only `dlss=` or `dlss5=` typed
into `settings.ini` turns it on (Auto then uses DLSS as the upscaler, never a second one beside it).

## 13. DLSS 5 back in the menus (2026-11-02)

DLSS 5 Neural Rendering is offered again; DLSS 4 (the Super Resolution modes, VR upscaling's DLSS) stays out.

- **F1 > Graphics and F1 > VR > NVIDIA DLSS 5 Neural Rendering** (the same section in both tabs). While DLSS runs this start: the switch and all of NVIDIA's settings,
  live from the next frame (section "Live DLSS 5"), saved when a control is let go. Otherwise one switch,
  *DLSS 5 (from the next start)*, which saves `dlss5`: DLSS needs the motion vectors made from the start.
- **VR launcher > Graphics > NVIDIA DLSS 5 Neural Rendering.** Detect PC, the switch (once this PC passes) and the
  settings, saved with Save. A `dlss=` mode typed into `settings.ini` is left as it is.

## Both eyes in one motion pass (mobile GPUs, 2026-11-03)

**Why:** a Quest 3 (standalone) player's log showed the motion pass (frame synthesis on) costing 1.1 to 2.3 ms of CPU a
frame for packets and 0.6 to 1.4 ms recording, with every depth-writing draw drawn again once per eye on a GPU that was
already the limit (about 14 ms a frame in `xrEndFrame`).

**Changed, in mobile-GPU builds only** (`mobile_gpu.h`: Android, arm64, or a desktop test build configured with
`-DWWHD_TEST_MOBILE_GPU_PATHS=ON` in its own folder; Windows and Linux on x64 compile the code as it was):
- `motion_pipeline.cpp`: each motion twin also gets a both-eyes form (`pipeline2`): view mask 0b11, the shader's
  `Variant.views` 2, its per-frame bindings with four descriptors (current eye 0, current eye 1, previous eye 0,
  previous eye 1).
- `draw.cpp` (`motion_both`): a multiview draw against both layers of one depth builds its two eyes' packets as before,
  then combines them into one packet (eye 2) with those four elements and dynamic offsets in the same order. Where the
  two cannot be combined (an eye left out, a different shape, no both-eyes pipeline), each eye's goes on its own.
- `motion_pass.cpp` (`record_both_eyes`): both eyes' motion targets in one two-layer image (eye 1's is the second
  layer, as a two-eye render target's). The geometry: both-eyes packets in one multiview pass, then each eye's own in
  its pass. The background: each eye's in draw order, both-eyes ones drawn when next for both eyes. A barrier between
  the passes (the image is loaded again). About 3 passes a frame, where the eyes' own passes drew 2 with every draw twice.
- `WWHD_VK_MOTION_BOTH_EYES=0`, or `vkMotionBothEyes=0` in settings.ini, turns it off (the eyes' own passes). The
  environment variable takes precedence.

**Checked on the desktop** (test build, desktop stereo, `WWHD_VK_MOTION_PASS=1`, fixed seed, Link walking and the camera
turning, motion targets dumped with `WWHD_VK_MOTION_DUMP`): the game's pictures identical in every run; the motion
targets identical to the eyes' own passes at frame 4160 (both eyes), and at frame 4200 in one run but not the next.
**Open:** a run-to-run difference remains at frame 4200 (the eyes' own passes never vary); not found yet. Not yet run on
a Quest.

## 14. DLSS 5 out of the menus again (2026-11-03)

At the player's request, F1 > Graphics, F1 > VR and the VR launcher's Graphics tab no longer offer DLSS 5 Neural
Rendering (the sections of section 13 removed: `overlay.cpp` `dlss5_options`, `vr_launcher.cpp`
`tab_graphics_dlss5`). The renderer's DLSS code stays, as in section 12: `dlss5=1` typed into `settings.ini` still runs
it, and the launcher keeps a saved `dlss5` value as it is.
