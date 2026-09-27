# Tier 5: the VA-API daemon

A host-side (glibc) persistent process that does real hardware video work on behalf of the Codec2
components running inside Android (bionic): H.264 **encode** (AMD/Intel, via each vendor's own
`VAEntrypointEncSlice`) and, since Tier 7, H.264 **decode** (NVIDIA, via `nvidia-vaapi-driver`'s
`VAEntrypointVLD`). One process, two independent backends — a host can have either, both (two
different GPUs down the line), or neither yet; see "Two backends, one daemon" below.

This file covers the daemon itself and its wire protocol. The sections below are numbered by when
each capability landed (5.2 through 5.12 for encode, then Tier 7 for decode) — treat them as a
build order, not a changelog to read start to finish; `DEVLOG.md` has the full session-by-session
narrative if you want that.

## Why a daemon instead of calling VA-API directly from the Android component

The original Tier 5 plan was to port Mesa's VA-API Gallium frontend
(`gallium/frontends/va`/`targets/va`) to Soong and link it against this build's Android-side
`radeonsi` driver. That driver doesn't exist: this AOSP build's Mesa (`external/mesa3d`) only
compiles the `gfxstream`/ANGLE/Vulkan pieces for Android -- real GLES rendering is *forwarded to
the host*, the same way ChromeOS ARC++ and the Android Emulator do it. Porting the actual
`radeonsi` + winsys + (likely) LLVM stack to bionic would be a vastly bigger undertaking than
porting just the VA-API frontend -- realistically weeks of work on its own.

Since Android's own graphics stack already solves "the guest doesn't have a real GPU driver" by
forwarding to the host, Tier 5 does the same thing for encode: this daemon runs on the host,
using the exact VA-API pipeline already proven in `tier2-vaapi-encode`/`tier3-dmabuf-import`. The
Android-side Codec2 component becomes a thin IPC client instead of needing any VA-API/Mesa code
of its own.

## Protocol (`protocol.h`)

`AF_UNIX`/`SOCK_STREAM`. One connection = one request/response (still true for both commands —
a real streaming protocol, reusing one connection across many frames, remains future work for
either direction).

Every connection starts with a plain 4-byte `VaapiCommand` tag (`VAAPI_CMD_ENCODE` or
`VAAPI_CMD_DECODE`), read via a plain `read()` before either command's own request struct — this
is how one daemon process dispatches both kinds of request on the same socket.

**Encode** (`EncodeRequest`/`EncodeResponse`):
- **Request**: tag, then `sendmsg()` with an `EncodeRequest` struct (width, height, NV12 plane
  strides/offset, dma-buf size) as the regular payload and the dma-buf fd as `SCM_RIGHTS`
  ancillary data, in a single call — so the fd and the metadata describing its layout can never
  arrive mismatched.
- **Response**: an `EncodeResponse` header (status + byte count), then that many bytes of
  Annex-B H.264 if status is 0.
- NV12 only, matching what a real Codec2 encoder input buffer (`GRALLOC_USAGE_HW_VIDEO_ENCODER`)
  should look like — not the RGBA SurfaceFlinger composited buffer the Tier 3 real-gralloc spike
  found (that was the wrong kind of buffer for this exact reason, see that tier's README).

**Decode** (`DecodeRequest`/`DecodeResponse`, added in Tier 7 — see that section below):
- **Request**: tag, then a plain `write()` of a `DecodeRequest` header (width/height hints,
  bitstream size), then exactly `bitstream_size` bytes of Annex-B H.264 — no `SCM_RIGHTS` at all,
  since there's no client dma-buf to import on the way in (the daemon's own decode surface is
  driver-allocated).
- **Response**: a `DecodeResponse` header (status + frame size), then that many bytes of
  tightly-packed NV12 if status is 0.
- The daemon does its own NAL splitting/parsing internally and derives the real width/height from
  the stream's own SPS — the request's width/height are hints only, cross-checked (not trusted)
  against what the SPS actually says.

## Build & run

```sh
# The daemon itself: encode's own deps (libva/libva-drm/libgbm/libEGL) plus -ldl for decode's own
# lazy libvulkan.so.1 loading (see "Two backends, one daemon" above) - no -lvulkan needed at link
# time, so a build on an AMD/Intel-only host still doesn't need libvulkan installed at all.
gcc -O2 -Wall -o daemon daemon.c decode_h264.c -lva -lva-drm -lgbm -lEGL -ldl

gcc -O2 -Wall -o test-client test-client.c $(pkg-config --cflags --libs libdrm)
gcc -O2 -Wall -o decode-test-client decode-test-client.c

sudo mkdir -p /dev/vaapi-helper && sudo chmod 777 /dev/vaapi-helper   # first run only

./daemon &          # listens on /dev/vaapi-helper/socket; logs which backend(s) initialized
./test-client       # encode: builds a synthetic dma-buf, sends it, writes out.h264
ffprobe out.h264 && ffmpeg -i out.h264 -f null -

# decode: needs a real Annex-B stream first, see decode-test-client.c's own error message
# for the exact ffmpeg invocation if /tmp/test.h264 doesn't exist yet
./decode-test-client
```

