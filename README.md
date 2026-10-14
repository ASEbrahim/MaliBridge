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

Tested on one device so far: a Mali-G720 Immortalis (MediaTek Dimensity 9300+, driver r44p1),
inside the DroidDeck launcher, which runs the server and starts the guest.
Everything here is still work in progress.

| Game | Result |
|---|---|
| Hades | runs well |
| Hollow Knight | runs well, 87-124 fps |
| ULTRAKILL | runs well, 55-57 fps |
| Celeste | runs well, 57-59 fps |
| Portal | runs well, 80 fps |
| Cuphead | runs well |
| Dead Cells | runs well |
| ENDER LILIES | runs well, 42-44 fps |
| Lethal Company | runs well, 43-45 fps |
| Subnautica | runs well, 20-24 fps with Detail set to High |
| Inscryption | runs well, 68-83 fps |
| Doki Doki Literature Club | runs well, title screen 91-92 fps |
| Half-Life 2 | runs well, 59-74 fps in the opening chapter |
| Portal 2 | runs well, about 45-50 fps in play with drops to around 30 |
| Hollow Knight: Silksong | runs very well, a steady 62-66 fps in play |
| Stardew Valley | runs well, 60 fps (its cap) on the title screen |
| Risk of Rain 2 | runs well at half resolution: 18-30 fps through a boss fight with crowds and effects (15-23 fps at 1280x720); needs descriptor batching off, the v0.2.1 default |
| The Witcher 3 (DX12) | runs, but slowly (long GPU stalls while loading) |
| Stray | runs, 17 fps in the opening scene (limited by the emulated render thread) |
| Deep Rock Galactic | runs, but slowly |

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

- **BC1-BC7 textures**: decoded on the GPU by a compute shader when uploaded (`shaders/bc_decode.comp`), and where the GPU samples ETC2/EAC/ASTC (Mali does) encoded again into those in the same pass (`shaders/etc2_enc.glsl`, `astc_enc.glsl`, `eac_enc.glsl`; BC6H into ASTC 4x4 HDR, `astc_hdr_enc.glsl`, where the GPU has it), so the texture stays compressed at about its BC size. Otherwise decoded (BC6H to RGBA16F); BC1 goes to 16-bit where it can. `VKBRIDGE_BC_TRANSCODE=0` decodes everything; `VKBRIDGE_BC_TRANSCODE_QUALITY=fast` encodes with less search (faster uploads, a little lower PSNR). Copies into a BC image from a same-size uncompressed one (`vkCmdCopyImage`, D3D11 `CopyResource`) go through the same path; copies out of a BC image into an uncompressed one are not supported.
- **Push descriptors** (`VK_KHR_push_descriptor`): emulated with descriptor sets per command buffer. vkd3d-proton will not start without them.
- **Descriptor limits**: the 1,000,000-per-stage limits vkd3d-proton asks for are reported, and layouts are cut down to what the GPU has (500,000 on r44).
- **Null descriptors / robustness2**: reported, with null descriptors replaced by dummy resources (the buffer zero-filled; in descriptor set updates, templates, push descriptors and vertex buffer binds), and the driver's own buffer and image robustness turned on. A null image gets a stand-in of the shape its shader declares (1D, 2D, 3D, cube, arrays; float, uint or sint), read from the pipelines' SPIR-V as Lee Gao's compat_layer does.
- **vkd3d-proton descriptor set 3 repair**: rebinds the heap set when vkd3d-proton binds a corrupted handle. The corruption comes from a union in vkd3d-proton v2.12-v3.0 and hung the GPU in The Witcher 3.
- **Stream output fold**: the pass-through geometry shader that DXVK builds for D3D11 stream output is folded into the vertex shader. Mali runs geometry shaders very slowly (Subnautica went from 6 to ~30 fps).
- **Clip/cull distance**: built-ins turned into varyings, with clipping done in the fragment shader.
- **Per-vertex built-ins**: loose built-ins in geometry and tessellation shaders are folded into `gl_PerVertex` blocks. Loose ones crash Mali's compiler.
- **SPIR-V fixes**: DXVK's spec-constant `OpSelect` guards, which Mali's compiler folds wrongly so textures read black; YCbCr samplers in compute layouts.
- **Vertex attribute divisor**: the binding gets a stride of 0 and is rebound for each run of instances, with draws split to match.
- **maintenance5 / maintenance6, depth clip enable, load/store op none**: mapped onto what the driver has.
- **dualSrcBlend, fillModeNonSolid, multiViewport**: reported so that DXVK 3 starts. The rare use is approximated and logged.
- **multiDrawIndirect** (Mali-G57 on r32/r38): an indirect draw of n commands is recorded as n single indirect draws.
- **Storage texel buffer alignment**: any texel offset is accepted (vkd3d-proton 3).
- **Elastic descriptor pools**: allocation past a pool's size, which desktop drivers allow and Mali refuses.
- **OPAQUE_FD external memory and image dma-buf export**: backed by dma-buf / dma-heap pages.
- **Pipeline compile crash guard**: a driver crash inside shader or pipeline compilation fails that call instead of the whole session.
- **Persistent pipeline cache**: Mali recompiles everything in each new process unless it gets a cache, and DXVK passes none.
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

`host/` builds both halves for x86_64 Linux. The server then runs on the desktop's own driver
(RADV by default), and it can pretend to lack what Mali lacks:

```sh
make -C host          # server, client ICD and test programs into host/out
host/run_tests.sh     # every check, once on the plain driver and once imitating Mali (mali_env.sh)
```

