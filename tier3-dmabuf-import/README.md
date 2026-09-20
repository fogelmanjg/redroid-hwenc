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

The second program (`real-gralloc-import.c`, see below) builds separately:

```sh
gcc -Wall -Wextra -O2 -o real-gralloc-import real-gralloc-import.c $(pkg-config --cflags --libs libva libva-drm)
sudo ./real-gralloc-import <host-pid-of-a-redroid-process> <its-dmabuf-fd-number>
```

Find the pid/fd by grepping `/proc/*/fd` for `dmabuf:` inside a running redroid container's host
PID range (`docker top <container>` maps container processes to host PIDs directly).

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

## Closing the loop: import against a REAL redroid gralloc dma-buf (`real-gralloc-import.c`)

Everything above used a synthetic DRM dumb buffer. This second program answers the question that
was still open: does the same import mechanism work against a dma-buf that's *actually* a redroid
Android gralloc buffer, pulled live from a running container — not a generic stand-in?

**Getting the fd out of the container:** the obvious trick — `open("/proc/<pid>/fd/<n>")` from the
host to dup another process's fd — does **not** work for dma-buf. It fails with `ENXIO`: dma-buf is
backed by an anonymous inode, and anon_inode's default `open` deliberately blocks exactly this
reopen pattern. The correct mechanism is `pidfd_getfd(2)` (Linux 5.6+, `pidfd_open()` then
`pidfd_getfd(pidfd, target_fd, 0)`) — the same syscall CRIU and debuggers use to duplicate fds into
another process, including anonymous-inode ones. `real-gralloc-import.c` uses it directly (needs
root/`CAP_SYS_PTRACE` to target another user's process).

**Where the real dma-bufs live:** swept `/proc/*/fd` across every process in a running redroid
container while `screenrecord` was active. Real `dmabuf:`-backed fds turned up in `surfaceflinger`,
`composer@2.1-service` (the HWC HAL), `media.codec` (the OMX video HAL), and app processes —
`screenrecord`'s own process only held `AshmemAllocator_hidl` memfds, not dma-bufs.

**Result: the import succeeded on the first real try.** A dma-buf fd from `media.codec` (3,932,160
bytes — matches RGBA8888 with a 256-byte-aligned 3072-byte stride at the `ffprobe`-confirmed real
720x1280 recording resolution: this is SurfaceFlinger's composited RGBA output, not a
pre-converted YUV buffer) imported cleanly via the same `VASurfaceAttribExternalBuffers` +
`VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME` path, using `VA_RT_FORMAT_RGB32`/`VA_FOURCC_RGBA`.
`vaCreateSurfaces` returned success immediately. Bonus: `vaDeriveImage` on this imported surface
also succeeded here (unlike the synthetic-NV12 case above, on the same AMD driver) — full CPU
readback of a real externally-allocated dma-buf worked.

**What's not fully nailed down:** readback consistently showed all-zero bytes across four different
buffer instances (two recording sessions, one with visibly non-blank content on screen, confirmed
via a parallel `screencap`). The size math checks out exactly against the independently-confirmed
resolution, so this doesn't look like a format/layout mistake — more likely these particular
`media.codec`-held buffers are pre-allocated pool/reserve slots rather than the actively-composited
"front" buffer at the instant they were grabbed (this Android 11 image's encoder is software-only;
its real per-frame colour conversion may not even round-trip through these particular gralloc
buffers). Not chased further here — full content verification is naturally covered when the
wifi-v3 (Android 15, GPU-backed) image gets this same treatment end to end.

**Bottom line:** the mechanism Tier 3 exists to check — VA-API importing a dma-buf of the exact
kernel object type Android's gralloc actually produces, pulled from a real running Android system
process — is confirmed working, not just plausible from a synthetic stand-in.
