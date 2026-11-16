# Compressed dma-buf exports and imports that name LINEAR

A dma-buf image at DRM-format-modifier tiling can be laid out by a compression scheme (AFBC on Mali).
`vkGetImageSubresourceLayout` then answers with the compressed layout: on Mali the row pitch is the
bytes of one superblock row, not of one pixel row. An importer that passes that layout back while
naming `DRM_FORMAT_MOD_LINEAR` gets a linear image sized from the pitch, which is far larger than the
dma-buf.

## Measured on Mali-G2-Ultra-NX MC16 (driver r56), 2026-11-08

The exporter is a Zink surface: 1668x2173 `VK_FORMAT_B8G8R8A8_UNORM`, usage `0x97`, flags `0x8`
(`VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT`), a one-view-format format list, and a modifier list of the
AFBC modifiers.

| | Value |
|---|---|
| Modifier the driver chose | `0x0820000000000101` |
| Memory requirement, and the dma-buf's size | 3,760,128 bytes (1728x2176 payload at 8 bpp) |
| `vkGetImageSubresourceLayout` (COLOR and MEMORY_PLANE_0) | offset 0, rowPitch 27,648, size 3,760,128 |

27,648 is 108 superblocks x 256 bytes: one superblock row of payload.

The importer then created a DRM-modifier image with
`VkImageDrmFormatModifierExplicitCreateInfoEXT { drmFormatModifier = 0, planeCount = 1,
pPlaneLayouts = {offset 0, rowPitch 27,648, size 0} }`. Mali read the pitch as a pixel row's:

```
rowPitch x (height - 1) + width x 4 = 27,648 x 2172 + 6,672 = 60,058,128 bytes
```

`vkAllocateMemory` of 60,058,128 bytes against the 3,760,128-byte dma-buf failed with
`VK_ERROR_INVALID_EXTERNAL_HANDLE` (-1000072003). The server logged:

```
dma-buf import: 60058128 bytes asked for, the dma-buf has 3760128 (an importer's image laid out unlike the exporter's?)
device 457: vkAllocateMemory of 60058128 bytes in memory type 12 (flags 0xb, heap 0), dedicated to an image, dma-buf import, failed: -1000072003
```

Two more shapes of the same import do not work either:

- the explicit AFBC modifier with the full layout (`rowPitch` and `size`) is refused with
  `VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT`;
- the AFBC modifier with the row pitch alone is laid out as the exporter (3,760,128 bytes) and
  imports, but requires the caller to know the modifier that the LINEAR answer hides.

## What the server does

`server/server_dmabuf_meta.c` keeps the newest 64 dma-buf exports: format, extent, mip and layer
counts, the modifier the driver chose, and the plane layout `vkGetImageSubresourceLayout` answered.

`server/server_emu_layer.c` matches every dma-buf import that names an explicit DRM modifier and one
plane layout against that register (same format, extent, mips, layers, offset and row pitch; a zero
size matches any). On a match the image is created at the export's modifier alone, without the
explicit plane layout:

```c
vkb_emu_chain_take(ci, VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT);
VkImageDrmFormatModifierListCreateInfoEXT list = { ..., 1, &export_modifier };
```

The driver lays the image out exactly as the exporter did, `vkGetImageMemoryRequirements` answers
3,760,128 bytes, `vkGetImageSubresourceLayout` answers the layout the importer passed, and
`vkGetImageDrmFormatModifierPropertiesEXT` answers the real (compressed) modifier. If the driver
refuses the list, the create info goes back to what the app asked and the existing retries run.

The register only answers modifiers other than LINEAR: a LINEAR export's importer is right about
LINEAR, and nothing changes for it. LINEAR exports, AHardwareBuffer dma-bufs, and the client are
untouched.

## Expected log

The first eight matches log:

```
dma-buf import image (format 44 1668x2173): its layout (offset 0, row pitch 27648, size 3760128) is the dma-buf export's; made at modifier 0x820000000000101 without the explicit plane layout
```

With the fix, an import of the dma-buf above asks 3,760,128 bytes and succeeds, and no
`60058128 bytes asked for` line appears.

## Reproducing without an application

1. Create an image with `VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT`, a `VkExternalMemoryImageCreateInfo`
   with `VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT`, and a `VkImageDrmFormatModifierListCreateInfoEXT`
   of the compressed modifiers `vkGetPhysicalDeviceFormatProperties2` lists for the format (leave
   LINEAR out, or the driver picks it first).
2. Record `vkGetImageMemoryRequirements`, `vkGetImageDrmFormatModifierPropertiesEXT` and
   `vkGetImageSubresourceLayout` for `VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT`; allocate the memory
   with `VkExportMemoryAllocateInfo`; get the dma-buf with `vkGetMemoryFdKHR`.
3. Create a second image with the same format, extent, usage and flags, and a
   `VkImageDrmFormatModifierExplicitCreateInfoEXT` naming `DRM_FORMAT_MOD_LINEAR` with the plane
   layout from step 2.
4. On an unfixed server, step 3 needs 60,058,128 bytes and the import fails; with the fix it needs
   3,760,128 bytes and binds.

## Tests

`tests/dbmeta_unit.c` covers the register (matching, size 0, wrong pitch/offset/format/extent/mips/
layers, LINEAR exports, newest wins, the oldest falls out) and runs in `host/run_ci_tests.sh`.

## Measured with an application: Zen 1.24.1b on the same tablet

Before the change, Zink's import of its own 1668x2173 export asked 60,058,128 bytes and failed
(`zink: couldn't allocate memory: heap=0 size=60058128`), and Gecko fell back to `wl_shm`.

With the change, the same run creates the import at the export's modifier:

```
dma-buf import image (format 44 1668x2173): its layout (offset 0, row pitch 27648, size 0) is the dma-buf export's; made at modifier 0x820000000000101 without the explicit plane layout
```

The 60,058,128-byte allocation is gone. The compositor then refuses the buffer Zen presents:

```
(niri) Wayland protocol error: [destroyed object]: error 6: Invalid stride (27648) or height (2173) for plane 0.
```

Zen's wl_buffer names `DRM_FORMAT_MOD_LINEAR` with the compressed export's stride. The bridge is not
the source of that metadata: it answers `vkGetImageDrmFormatModifierPropertiesEXT` for the export
image with the true modifier (`0x0820000000000101`, logged), and the exporter is the caller of
`vkCreateImage`/`vkGetMemoryFdKHR` that hands the buffer to the compositor. An exporter must use the
modifier it queried (or a stride that matches the modifier it names). Until it does, this fix removes
the allocation failure and the silent `wl_shm` fallback starts at the compositor instead.

