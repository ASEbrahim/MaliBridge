# Mali compatibility layers compared with MaliBridge

Survey date: 2026-10-14, against MaliBridge `main` d28c07c. The other projects were read as source
only. None of their scripts or binaries was run.

| Layer | Licence | Revision read | What it is |
|---|---|---|---|
| [leegao/compat_layer](https://github.com/leegao/compat_layer) | MIT | `main` 4abad99; branches `compatibility-vkd3d-2.14.1`, `-dxvk-3.0.1`, `-wrapper` (gpuinfo reports per GPU and driver), `spoof-bindless`, `r44` | Implicit Vulkan layer that makes Mali blob drivers start vkd3d-proton 2.14.1 / DXVK |
| [leegao/bcn_layer](https://github.com/leegao/bcn_layer) | MIT (copyright Pipetto-crypto) | `main` 50993a2 | Layer that decodes or transcodes BC1-7 (Granite shaders, ETC2/ASTC encoders) |
| [leegao/bionic-vulkan-wrapper](https://github.com/leegao/bionic-vulkan-wrapper) | **none** | `wrapper` c8baafbd4f4 | Mesa-based wrapper ICD (Winlator lineage). Read for ideas only; nothing is ported from it |
| [DavidCimo/droiddeck-mali](https://github.com/DavidCimo/droiddeck-mali) | GPL-3.0 | `mali-venus` 4f83889 | Venus/vtest route on a Pixel 9 Pro (Mali-G715): Mesa and virglrenderer patches, plus `VK_LAYER_DROIDDECK_mali_compat` |
| [Droid-Deck/DroidDeck](https://github.com/Droid-Deck/DroidDeck) `test/non-adreno-all` | GPL-3.0 (gamescope patches: BSD-2) | ab54536 | The412Banner's non-Adreno test build: David's layer extended (`draw.c`), gamescope patches 0126-0133, PR #517's bridge (excluded here) |

## The workarounds, layer by layer

**MB** gives MaliBridge's status: **have** (already there), **ported** (on branch
`compat/from-layers`), **gap** (missing). **Rec** is the recommendation: **port**, **later**
(worth doing, but not small or not host-testable), or **not needed** (no Mali target of
MaliBridge's lacks the feature, or the workaround is unsafe).

### compat_layer (Lee Gao, MIT)

Target: Mali blob drivers r32p1 and later, Valhall and 5th gen (G57/G68/G77/G78, G310/G610/G710,
G615/G715, G720, G925, G1-Ultra). G72 needs r44p1 or later. Bifrost G52/G76 have no descriptor
indexing, so they get D3D12 FL11 only.

| Workaround | How | MB | Rec |
|---|---|---|---|
| `VK_KHR_push_descriptor` (vkd3d requires it) | Emulated with per-command-buffer sets | have (`server_emu_push.c`) | - |
| robustness2 `nullDescriptor` | Nulls replaced by stand-in resources. Optionally "precise": the view type and format of the stand-in come from SPIR-V reflection of the bound pipeline (`shader_reflector.cpp`, `null_descriptors.cpp`) | have (one 2D RGBA8 view, an R32_UINT texel view, a buffer) | precise variant: **later** (see "Recommended next" 1) |
| Null descriptors in native `vkCmdPushDescriptorSet*` (1f81a12) | Writes fixed before the driver sees them | **ported** (cb28621) | done |
| Null vertex buffers (c228a47) | Stand-in buffer bound | have | - |
| robustness2 emulation turns on `robustImageAccess` (VK_EXT_image_robustness) and `robustBufferAccess` | Feature structures and extension added at `vkCreateDevice` | buffer: have; image: **ported** (056a3fd) | done |
| Zeroed null buffer (`memset` after `vkMapMemory`) | Stand-in buffer cleared on the host | **ported** (33b6a89) | done |
| `VK_EXT_device_fault` always enabled when present | Added at `vkCreateDevice`, for logs | have, only with the hang detector (`VKBRIDGE_HANG_DETECT`) | not needed |
| Dense `sparseBinding` emulation (`COMPAT_EMULATE_SPARSE_BINDING`, off by default) | Sparse resources backed by whole allocations; `vkQueueBindSparse` emulated; `SPARSE_BINDING` added to the compute queue family; residency properties spoofed | gap | **later**: large; D3D12 tiled resources (FL 12.0 strictly) |
| Generic feature spoof list (`generated_spoofed_profile.hpp`): core features such as geometryShader, tessellationShader, logicOp, multiDrawIndirect, pipelineStatisticsQuery, sparseResidency*, shaderResourceResidency/MinLod; next-chain features such as customBorderColor, transformFeedback/geometryStreams, Vulkan 1.2/1.3 feature bits | Reported TRUE; at `vkCreateDevice` any bit the driver lacks is masked off ("Masking feature") | Only what is emulated is reported (BC, clip/cull, dualSrc, fillMode, multiViewport, divisor, depth clip, maint5/6, robustness2, push). Requested bits the GPU lacks are masked the same way (`server_emu.c`) | **not needed** as a blanket. A feature reported but not emulated fails at the first use, not at start-up |
| Spoofed extensions: `VK_EXT_custom_border_color`, `VK_EXT_transform_feedback`, `VK_EXT_calibrated_timestamps`, `VK_EXT_depth_clip_enable` | Listed if missing; masked at `vkCreateDevice` | depth_clip: have (as depth clamp). The rest are native on r32+ Valhall | **not needed** for r44 (custom border colour is missing only on r26/r28 G57/G68/G77) |
| Property spoofs: `filterMinmaxSingleComponentFormats`, `robustBufferAccessUpdateAfterBind`, `uniformTexelBufferOffsetSingleTexelAlignment`, subgroup ops and stages, `maxCustomBorderColorSamplers` 2048, sparse residency properties | Overwritten | storage texel alignment: have (`VKB_EMU_TEXEL_ALIGN`). `filterMinmaxSingleComponentFormats` passes natively on G615/G720 r44p1 (Lee flags it only on G57 r38p1). The other properties are not in the DXVK/vkd3d profiles; MaliBridge found uniform texel alignment fine on r44 | **not needed** (r44); `uniformTexelBuffer...` **later** for r32-r38 (same mechanism as the storage one). Subgroup and sparse residency spoofs are unsafe (shaders and residency queries would trust them) |
| Descriptor limits for vkd3d (1M update-after-bind) | Spoofed to 1,000,000; branch `spoof-bindless` caps layout bindings at 100k | have (reported 1M, layouts cut to the GPU's 500k, `server_emu_desc.c`) | - |
| Descriptor pool recycling across exhausted pools | Pools grown and recycled | have (`server_pools.c`) | - |
| `VK_EXT_descriptor_indexing` aliasing for Vulkan 1.1 drivers | The 1.2 feature struct is mapped onto the extension | gap | **not needed** (every Valhall driver on r32+ is 1.3) |
| Mali hardware counters (`mali_gpu_profiler.cpp`, libGPUCounters) | Per-dispatch counters | MaliBridge has timestamp profiling only (`server_gpuprof.c`) | **later** (diagnostics) |
| gpuinfo profile checker (`generate_spoofed_profile.py`, `check_profiles.yml`) | Every gpuinfo.org Mali report checked against VP_DXVK / VP_D3D12 profiles | n/a | use as reference data (G615 below) |

### bcn_layer (Pipetto-crypto / Lee Gao, MIT)

Target: any GPU without BC. ASTC/ETC2 transcoding tuned on Mali.

| Workaround | How | MB | Rec |
|---|---|---|---|
| BC1-7 decode on upload (Granite compute shaders) | `vkCmdCopyBufferToImage(2)` intercepted | have (`shaders/bc_decode.comp`) | - |
| Transcode to ETC2 / ASTC 4x4 | Compute encoders (`astc_encoder.slang` v3, "350% faster"; `etc2_encode.slang`) | have (ported from etc2_encode). The ASTC encode costs ~790 ms for 4096x4096x2 BC7 on the G720 | **later**: compare its v3 ASTC shader with `perf/astc-encode` |
| `vkCmdCopyImage(2)` from an uncompressed (block-compatible) image into a BC image (f3cb7e9, issue #18) | Source copied to a staging buffer one texel per block, then decoded/transcoded into the destination | gap (logged once: "copies between BC and uncompressed images") | **later, high value**: needs the existing scratch path plus an image-to-buffer copy; host test in `bcn_check` |
| BC formats in `VkFormatProperties3` (DXVK 2, c4755ee) | Format answer patched in `vkGetPhysicalDeviceFormatProperties2` too | have (`client/emu.c`) | - |
| sRGB for BC1 TYPELESS views (6878f43) | Decoded to an sRGB-capable RGBA8 | have (mutable decoded image with the DXGI family) | - |
| 3D BC textures (252fb6d) | Decode per depth slice | have | - |
| Pipeline state snapshot around injected dispatches | Saved and restored | have (`server_emu_cmdstate.c`) | - |
| Mali GPU counters on transcode command buffers | libGPUCounters | gap | later (diagnostics) |

### bionic-vulkan-wrapper (Lee Gao and others, no licence: ideas only, no code ported)

Target: Mali and Adreno blob drivers under Winlator-style wrappers (G57 r32p1, G715 r38p1 noted in comments).

| Workaround | How | MB | Rec |
|---|---|---|---|
| DXVK spec-constant composite folded wrongly by Mali (issue #93) | spirv-tools pass `FixMaliSpecConstantCompositePass` | have (`server_spvfix.c`, own implementation) | - |
| Mali/Qualcomm constant-folding bug (issue #161) | spirv-tools pass `MaliOptimizationBarrierPass` (prebuilt fork; the pass source is not in the repo) | gap | **later**: needs a reproducer. Ask Lee what the pass does and under which licence |
| Hide `VK_EXT_extended_dynamic_state` 1/2/3 on Mali blob for DXVK < 2.0 (issue #50) | Engine name/version from `VkApplicationInfo` | gap | **later**: own implementation in the client if an old-DXVK game shows it. Good candidate for the shared layer if Lee licenses it |
| Clip distance removed (`RemoveClipCullDistPass`) | Distances dropped | have, emulated properly (`spirv_clip.c`) | - |
| fillModeNonSolid, geometry stage dropped when `geometryShader` is missing (PowerVR) | Pipeline rewritten | fillMode: have. Geometry: Mali has it | not needed |
| Features reported and masked at `vkCreateDevice`: geometryShader, depthClamp, depthBiasClamp, dualSrcBlend, multiDrawIndirect, vertexPipelineStoresAndAtomics, transformFeedback, customBorderColor, hostQueryReset | Reported TRUE, masked to the real value | dualSrc, multiDrawIndirect: have/ported. The rest are native on Valhall r32+ | not needed |
| WSI: BGRA8 first, presentId/presentWait, swapchainMaintenance1 | Mesa WSI options | MaliBridge has its own client WSI | not needed |

### droiddeck-mali (David Cimo, GPL-3.0) and the DroidDeck test branch (The412Banner, GPL-3.0)

Target: Pixel 9 Pro (Mali-G715, Android 17) over Venus/vtest; the test branch adds Mali-G57 and G615 (Edge 60) experience.

| Workaround | Where | How | MB | Rec |
|---|---|---|---|---|
| fillModeNonSolid, multiViewport | `tools/venus/layer/pipeline.c` | Reported; LINE/POINT drawn FILL; first viewport only | have | - |
| Clip distance / cull distance | `tools/venus/layer/spirv.c` | Clip: varying + `OpKill`. Cull: dropped | have (clip emulated; cull accepted, not applied) | - |
| textureCompressionBC | `tools/venus/layer/bc.c` | CPU decode with bcdec at record time | have (GPU decode/transcode) | - |
| robustBufferAccess2 → robustBufferAccess | `layer.c` | Feature swapped at `vkCreateDevice` | have | - |
| nullDescriptor stand-ins | test branch `tools/venus/layer/draw.c` (22694a3, 55b30fb) | Buffer zero-filled, RGBA8 view for uniform texel nulls, R32_UINT for storage; plain, template and push writes; vertex buffers | have; zero-fill and push **ported** (33b6a89, cb28621) | texel-view format split: **later** (with the precise variant) |
| **multiDrawIndirect** (Mali-G57) | test branch `tools/venus/layer/draw.c` | Indirect draw of n → n single draws | **ported** (938c5bf, `VKB_EMU_MULTI_DRAW`) | done |
| depthClipEnable via depth clamp | `layer.c` | Same as MaliBridge | have | - |
| Layer active only in Wine processes, not gamescope (`DROIDDECK_MALI_COMPAT`) | `layer.c` | Program-name gate | MaliBridge emulations are per device and only for missing features | not needed |
| dma-buf export where the driver only imports | virglrenderer patch | dma-heap `system-uncached` → `system` → AHardwareBuffer BLOB, imported as dma-buf; importable reported exportable for images **and buffers** | images: have (`VKB_EMU_DMABUF_EXPORT`, dma-heap only). AHB fallback and the buffer answer: gap | AHB fallback: **later (tablet)**; buffer answer: **later** (no known consumer) |
| `VK_EXT_physical_device_drm` hidden under vtest | Mesa Venus patch | Extension off | MaliBridge reports a stand-in node (`VKBRIDGE_DRM_RENDER`) | not needed |
| Sync-fd export faked by a CPU wait | Mesa Venus patch | Wait, then fd -1 | MaliBridge passes real sync fds | not needed |
| WSI readback through a cached, non-coherent memory type | Mesa Venus patch | Invalidate before CPU reads | MaliBridge WSI is its own; dma-heap uncached by default | not needed |
| gamescope 0127 (shm buffers re-uploaded each commit), 0126 explicit sync off without syncobjs | gamescope patches, session env | - | session-level | not needed in the bridge; keep `GAMESCOPE_DISABLE_EXPLICIT_SYNC=1` for the stand-in node |
| gamescope 0128 robustness2 optional, 0131 `findMemoryType` fallback, 0133 unexportable modifiers, 0130 no immutable YCbCr samplers in compute layouts | test-branch gamescope patches | Each works around the PR #517 bridge copy | All four are handled by MaliBridge `main`: robustness2 reported, host-visible types keep DEVICE_LOCAL, dma-buf export emulated, YCbCr samplers swapped (`server_spvfix.c`) | see the G615 section |

## The Mali-G615 (Motorola Edge 60)

The owner's report: on a Mali-G615 the bridge stops in gamescope's init at `rendervulkan.cpp:2418`
(`BInit`) and has a YCbCr sampler problem. There are no logs yet. What the surveyed projects say:

1. **Feature gaps.** Lee's gpuinfo reports (compat_layer branches `compatibility-dxvk-3.0.1` and
   `compatibility-vkd3d-2.14.1`) list G615 MC2 (reports 49735, 49946, 50133) and G615 MC6 (49402,
   49804) on **r44p1**. They are line-for-line identical to the G720 Immortalis MC12 r44p1 report
   (49471) for both DXVK 3.0.1 and vkd3d 2.14.1, apart from the name and driver hash. If the Edge 60
   runs r44p1, MaliBridge's emulations cover it as they cover the G720: BC, push descriptors,
   robustness2, divisor, dualSrcBlend, fillModeNonSolid, multiViewport, clip/cull distance,
   maintenance6, load_store_op_none, storage texel alignment and the 500,000 descriptor limits are
   the same gaps. Like the G720 it lacks `VK_EXT_external_memory_host` (wrapper profile), which
   MaliBridge does not use on Mali (dma-buf, dma-heap or AHB sharing). Newer drivers (r47-r54) only
   lose gaps.
2. **`rendervulkan.cpp:2418` is `assert(modifiers.size() > 0)`** in `CVulkanTexture::BInit`.
   That is upstream gamescope 3.16.29's line 2264, shifted by the test build's patches. The
   flippable output image found no DRM modifier whose `vkGetPhysicalDeviceImageFormatProperties2`
   answer carries `EXPORTABLE`. The412Banner's commit 72196c3 (gamescope patch 0133) says so: "the
   Mali bridge forwards the phone driver's answer, which leaves EXPORTABLE out". That build ran the
   **PR #517 in-tree bridge** (DroidDeck ee0fb082), an older copy without `VKB_EMU_DMABUF_EXPORT`.
   MaliBridge `main` reports dma-buf EXPORTABLE wherever the driver says IMPORTABLE, and backs such
   allocations with dma-heap pages (`server_memory.c`, `client/physdev.c`). Likely fixed already.
   It still depends on two things, which a tablet log answers:
   - `/dev/dma_heap/system-uncached` or `/dev/dma_heap/system` opens in the app's context
     (`dmaheap: allocating from ...`). If neither does, port David's AHardwareBuffer BLOB fallback
     to `alloc_exportable`.
   - The detection probe (R8G8B8A8, linear modifier, sampled and colour attachment) gets IMPORTABLE
     from the G615 driver (bit 0x100 in `emulation 0x... installed`). If not, the probe should also
     try B8G8R8A8 and the modifiers gamescope asks for.
3. **YCbCr samplers.** The412Banner (gamescope patch 0130, commit 6c8a086) found every composite
   compute pipeline failing with `VK_ERROR_INITIALIZATION_FAILED` on the G615 through Venus. The
   common factor was binding 4's immutable YCbCr samplers in a compute layout. MaliBridge `main`
   swaps immutable YCbCr samplers in compute bindings for a plain linear sampler (`server_spvfix.c`,
   found on the G720). The PR #517 copy predates it. Likely fixed; the log line to look for is
   "compute layout: immutable YCbCr samplers replaced".
4. **Also from the test branch, already in MaliBridge:** the G615 lists a
   DEVICE_LOCAL|LAZILY_ALLOCATED type first (skipped by `client/physdev.c` and the null-descriptor
   stand-ins). gamescope wants DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT (kept on shared types).
5. **Session-level, not the bridge:** gamescope's explicit sync needs DRM syncobjs, which the stand-in
   render node does not have (test branch 0136fb6 sets `GAMESCOPE_DISABLE_EXPLICIT_SYNC=1` on
   non-Adreno). Present modes: none of the layers changes them for Mali. DRM format modifiers: only
   David's virglrenderer (export fixup) and MaliBridge (dma-buf export emulation) touch them.

## Ported on `compat/from-layers`

| Commit | What | From |
|---|---|---|
| 056a3fd | robustness2 emulation turns on `robustImageAccess` where the driver has it | compat_layer (MIT) |
| 33b6a89 | null-descriptor stand-in buffer zero-filled | mali_compat (GPL-3.0), compat_layer (MIT) |
| cb28621 | nulls in native push descriptor writes replaced; smoke pushes a null buffer; new run_tests step | compat_layer 1f81a12 (MIT), mali_compat `draw.c` (GPL-3.0) |
| 938c5bf | `VKB_EMU_MULTI_DRAW`: indirect draws split where `multiDrawIndirect` is missing; smoke multi-draw checks | mali_compat `draw.c` (The412Banner, GPL-3.0) |

Each was written for MaliBridge from the idea; no code was copied. Each commit message and the
README Credits name the source and its licence.

## Recommended next

1. **Precise null descriptors** (compat_layer, MIT): stand-ins by view type (2D, 2D array, cube, 3D)
   and format class (float, uint, sint), chosen from the pipeline layout's SPIR-V usage. Today a null
   bound where a shader reads a cube or 3D image gets a 2D view, which is undefined behaviour and a
   plausible fault on Mali. Medium-sized; host-testable with RADV under the Mali imitation.
2. **Uncompressed → BC `vkCmdCopyImage`** (bcn_layer, MIT): D3D11 `CopyResource` between
   block-compatible formats, and vkd3d BC aliasing. Medium-sized; `bcn_check` can test it.
3. **AHardwareBuffer fallback for dma-buf export** (David, GPL-3.0), if the Edge 60 log shows no
   dma-heap.
4. **Dense sparse binding** (compat_layer) for D3D12 tiled resources, once a game needs it.
5. **Mali optimization barrier pass** (wrapper #161), once a reproducer exists and Lee says what it
   does under which licence.

## For a shared layer with Lee

MaliBridge's emulations already run as a dispatch-table layer inside the server. `README.md`'s
roadmap lists packaging them as a standalone Vulkan layer. Lee's compat_layer and bcn_layer are
MIT, and MaliBridge is GPL-3.0, so code can flow from Lee's layers into MaliBridge but not back
without relicensing. Suggested split:

- **Shared data, licence-neutral:** the gpuinfo profile checker and per-driver gap tables
  (compat_layer's `compatibility-*` branches). They are facts, and both projects can key their
  emulations off one table.
- **Shared code, MIT:** if the owner wants the shared layer under MIT, the candidates written fresh
  for it are the small, generic pieces: feature/extension reporting with masking at
  `vkCreateDevice`, null descriptors (precise variant), push descriptor emulation, multiDrawIndirect,
  robustness wiring, and descriptor-limit cutting. The BC transcoders already derive from Lee's MIT
  etc2_encode. MaliBridge-only parts (SPIR-V rewrites for clip distance, stream-output fold,
  per-vertex blocks, the crash guard, memory sharing) stay GPL in MaliBridge.
- **Ask Lee** to put a licence on bionic-vulkan-wrapper's own files (spec-composite pass, optimization
  barrier pass, the DXVK < 2 extended dynamic state rule). Until then, nothing from it is used.

## Needs the tablet

- G615: the log lines named in the G615 section (dma-heap open, emulation bits, YCbCr swap), and
  whether gamescope starts on MaliBridge `main` with these commits.
- `robustImageAccess` cost on Mali: compare fps in one heavy game (Subnautica) with and without
  `VKBRIDGE_NO_FORCED_ROBUSTNESS=1`. RADV cannot show Mali's cost.
- multiDrawIndirect: only a G57-class device (r32/r38) exercises it. The G615/G720 have the feature,
  so the emulation stays off there.
- Null push descriptors: only a driver with native push descriptors and no robustness2 exercises the
  new path. r44 emulates push descriptors, so on the G720 the existing path is used.