## Result

`test-client` (host-side only, no Android involved at this checkpoint) builds the same synthetic
NV12 DRM dumb buffer as `tier3-dmabuf-import`, sends it over the socket, and gets back **69 bytes,
byte-for-byte identical to `tier2-vaapi-encode`'s own reference output** — confirmed decodable.
Ran the client twice against the same long-lived daemon process to confirm the persistent VA-API
context (created once at startup, reused per request, only recreated if the resolution changes)
survives repeated requests correctly.

## 5.3: confirmed working from inside Android too

`android-test-client/` is the same protocol/test logic, built via Soong (`cc_binary`, `vendor:
true`) against the AOSP tree directly — no NDK needed, bionic handles `AF_UNIX`/`SCM_RIGHTS`/
`libdrm` identically to glibc here, no surprises. Bind-mounted the daemon's socket directory
(`/dev/vaapi-helper`) into a redroid test container the same way these containers already bind-
mount `/dev/binder`, pushed the built binary to `/data/local/tmp` (writable at runtime, unlike
`/vendor` which is genuinely read-only once Android has booted — see `DEVLOG.md` for the deploy
lessons), and ran it via `docker exec`. Result: **69 bytes, byte-for-byte identical to Tier 2's
reference output**, this time originating from a binary actually running inside Android.

## 5.4: confirmed against a real Android gralloc buffer