The suite runs `smoke`, `bcn_check` (every BC format and mode, bit-exact against bcdec; imitating
Mali, once with BC transcoded to ETC2/EAC/ASTC, by PSNR against bcdec, and once decoded; the BC6H
-> ASTC HDR transcoder, which RADV cannot sample, runs directly on HDR images and its blocks are
decoded by bcn_check's own ASTC decoder; `bcn_check hdr` runs only that),
`nulldesc_check` (null images of every view type and numeric class, written directly, by template
and pushed; imitating Mali, each must get a stand-in of its shape), `opaque_check`, `recycle_check`,
`desc_traffic_check` (a DXVK-like set allocation and update per
draw, read back by every draw; with and without `VKBRIDGE_NO_DESC_BATCH=1`, the client's message
and byte totals printed), `flicker_check` (DXVK's frame structure: frames in flight, a pool per
frame reset and refilled, submits from another thread, synchronous calls from a third; batched
and not), vkcube over Wayland (dma-buf and wl_shm) and X11, and, if the
`vkb-arch-gamescope` docker image exists, gamescope with Zink. It also fails on any protocol
error in the server log. `d3d11_check` runs under Proton's wine and DXVK when `VKB_WINE_DIR`
points to a prefix holding `d3d11_check.exe` and DXVK's DLLs, and `d3d11_flicker` (every frame of
a many-draw D3D11 workload read back and checked, batching on) when `d3d11_flicker.exe` is there too. `tests/clip_check.sh` and
`tests/pervertex_check.sh` test the SPIR-V rewrites (spirv-tools from PATH or a docker image).
`query_check`, `placed_check` and `desc_limits_check` run against `. host/mali_env.sh`.
`crash_contain_check` needs a server started with `VKBRIDGE_TEST_CRASH_PIPELINE=1`.

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
| `VKBRIDGE_MEM_STATS=0` | server | turns off the periodic memory accounting line (on by default) |
| `VKBRIDGE_TRACE=1` | client | logs every call to stderr (`VKBRIDGE_TRACE_COMM=<name>` limits this to one process) |
| `VKBRIDGE_DISCARD_SHADERS=h-<hash>,...` | server | pipelines with these shaders draw nothing |
| `VKBRIDGE_STUB_SHADERS=h-<hash>,...` | server | these fragment shaders are replaced by a constant colour |
| `VKBRIDGE_SKIP_SHADERS=h-<hash>,...` | server | the hang detector skips draws with these shaders |
| `VKBRIDGE_DUMP_SPIRV=<dir>` | server | saves the shader modules handed to the driver |
| `VKBRIDGE_SPIRV_FIX=0/1` | server | forces the SPIR-V fixes off or on (default: on for ARM GPUs) |
| `VKBRIDGE_EMULATE=0` | server | no emulations at all |
| `VKBRIDGE_EMU_OFF=<hex mask>` | server | turns off chosen emulations for bisecting (bits in `common/vkb_proto.h`) |
| `VKBRIDGE_EMU_<NAME>=0` | server | turns off one emulation (`BCN`, `PUSH`, `DESCLIMITS`, `ROBUSTNESS2`, `MULTIDRAW`, ...) |
| `VKBRIDGE_BC_TRANSCODE=0` | server | BC textures decoded instead of transcoded to ETC2/EAC/ASTC (`VKBRIDGE_FAKE_MISSING=astchdr`: only BC6H decoded) |
| `VKBRIDGE_BC_TRANSCODE_QUALITY=fast` / `high` | server | how hard the BC transcoders search. `fast`: shorter searches (on the host GPU about 3.7x faster ETC2, 1.6-1.8x EAC, 1.5x BC7 -> ASTC) at 0.2-2 dB PSNR less, for quicker loading of large textures; `high`: every ASTC candidate, up to 0.6 dB more for 1.5x the BC7 time (ETC2/EAC as default). Default: today's encoders |
| `VKBRIDGE_NO_XFB_FOLD=1` | server | turns off the stream output fold |
| `VKBRIDGE_NO_CRASH_GUARD=1` | server | turns off pipeline crash containment |
| `VKBRIDGE_NO_FORCED_ROBUSTNESS=1` | server | the robustness2 emulation leaves buffer and image robustness to the app instead of turning them on |
| `VKBRIDGE_NULLDESC_2D=1` | server | null images all get the one 2D RGBA8 stand-in, whatever shape the shader declares (as before v0.3) |
| `VKBRIDGE_FAKE_MISSING=bc,push,...` | server | pretends the GPU lacks features (host testing, see `host/mali_env.sh`) |
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
- [compat_layer](https://github.com/leegao/compat_layer) by Lee Gao, MIT: turning on image
  robustness alongside buffer robustness for the robustness2 emulation (`server/server_instance.c`)
  follows its robustness2 emulation, as do zero-filling the null-descriptor stand-in buffer and
  replacing nulls in push descriptor writes (`server/server_nulldesc.c`). No code is copied; `docs/COMPAT_SURVEY.md` compares the layers.
- The Mali compatibility layer (`VK_LAYER_DROIDDECK_mali_compat`) by David Cimo
  ([droiddeck-mali](https://github.com/DavidCimo/droiddeck-mali)) and The412Banner (DroidDeck branch
  `test/non-adreno-all`), GPL-3.0: the zero-filled null-descriptor buffer and the null push
  descriptor writes follow its stand-ins, and the multiDrawIndirect emulation
  (`server/server_emu_compat.c`) follows The412Banner's `draw.c`.
- Design notes from Winlator's Vortek (running the server inside the app process) and GameNative
  (the order of memory-sharing strategies on Mali, the r44 robustness2 profile) informed the
  server. No code from either project is included.
