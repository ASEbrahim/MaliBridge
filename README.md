# MaliBridge

A Vulkan bridge that runs DX11/DX12 PC games on Mali GPUs.

## What it is

PC games on an Android device run in a Linux guest (glibc, aarch64) under x86 emulation, with
DXVK or vkd3d-proton turning Direct3D into Vulkan. The guest has no GPU driver of its own. The
Mali driver lives on the Android side, and it is missing features that DXVK and vkd3d-proton
need before they will start: BC textures, push descriptors, robustness2, clip distances and more.

MaliBridge is two programs. The **client** is a Vulkan ICD (`libvulkan_droidbridge.so`) that
the guest's Vulkan loader picks up like any other driver. It encodes each call and sends it over
a Unix socket. The **server** runs on Android, either in the app's own process (JNI) or as a
separate executable. It decodes the calls and runs them on the real Mali driver. Where Mali is
missing a feature, the server makes up for it (decodes BC textures on the GPU, rewrites SPIR-V,
emulates push descriptors, and so on), and the client reports the feature as present.

Host-visible memory is shared, not copied: the server exports dma-bufs, dma-heap pages or
AHardwareBuffers, and the client maps them. Command buffers are recorded on the client and sent
in one message at `vkEndCommandBuffer`. Most calls do not wait for a reply.

```
 Linux guest (glibc, aarch64)                Android (bionic)
+---------------------------------+        +-----------------------------------+
| game (x86, emulated)            |        | vkbridge server                   |
|   D3D11 / D3D12                 |        |   decode, then run each call      |
|   DXVK / vkd3d-proton           |        |   Mali emulations (BCn, push      |
|   Vulkan loader                 |        |   descriptors, SPIR-V fixes, ...) |
|   libvulkan_droidbridge.so ------ socket --->  libvulkan.so (Mali driver)    |
|   (client ICD)                  |        |                                   |
+---------------------------------+        +-----------------------------------+
            \________ shared memory (dma-buf / dma-heap / AHB) ________/
```

The wire code for every Vulkan command and structure is generated from the Khronos registry
(`vk.xml` 1.4.341) by `codegen/gen_vkbridge.py`. The client and server check a protocol hash
when they connect.

## Status

Tested in depth on a Mali-G720 Immortalis (Galaxy Tab S10+, MediaTek Dimensity 9300+, driver
r44p1) and on a Mali-G925 Immortalis (Galaxy Tab S11 Ultra, Dimensity 9400, Android 16), inside
the DroidDeck launcher, which runs the server and starts the guest. With the same build the G925
runs these games about 1.4-2x faster than the G720. See [INSTALL.md](INSTALL.md) to try it.
Everything here is still work in progress.

| Game | Mali-G720 (Tab S10+) | Mali-G925 (Tab S11 Ultra) |
|---|---|---|
| Hades | runs well | not tested |
| Hollow Knight | runs well, 87-124 fps | runs well (player report) |
| ULTRAKILL | runs well, 55-57 fps | not tested |
| Celeste | runs well, 57-59 fps | not tested |
| Portal | runs well, 80 fps | not tested |
| Cuphead | runs well | not tested |
| Dead Cells | runs well | not tested |
| ENDER LILIES | runs well, 42-44 fps | not tested |
| Lethal Company | runs well, 43-45 fps | not tested |
| Subnautica | runs well, 20-24 fps with Detail set to High | 28-40 fps (while thermally throttled) |
| Inscryption | runs well, 68-83 fps | not tested |
| Doki Doki Literature Club | runs well, title screen 91-92 fps | not tested |
| Half-Life 2 | runs well, 59-74 fps in the opening chapter | 95-103 fps |
| Portal 2 | runs well, about 45-50 fps in play with drops to around 30 | 115 fps in the menu |
| Hollow Knight: Silksong | runs very well, a steady 62-66 fps in play | 72-96 fps |
| The Elder Scrolls V: Skyrim Special Edition | runs well, about 60 fps (its cap) on Medium; smooth in third person, some stutter on first load | not tested |
| Stardew Valley | runs well, 60 fps (its cap) on the title screen | 57-62 fps |
| Risk of Rain 2 | runs well at half resolution: 18-30 fps through a boss fight with crowds and effects (15-23 fps at 1280x720); needs descriptor batching off, the v0.2.1 default | 30-43 fps at 1280x720; 20-28 fps in a crowded boss fight, measured while screen recording |
| The Witcher 3 (DX12) | runs, but slowly (long GPU stalls while loading) | starts since v0.4 (before, "DirectX 12 required"); the intro plays at 60 fps. In game: known issue, being worked on (see below) |
| Stray | runs, 17 fps in the opening scene (limited by the emulated render thread) | known issue, being worked on (see below) |
| Deep Rock Galactic | runs, but slowly | not tested |

