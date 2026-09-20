# Tier 5.2: VA-API encode daemon

A host-side (glibc) process that does the real hardware H.264 encode on behalf of the Codec2
component that will eventually run inside Android (bionic).

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

`AF_UNIX`/`SOCK_STREAM`. One connection = one encode request/response for now (a real streaming
protocol -- reusing one connection across many frames, proper GOP structure -- is Tier 5.6's
concern once this baseline round-trip is confirmed end to end from inside Android).

- **Request**: `sendmsg()` with an `EncodeRequest` struct (width, height, NV12 plane strides/
  offset, dma-buf size) as the regular payload and the dma-buf fd as `SCM_RIGHTS` ancillary data,
  in a single call — so the fd and the metadata describing its layout can never arrive mismatched.
- **Response**: an `EncodeResponse` header (status + byte count), then that many bytes of
  Annex-B H.264 if status is 0.

NV12 only, matching what a real Codec2 encoder input buffer (`GRALLOC_USAGE_HW_VIDEO_ENCODER`)
should look like — not the RGBA SurfaceFlinger composited buffer the Tier 3 real-gralloc spike
found (that was the wrong kind of buffer for this exact reason, see that tier's README).

## Build & run

```sh
gcc -O2 -Wall -o daemon daemon.c $(pkg-config --cflags --libs libva libva-drm)
gcc -O2 -Wall -o test-client test-client.c $(pkg-config --cflags --libs libdrm)

./daemon &          # listens on /dev/vaapi-helper/socket, creates the dir if needed
./test-client       # builds a synthetic dma-buf, sends it, writes out.h264
ffprobe out.h264 && ffmpeg -i out.h264 -f null -
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

## What's next

- **5.5**: wire this client logic into `tier4-codec2-skeleton`'s `createComponent()` for a real
  Codec2 component, not just a standalone test binary.
