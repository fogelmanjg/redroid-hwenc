# Tier 3: dma-buf import into a VA-API surface

The make-or-break spike: can a dma-buf VA-API did **not** allocate itself
be imported as a VA-API surface and actually driven through real hardware
H.264 encode — not just accepted by `vaCreateSurfaces` without erroring?

## Build & run

```sh
sudo apt-get install -y libva-dev libdrm-dev   # if not already present
gcc -Wall -Wextra -O2 -o vaapi-dmabuf-import main.c $(pkg-config --cflags --libs libva libva-drm libdrm)
./vaapi-dmabuf-import
ffprobe out.h264   # should report: h264, Constrained Baseline, 320x240
```

Edit `DRM_RENDER_DEVICE`/`DRM_PRIMARY_DEVICE` in `main.c` if your
encode-capable GPU isn't at `/dev/dri/renderD128` / `/dev/dri/card1`.

## What this actually tests

1. Allocate a buffer via a plain DRM dumb buffer (`DRM_IOCTL_MODE_CREATE_DUMB`
   on the primary node) — deliberately **not** through Mesa's generic GBM.
   Export it as a dma-buf via `drmPrimeHandleToFD`, the same mechanism a
   gralloc buffer's native handle uses (a `C2ConstGraphicBlock`'s
   `handle->data[i]` are dma-buf fds directly, confirmed against shipped
   AOSP Codec2 code in Tier 1 — see `DEVLOG.md`).
2. Write a known flat-128 NV12 pattern into it via a **plain `mmap()` of
   the dma-buf fd — VA-API is not involved in this write at all.**
3. Import the fd into a VA-API surface via `VASurfaceAttribExternalBuffers`
   + `VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME`.
4. Feed that imported surface through the exact same encode call sequence
   Tier 2 already proved correct (packed SPS/PPS/slice headers, the
   multi-call `vaRenderPicture` split) and check the output is real,
   decodable H.264.

## Results on this hardware (AMD Renoir APU, radeonsi, Mesa 25.2.8)

**Generic GBM cannot allocate NV12 at all.** The first version of this
spike used `gbm_bo_create(gbm, W, H, GBM_FORMAT_NV12, GBM_BO_USE_LINEAR)`
as the stand-in for "what a gralloc buffer looks like." It failed
outright — `gbm_device_is_format_supported()` reports NV12 unsupported
for *every* usage-flag combination tried (`0`, `LINEAR`, `RENDERING`,
`SCANOUT`, and combinations) on this driver; only RGB(8888) formats are
allocatable through generic libgbm here. This is a real finding, not a
dead end to gloss over: **whatever allocator redroid's Android-side
gralloc uses for YUV buffers is not going through this same generic
libgbm path** — that path can't produce NV12 on this GPU at all. Still
open: which allocator redroid's gralloc actually uses, and whether it
hits the same wall.

**Pivoted to a plain DRM dumb buffer** (allocator-agnostic, always linear,
available on any DRM driver) to test the import mechanism itself,
independent of that gralloc-allocator question.

**The import succeeded**, and — the actual answer to Tier 3's question —
**the encode from the imported surface produced output byte-for-byte
identical to Tier 2's fully VA-API-native run**: same 69-byte Annex-B
stream, and decoding both confirms the same output pixel value (130,
Tier 2's documented QP26-quantization rounding of the fed-in 128). This
is strong evidence the hardware encoder genuinely read real pixel data
from the imported dma-buf, not zeros or garbage.

**One thing that did *not* work**: `vaDeriveImage` on the imported
surface fails (`VA_STATUS_ERROR_OPERATION_FAILED`), so the originally
planned "write via mmap, read back via VA-API" coherency check had to be
skipped — this radeonsi driver apparently doesn't support CPU-mapping a
PRIME-imported surface directly, even though it's fully usable as encode
input. The byte-identical encode output above is what actually answers
the zero-copy-usability question instead.

## Cross-hardware validation (2026-09-20, same day)

Ran unchanged (only `/dev/dri/card1` → `/dev/dri/card0`, each host's actual primary node) on two
more real GPUs:

| GPU | Driver | Encode entrypoint | Import result | `vaDeriveImage` on imported surface | Encode output |
|---|---|---|---|---|---|
| AMD Renoir APU (server01) | radeonsi | `EncSlice` | succeeds | fails (non-fatal) | byte-identical to baseline |
| AMD Radeon RX 480 / Polaris10, discrete (jgustavo46) | radeonsi | `EncSlice` | succeeds | fails (non-fatal) | byte-identical to baseline |
| Intel Iris Xe / TigerLake-LP iGPU (jfogelman-n02) | iHD | `EncSliceLP` | succeeds | **succeeds** — direct zero-copy readback confirmed | byte-identical to baseline |

The mechanism holds on all three. The one real cross-vendor difference: Intel's `iHD` driver
supports `vaDeriveImage` (direct CPU mapping) on a PRIME-imported surface; this AMD radeonsi build
does not, on either GPU generation tested — but that limitation doesn't affect encode correctness
on AMD, since the byte-identical output proves the encoder itself reads the imported memory fine
either way. Worth remembering if a future Codec2 component ever wants to read back an imported
surface on the CPU side (e.g. for a software fallback path): that would need a different mechanism
on AMD, or an AMD-specific workaround, while Intel just works.

## What's still open

This proves the *mechanism* — VA-API on this AMD hardware can import and
correctly encode from a dma-buf it didn't allocate. It does **not** yet
prove the buffer came from redroid's actual Android gralloc: the dumb
buffer here is linear and allocator-agnostic, not necessarily the same
tiling/layout a real `AHardwareBuffer`/gralloc allocation would produce.
The next step is getting a real dma-buf fd out of a running redroid
container (allocate an `AHardwareBuffer` via NDK inside the container,
pull its native handle's fd out, hand it to this same import path on the
host) and confirming it imports and encodes the same way.