`android-test-client/real_gralloc_client.c` allocates a genuine gralloc buffer via
`AHardwareBuffer_allocate(AHARDWAREBUFFER_USAGE_VIDEO_ENCODE)` and gets its dma-buf fd via
`AHardwareBuffer_getNativeHandle()` (`libnativewindow` is LLNDK, explicitly meant for vendor code
to link against) — no more scavenging another process's fd like Tier 3's original real-gralloc
spike had to. Two real findings along the way (neither a blocker, see `DEVLOG.md` for the full
story): this build's Mapper HAL doesn't implement `AHardwareBuffer_lockPlanes()` (worked around
with a plain `mmap()` of the dma-buf fd instead), and requesting flexible YUV420 actually returns
an RGBA8888-sized buffer — plus one real fix: the tightly-packed stride `AHardwareBuffer_describe()`
reports fails VA-API import outright (`"resource allocation failed"`, isolated to a pitch
alignment requirement, not the buffer's origin), fixed by filling/describing the buffer with an
aligned stride instead. Result: another **69 bytes, byte-for-byte identical to Tier 2's reference
output**, this time encoded from a real Android-allocated gralloc buffer.

## 5.5: a real C2Component, driven through the genuine framework machinery

`codec2-component/VaapiEncComponent.{h,cpp}` is a real `C2Component` built on
`SimpleC2Component` (the same base class AOSP's own software codecs use, e.g. the stock
`c2.android.avc.encoder` Tier 0's fix unlocked) rather than porting `external/v4l2_codec2`'s much
larger `EncodeComponent` verbatim — that reference's `VideoEncoder` interface is built for V4L2's
async, interrupt-driven model, which doesn't fit this daemon's simple synchronous round trip, and
even the reference project itself has no complete example wiring it end to end. `process()`
extracts the input `C2GraphicBlock`'s dma-buf fd via `block.handle()->data[0]` (the same pattern
`v4l2_codec2`'s own `createInputFrame()` uses), sends it to the daemon, and writes the H.264 bytes
back via `createLinearBuffer()` — a helper `SimpleC2Component` already provides. `service.cpp`'s
`createComponent()`/`createInterface()` now construct real instances of this instead of returning
`C2_NOT_FOUND`.

`codec2-component/component_test.cpp` drives it with a **real** `C2Work` wrapping a **real**
`C2GraphicBlock` — built via `_C2BlockFactory::CreateGraphicBlock(AHardwareBuffer*)`, a genuine
AOSP API for wrapping an app-provided `AHardwareBuffer` into Codec2 (the same mechanism real
Surface-based encoder input eventually goes through) — through the actual
`queue_nb()`/`process()`/`onWorkDone_nb()` machinery, not a hand-rolled shortcut. Result: **83
bytes of valid, decodable H.264** (a different byte count than every other tier's 69/70 is
expected here — this test didn't fill the buffer with the usual flat-128 pattern, so it's
compressing real uninitialized memory content instead of a known test pattern).

## 5.6–5.12: hardening encode across real GPUs

Getting a real, unmodified app (`scrcpy`) recording correct video through the genuine
`MediaCodec`/`Codec2Client` framework path — not just a hand-built `C2Work` — surfaced a run of
real, GPU-specific bugs: HAL registration/instance-naming gaps, AMD VCN's DCC-compression
restriction (`AMD_DEBUG=nodcc`), the discovery that a real `Surface`-sourced frame is RGBA (not
NV12) and needs a VPP conversion stage, a GPU-generation-specific DRM format modifier that can't
be queried from the Android side at all (so it's determined once at daemon startup instead — see
`rgba_modifier_init()` in `daemon.c`), an Intel-specific tiling-priority difference from AMD, and
an older AMD generation (Polaris/GFX8) that can't describe its tiling as a modifier at all and
needs VA-API's old, pre-modifier import path. All of it lives in this daemon, none of it touches
the Android-side component — see the top-level `README.md`'s hardware compatibility table and
`DEVLOG.md` for the full, bug-by-bug story.

## Two backends, one daemon

`daemon.c`'s `main()` initializes encode (`vaapi_state_init()`) and decode (`decode_h264_init()`)
independently and neither failure is fatal by itself — only both failing together means the
daemon has nothing to do. This matters because the two backends target different vendors: every
encode-capable host tested so far is AMD/Intel, and the only decode-capable host is NVIDIA (no
GPU tested here does both). `handle_connection()` dispatches on the command tag and each handler
checks its own backend's availability, rejecting cleanly (a normal error response) rather than
crashing if that backend never initialized on this host.

One subtlety worth knowing if you're reading `decode_h264_init()`: `nvidia-vaapi-driver` isn't
something libva's own auto-detection resolves to for an NVIDIA render node by itself, so that
function scopes `LIBVA_DRIVER_NAME=nvidia` to just its own `vaInitialize()` call (save the
previous value, force it, initialize, restore it) — so it doesn't clobber the encode backend's own
`vaInitialize()` call in the same process, regardless of which one `main()` happens to run first.

## Tier 7: NVIDIA decode

`decode_h264.c`/`decode_h264.h` port the standalone decode spike
([`tier6-vaapi-decode/main.c`](../tier6-vaapi-decode/), see its own README for the full six-bug
trail to bit-exact) into the persistent-state shape this daemon needs: `decode_h264_init()` (once,
at startup) and `decode_h264_frame()` (once per request — parses whatever SPS/PPS/IDR NALs are in
the buffer it's given, decodes, de-tiles via the same Vulkan technique this project's own NVENC
encode work uses, and hands back tightly-packed NV12 plus the width/height it read out of the
SPS). `decode-test-client.c` is decode's own standalone checkpoint, the same role `test-client.c`
plays for encode.

**Verified on two real hosts in the same session**: on a GTX 1050 Ti (no AMD/Intel GPU at all),
the daemon correctly disabled encode and served decode — a fresh `ffmpeg`-generated H.264 stream
decoded through the real socket protocol came back bit-exact against software decode (`ffmpeg`'s
own PSNR filter: `y:inf u:inf v:inf average:inf`). On an AMD Renoir APU (no NVIDIA GPU), the
daemon correctly initialized encode and failed decode init cleanly (no `nvidia-vaapi-driver`
package there), and the `VAAPI_CMD_ENCODE` tag round-tripped correctly against the existing encode
path (confirmed via the daemon's own request log matching `test-client.c`'s real request fields
exactly) — proving the new tag/dispatch layer doesn't disturb encode's own wire format.

The Codec2 side lives in `codec2-component/VaapiDecComponent.{h,cpp}` — the structural mirror of
`VaapiEncComponent`, registered as `c2.hardware.decoder.h264`. See that pair of files' own header
comments for what differs from the encoder (an `::output` picture-size param instead of `::input`,
`C2PlanarLayout`-aware copying into whatever the real gralloc allocation for the output frame
turns out to be).

## What's next

The daemon-standalone and AOSP-build checkpoints for decode are both done (see Tier 7 above); the
remaining step is the same one Tier 5.6 was for encode: driving `VaapiDecComponent` through the
real `MediaCodec`/`Codec2Client` framework path against a real H.264-containing video source,
instead of a direct socket round-trip. `codec2-component/component_test.cpp`/`client_test.cpp` are
the encode-side precedents to model a decode-side equivalent on.