Known issue on the Mali-G925: its shader compiler never finishes compiling Unreal Engine 4's
colour-grading shader (Stray), so the game waits for ever; The Witcher 3 stops after its intro in
what looks like the same way. Since v0.4 the bridge interrupts a pipeline compile that runs past
10 s and builds the pipeline again with a fallback: for that shader, a neutral colour grade in
place of the game's. This has not yet been confirmed on the device. A real fix, a rewrite of the
shader that the G925 compiles, is being worked on.

Known limit: BC textures are transcoded to ETC2/EAC/ASTC, which Mali samples compressed, at the
size of the BC data (BC1 -> ETC2 RGB8/RGB8A1, BC2/BC3/BC7 -> ASTC 4x4, BC4/BC5 -> EAC R11/RG11,
BC6H -> ASTC 4x4 HDR where the GPU has `textureCompressionASTC_HDR`, as the G720 does).
Without ASTC HDR, BC6H is decoded to RGBA16F (8x its compressed size), and so is any texture the
transcoder cannot take (storage images, uncompressed views in the format list, sizes or types the
GPU refuses). ASTC HDR has no negative values: BC6H SFLOAT texels below zero become 0.
Transcoding is lossy: `bcn_check transcode` measures 31-69 dB PSNR against the BC data depending
on format and content (measured on the Mali-G720: BC1 31-46 dB, BC2/BC3 46-60 dB, BC7 34-56 dB,
BC4/BC5 44-67 dB, BC6H -> ASTC HDR 67-69 dB in linear values against each image's largest).
It also costs upload time: a 4096x4096x2 BC7 texture takes about 310 ms to transcode to ASTC on
the G720 where decoding it takes about 105 ms, so games that stream many large textures load
somewhat more slowly. In Subnautica with Detail set to High the transcoded textures take 2.5 GB
where the BC data is 2.4 GB (decoded they reached 4.75 GB and the game was killed); Stray's BC
textures take 288 MB from 287 MB compressed.

## Mali workarounds

Each one turns on only when the GPU lacks the feature.

- **BC1-BC7 textures**: decoded on the GPU by a compute shader when uploaded (`shaders/bc_decode.comp`), and where the GPU samples ETC2/EAC/ASTC (Mali does) encoded again into those in the same pass (`shaders/etc2_enc.glsl`, `astc_enc.glsl`, `eac_enc.glsl`; BC6H into ASTC 4x4 HDR, `astc_hdr_enc.glsl`, where the GPU has it), so the texture stays compressed at about its BC size. Otherwise decoded (BC6H to RGBA16F); BC1 goes to 16-bit where it can. `VKBRIDGE_BC_TRANSCODE=0` decodes everything; by default (`VKBRIDGE_BC_TRANSCODE_QUALITY=auto`) mip levels of 1024x1024 texels or more are encoded with less search (faster uploads, a little lower PSNR) and smaller ones fully; `fast`, `default` or `high` sets one level for all. Copies into a BC image from a same-size uncompressed one (`vkCmdCopyImage`, D3D11 `CopyResource`) go through the same path; copies out of a BC image into an uncompressed one are not supported.
- **Push descriptors** (`VK_KHR_push_descriptor`): emulated with descriptor sets per command buffer. vkd3d-proton will not start without them.
- **Descriptor limits**: the 1,000,000-per-stage limits vkd3d-proton asks for are reported, and layouts are cut down to what the GPU has (500,000 on r44).
- **Null descriptors / robustness2**: reported, with null descriptors replaced by dummy resources (the buffer zero-filled; in descriptor set updates, templates, push descriptors and vertex buffer binds), and the driver's own buffer and image robustness turned on. A null image gets a stand-in of the shape its shader declares (1D, 2D, 3D, cube, arrays; float, uint or sint), read from the pipelines' SPIR-V as Lee Gao's compat_layer does.
- **vkd3d-proton descriptor set 3 repair**: rebinds the heap set when vkd3d-proton binds a corrupted handle. The corruption comes from a union in vkd3d-proton v2.12-v3.0 and hung the GPU in The Witcher 3.
- **Stream output fold**: the pass-through geometry shader that DXVK builds for D3D11 stream output is folded into the vertex shader. Mali runs geometry shaders very slowly (Subnautica went from 6 to ~30 fps).
- **Clip/cull distance**: built-ins turned into varyings, with clipping done in the fragment shader.
- **Per-vertex built-ins**: loose built-ins in geometry and tessellation shaders are folded into `gl_PerVertex` blocks. Loose ones crash Mali's compiler.
- **SPIR-V fixes**: DXVK's spec-constant `OpSelect` guards, which Mali's compiler folds wrongly so textures read black; YCbCr samplers in compute layouts.
- **Shift barriers**: a no-op `OpBitFieldInsert` after each left shift by a constant (Lee Gao's MaliOptimizationBarrierPass), so the compiler cannot fold the shift into speculative memory accesses that fault the GPU. On for every ARM GPU; `VKBRIDGE_SHL_BARRIER=0` turns it off.
- **Vertex attribute divisor**: the binding gets a stride of 0 and is rebound for each run of instances, with draws split to match. Also used where the driver has only `VK_KHR_vertex_attribute_divisor` without the zero divisor (the Mali-G925 on Android 16), and `VK_EXT_vertex_attribute_divisor` is offered there: vkd3d-proton refuses a GPU without it ("DirectX 12 required" in The Witcher 3).
- **Renamed extensions**: where a newer driver has only the KHR name of an extension that games look for under its EXT name (index_type_uint8, line_rasterization, robustness2, global_priority, load_store_op_none, vertex_attribute_divisor), the EXT name is offered and mapped onto the KHR one.
- **maintenance5 / maintenance6, depth clip enable, load/store op none**: mapped onto what the driver has.
- **Polygon mode LINE / POINT** (`fillModeNonSolid`, wireframe; DXVK 3 will not start without it): a pipeline that asks for it gets a line-list or point-list variant, and its draws go through that. A non-indexed draw uses a fixed list of each triangle's three edges; an indexed one gets its edges written by a compute shader (`server/shaders/polygon/lines.comp`) in a command buffer submitted just before the app's, so the render pass is not split. Strips, fans and strip restarts are handled; no geometry shader is used (Mali runs those slowly). Dynamic topology and restart (vkd3d-proton) are read at each draw. Back-face culling and depth bias do not apply to the emulated lines; indirect draws and pipeline libraries still draw solid (logged). `VKBRIDGE_POLYGON_EMU=0` draws all of it solid. The approach follows Mesa's panvk and taowen's libhybris compatibility layer (Apache-2.0).
- **dualSrcBlend, multiViewport**: reported so that DXVK 3 starts. The rare use is approximated and logged.
- **multiDrawIndirect** (Mali-G57 on r32/r38): an indirect draw of n commands is recorded as n single indirect draws.
- **Storage texel buffer alignment**: any texel offset is accepted (vkd3d-proton 3).
- **Elastic descriptor pools**: allocation past a pool's size, which desktop drivers allow and Mali refuses.
- **OPAQUE_FD external memory and image dma-buf export**: backed by dma-buf / dma-heap pages.
- **Pipeline compile crash guard**: a driver crash inside shader or pipeline compilation fails that call instead of the whole session.
- **Compile watchdog**: a pipeline compile still running in the driver after 10 s is interrupted, and the pipeline is built again with a fallback: Unreal Engine 4's colour-grading shader gets a neutral stand-in (`server/shaders/ue4_lut_standin.frag`, no grading), any other fragment shader a constant colour as the last resort. What worked is remembered in `<cache dir>/stuck-pipelines`, so later runs do not wait again; the stuck SPIR-V is saved to `<cache dir>/stuck-spirv/`.
- **Semaphore waits the driver fails**: the Mali-G925 driver sometimes answers `vkWaitSemaphores` with `VK_ERROR_INITIALIZATION_FAILED`. The wait is checked again and succeeds when the semaphores are signalled, instead of ending the game.
- **Persistent pipeline cache**: Mali recompiles everything in each new process unless it gets a cache, and DXVK passes none. The cache is kept per Steam game (by app id), and Steam's "Processing Vulkan shaders" step now fills the cache the game loads, so letting it finish shortens the first run's stutter. Processes that save the same cache at once merge their pipelines, and a game that is killed keeps what it compiled.
- **Shader quirk lists**: listed shaders are drawn with rasterization discarded or replaced by a constant colour, for draws the GPU cannot run.

## Building

The generated wire code (`common/gen/`) and the BC decoder's SPIR-V (`server/bc_spv.h`) are
committed, so a normal build needs neither Python nor a shader compiler.

**Server (Android, arm64-v8a, API 26+)** with the NDK's clang:

```sh
scripts/build-server-android.sh ~/Android/Sdk/ndk/27.1.12297006 out/jniLibs
# -> out/jniLibs/arm64-v8a/libvkbridge_server.so  (executable)
#    out/jniLibs/arm64-v8a/libvkbridge_jni.so     (in-process, JNI)
```

The JNI entry point is `Java_com_droiddeck_launcher_gpu_VkBridgeNative_nativeStart`
(`server/server_main.c`, built with `-DVKB_JNI`). The executable takes
`--socket PATH [--log FILE] [--vulkan LIB] [--cache-dir DIR] [--selftest]`.

**Client (Linux guest, glibc, aarch64)** with an aarch64 gcc:

```sh
scripts/build-client-linux.sh out/client [aarch64-linux-gnu-gcc]
# -> usr/local/lib/vkbridge/libvulkan_droidbridge.so, usr/local/share/vkbridge/droidbridge_icd.json,
#    usr/local/bin/droiddeck-vkbridge-smoke
```

In the guest, point the loader at the manifest (`VK_DRIVER_FILES=.../droidbridge_icd.json`) and the
client at the socket (`VKBRIDGE_SOCKET=/path/to/socket`).

**Regenerating:** `python3 codegen/gen_vkbridge.py` (wire code), `shaders/build.sh` (BC decoder;
glslangValidator, or the NDK's glslc), `tests/gen_shaders.sh` (test shaders; glslangValidator).

## Testing on a desktop

`host/` builds both halves for x86_64 Linux and runs the server on a desktop driver, optionally
imitating what Mali lacks. Every release passes its regression suite on both:

```sh
make -C host && host/run_tests.sh
```

Runs from different checkouts can go at the same time: each run has its own server, socket and
log, and stops only its own servers.

## Environment toggles worth knowing

The server reads these from its environment. Most of the ones read per device can also go in
`<cache dir>/env` as `KEY=VALUE` lines, which are re-read at every `vkCreateDevice`. That way
they take effect on the device without restarting the app.

| Variable | Side | Effect |
|---|---|---|
| `VKBRIDGE_SOCKET` | both | socket path |
| `VKBRIDGE_HANG_DETECT=1` | server | fence after every submission. A hung draw is reported with its state (bound sets, root constants, render targets, device fault info) |
| `VKBRIDGE_HANG_AFTER_MS` | server | how long a submission may run before it counts as hung |
| `VKBRIDGE_GPU_PROF=1` / `2` | server | per-pass GPU timestamps (render passes, dispatches, copies). `2` breaks them down by pipeline |
| `VKBRIDGE_STATS=<s>` | both | every `<s>` seconds, per-command counts of round trips and async requests, and the messages and bytes actually written to the socket. The client also logs the run's totals at exit |
| `VKBRIDGE_DESC_BATCH=1` | client | async requests batched into one message per flush and `vkUpdateDescriptorSets` packed per descriptor type (75x fewer messages in Subnautica). Off by default since v0.2.1: Risk of Rain 2 showed black flickering with it on; `VKBRIDGE_NO_DESC_BATCH=1` forces it off. With it on, `VKBRIDGE_DESC_BATCH_NOFRAME=1` keeps only the packing and `VKBRIDGE_DESC_BATCH_NOPACK=1` only the batching (to tell which half a problem follows) |
| `VKBRIDGE_CACHED_READBACK=0` / `1` / `apps` | server | cached readback: GPU results the CPU reads come from CPU-cached pages (the dma-heap's `system` heap, or the strategy's own dma-buf) with the cache maintenance done by `DMA_BUF_IOCTL_SYNC` at each flush and invalidate, where the startup self-test passes it. `1`: for the client's own frame readback (the copy present path); `apps`: also a HOST_CACHED memory type without HOST_COHERENT for apps; `0`: off. Default: `1` in performance mode, off in safe mode. The guest's `VKBRIDGE_CACHED_READBACK=0` turns off the frame readback's use of it |
| `VKBRIDGE_DMAHEAP_ANY_TYPE=1` | server | experiment: the dma-heap strategy imports its pages even into a memory type the driver's dma-buf fd properties leave out (out of the spec; the self-test still checks each type both ways before it is used) |
| `VKBRIDGE_MEM_STATS=0` | server | turns off the periodic memory accounting line (on by default) |
| `VKBRIDGE_TRACE=1` | client | logs every call to stderr (`VKBRIDGE_TRACE_COMM=<name>` limits this to one process) |
| `VKBRIDGE_DISCARD_SHADERS=h-<hash>,...` | server | pipelines with these shaders draw nothing |
| `VKBRIDGE_STUB_SHADERS=h-<hash>,...` | server | these fragment shaders are replaced by a constant colour |
| `VKBRIDGE_SKIP_SHADERS=h-<hash>,...` | server | the hang detector skips draws with these shaders |
| `VKBRIDGE_DUMP_SPIRV=<dir>` | server | saves the shader modules handed to the driver |
| `VKBRIDGE_SPIRV_FIX=0/1` | server | forces the SPIR-V fixes off or on (default: on for ARM GPUs) |
| `VKBRIDGE_SHL_BARRIER=0/1` | server | forces the optimization barrier after constant left shifts off or on (default: on for ARM GPUs) |
| `VKBRIDGE_EMULATE=0` | server | no emulations at all |
| `VKBRIDGE_EMU_OFF=<hex mask>` | server | turns off chosen emulations for bisecting (bits in `common/vkb_proto.h`) |
| `VKBRIDGE_EMU_<NAME>=0` | server | turns off one emulation (`BCN`, `PUSH`, `DESCLIMITS`, `ROBUSTNESS2`, `MULTIDRAW`, ...) |
| `VKBRIDGE_BC_TRANSCODE=0` | server | BC textures decoded instead of transcoded to ETC2/EAC/ASTC (`VKBRIDGE_FAKE_MISSING=astchdr`: only BC6H decoded) |
| `VKBRIDGE_BC_TRANSCODE_QUALITY=auto` | server | the default: `fast` for mip levels of 1024x1024 texels or more (2048x512 too; nearly all of a big texture's encoding time, and what a 720p-1080p screen shows least), `default` for smaller ones. Set `fast`, `default` or `high` to use one everywhere |
| `VKBRIDGE_BC_TRANSCODE_QUALITY=fast` / `default` / `high` | server | how hard the BC transcoders search. `fast`: shorter searches (on the host GPU about 3.7x faster ETC2, 1.6-1.8x EAC, 1.5x BC7 -> ASTC) at 0.2-2 dB PSNR less, for quicker loading of large textures; `high`: every ASTC candidate, up to 0.6 dB more for 1.5x the BC7 time (ETC2/EAC as default). `default`: the full search everywhere |
| `VKBRIDGE_COMPILE_WARN_S=<s>` | server | a pipeline compile running longer than this (default 5) is logged with its shaders' hashes and their SPIR-V saved to `<cache dir>/stuck-spirv/` |
| `VKBRIDGE_COMPILE_TIMEOUT_S=<s>` | server | a compile running longer than this (default 10) is interrupted and the pipeline built with a fallback; `0` only logs |
| `VKBRIDGE_COMPILE_FALLBACKS=gsfold,fc2` | server | extra fallbacks tried first after a stuck compile: the layer-only geometry stage folded into the vertex shader, and `SPV_KHR_float_controls2` stripped |
| `VKBRIDGE_LAYER_GS_FOLD=1` | server | folds every layer-only pass-through geometry stage up front |
| `VKBRIDGE_NO_PCACHE=1` | server | turns off the persistent pipeline cache |
| `VKBRIDGE_PCACHE_KEY=<name>` | client | which persistent pipeline cache this process gets (default: the Steam app id) |
| `VKBRIDGE_WAIT_FIX=0` | server | hands failed semaphore and fence waits straight back to the app; `VKBRIDGE_WAIT_RETRY_MS` (default 2000) is how long a failed wait is checked again |
| `VKBRIDGE_NO_XFB_FOLD=1` | server | turns off the stream output fold |
| `VKBRIDGE_NO_CRASH_GUARD=1` | server | turns off pipeline crash containment |
| `VKBRIDGE_NO_FORCED_ROBUSTNESS=1` | server | the robustness2 emulation leaves buffer and image robustness to the app instead of turning them on |
| `VKBRIDGE_NULLDESC_2D=1` | server | null images all get the one 2D RGBA8 stand-in, whatever shape the shader declares (as before v0.3) |
| `VKBRIDGE_FAKE_MISSING=bc,push,...` | server | pretends the GPU lacks features (host testing, see `host/mali_env.sh`). Read from `<cache dir>/env` at server start too, so a newer Mali can take an older one's paths |
| `VKBRIDGE_SERVER_HIDE_EXTS=<ext>,...` | server | hides these driver extensions from clients |
| `VKBRIDGE_HIDE_EXTS`, `VKBRIDGE_MAX_API` | client | hide extensions or cap the API version reported to apps |
| `VKBRIDGE_DEBUG=1`, `VKBRIDGE_LOG_LEVEL=<n>` | client | client log verbosity |

## Layout

```
client/        Vulkan ICD (guest side): dispatch, command buffers, memory, descriptors, WSI (Wayland, X11)
server/        Android server: connections, instances/devices, memory sharing, emulations, diagnostics
common/        wire protocol shared by both sides; gen/ holds the generated encoders/decoders
codegen/       gen_vkbridge.py: generates common/gen/ from vk.xml
shaders/       BC1-BC7 decoder and ETC2/EAC/ASTC (LDR, HDR) encoders (GLSL), table generators -> server/bc_spv.h
tests/         self-checking Vulkan tests, SPIR-V corpus, device-side tools (gpubench, gauntlet)
host/          desktop build and regression suite
scripts/       device builds (Android server, Linux client)
third_party/   Vulkan-Headers 1.4.341, bcdec, etc2_encode's licence (the encoders derive from it)
```

## Roadmap

- Package the Mali emulations as a standalone Vulkan layer that works without the socket bridge.
- Test on more devices and driver versions (other Mali generations, newer drivers).

## License

GPL-3.0-or-later. See [LICENSE](LICENSE). Each source file carries an SPDX header.

## Credits

- [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) 1.4.341 (Khronos Group),
  Apache-2.0 OR MIT: `third_party/Vulkan-Headers-1.4.341` (license in its `LICENSE.md`). The
  wire code is generated from its `vk.xml`.
- [bcdec](https://github.com/iOrange/bcdec) by Sergii Kudlai, MIT or Unlicense:
  `third_party/bcdec/bcdec.h` (license at the end of the file). The GPU decoder
  (`shaders/bc_decode.comp`) follows its arithmetic, its BC6H/BC7 tables are generated from it
  (`shaders/bc_tables.glsl`), and `bcn_check` uses it as the reference decoder. Patched here: its
  signed BC4/BC5 endpoints are cast to `signed char`, not `char` (unsigned on AArch64, where
  `bcn_check` built for the guest then expected unsigned endpoints).
- [etc2_encode](https://github.com/leegao/etc2_encode) by Lee Gao, MIT: the ETC2 and ASTC encoders
  (`shaders/etc2_enc.glsl`, `shaders/astc_enc.glsl`) are ported from its compute shaders and
  extended, with its tables; its licence is in those files and `third_party/etc2_encode/LICENSE`.
- Lee Gao (leegao) found the Mali compiler's mishandling of DXVK's spec-constant composites
  (bionic-vulkan-wrapper issue #93). `server/server_spvfix.c` fixes the same pattern.
- Lee Gao's MaliOptimizationBarrierPass ([SPIRV-Tools fork](https://github.com/leegao/SPIRV-Tools),
  Apache-2.0; bionic-vulkan-wrapper PR #162): `server/spirv_shlbarrier.c` is a C port of it (a
  no-op `OpBitFieldInsert` after each left shift by a constant, so Mali's compiler cannot fold the
  shift into speculative memory accesses); its notice is kept in that file.
- Lee Gao's write-up "WSI woes on Mali" (winlator-internals) and GameNative's mesa PR #8: older Mali
  drivers fail an import that carries a dedicated-allocation struct for a resource not made for that
  handle type; `server/server_errata_mem.c` takes the struct off such imports. No code is copied.
- [compat_layer](https://github.com/leegao/compat_layer) by Lee Gao, MIT: turning on image
  robustness alongside buffer robustness for the robustness2 emulation (`server/server_instance.c`)
  follows its robustness2 emulation, as do zero-filling the null-descriptor stand-in buffer and
  replacing nulls in push descriptor writes (`server/server_nulldesc.c`). No code is copied.
- The Mali compatibility layer (`VK_LAYER_DROIDDECK_mali_compat`) by David Cimo
  ([droiddeck-mali](https://github.com/DavidCimo/droiddeck-mali)) and The412Banner (DroidDeck branch
  `test/non-adreno-all`), GPL-3.0: the zero-filled null-descriptor buffer and the null push
  descriptor writes follow its stand-ins, and the multiDrawIndirect emulation
  (`server/server_emu_compat.c`) follows The412Banner's `draw.c`.
- The412Banner's testing of an early copy of the bridge on Mali-G615 and Mali-G925 phones
  (DroidDeck branch `test/non-adreno-all`) found the pipeline cache directory string used after the
  in-app start freed it (`server/server_pcache.c`), and its crash logs led to the decoders refusing
  a single-element pointer sent without the count 1 (`codegen/gen_vkbridge.py`). The crash line's
  library offset and replayed commands (`server/server_main.c`) and the self-test's "no host-visible
  memory type" message (`tests/smoke.c`) follow its crash detail and "memory sharing" check.
- Cached readback (`server/server_memory.c`, `client/wsi.c`) follows David Cimo's Venus work on the
  Mali-G715 ([droiddeck-mali](https://github.com/DavidCimo/droiddeck-mali) `docs/mali-venus.md`,
  GPL-3.0): the frame readback buffer in a cached memory type backed by `/dev/dma_heap/system`,
  flush and invalidate as a `DMA_BUF_IOCTL_SYNC` start/end pair. No code is copied.
- [libhybris](https://github.com/taowen/libhybris/tree/arlinux) `arlinux` branch by taowen, Apache-2.0:
  the polygon-mode emulation (`server/server_emu_polygon.c`, `server/shaders/polygon/lines.comp`)
  follows the approach of its `VK_LAYER_HYBRIS_compat` layer (`polygon_*.c`, `polygon_indices.comp`):
  the triangles' index stream rewritten into their edges and drawn as a line list. Mesa's panvk
  does the same on Mali hardware. No code is copied.
- Design notes from Winlator's Vortek (running the server inside the app process) and GameNative
  (the order of memory-sharing strategies on Mali, the r44 robustness2 profile) informed the
  server. No code from either project is included.
