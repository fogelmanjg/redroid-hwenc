# redroid-hwenc

**Languages:** English | [Español](README.es.md) | [中文](README.zh-CN.md)

> This project's official language is **English**. The other language files are provided for
> convenience and may not be perfectly accurate or up to date — if in doubt, this file is the
> source of truth.

Hardware video encoding (H.264/H.265, via VA-API) for [redroid](https://github.com/remote-android/redroid-doc).

## The problem

BlueStacks, Nox and MuMu have no real equivalent on Linux desktop. [redroid](https://github.com/remote-android/redroid-doc)
is the closest thing — Android running in a Docker container, with GPU passthrough for
rendering — but streaming that screen (scrcpy, or any video consumed inside an Android app)
still falls back to a **pure software encoder**
(`OMX.google.h264.encoder` / `c2.android.avc.encoder`). `gpuMode=host` accelerates rendering
(OpenGL/Vulkan via Mesa), never the encoder.

This request has been **open since 2022** in the official repo, with nobody having solved and
published a fix:

- [remote-android/redroid-doc#126](https://github.com/remote-android/redroid-doc/issues/126) — AMD VA-API (OMX/Codec2)
- [remote-android/redroid-doc#172](https://github.com/remote-android/redroid-doc/issues/172) — same request, AMD
- [remote-android/redroid-doc#168](https://github.com/remote-android/redroid-doc/issues/168) — same request, Intel
- [remote-android/redroid-doc#535](https://github.com/remote-android/redroid-doc/issues/535) — H.265 request, unanswered comment from September 2025

The project maintainer (`zhouziyang`) always gives the same answer: the VA-API drivers are
already bundled in redroid, but the Codec2/OMX component that actually uses them for encoding is
left as a task for the community. In 4 years, nobody finished it and shipped it.

## Why now

It's not that it's impossible — it's that the intersection of "cares about this specific
problem" and "is willing to dig into AOSP/Codec2/VA-API" is rare. Most people in those issues
are users asking for the feature, not people willing to write it. This repo is an attempt to
solve it in public, in increasing-difficulty tiers, documenting the process as it goes (see
[DEVLOG.md](DEVLOG.md)).

## Roadmap (by difficulty, not by time)

Each tier assumes the previous one is done. ⭐ marks the highest-leverage checkpoint.

- [x] **Tier 0 — Pure research.** How redroid/scrcpy pick the encoder today (via
      `MediaCodecList` capability matching, or a hardcoded name?). Confirm which VA-API encode
      entrypoints (`VAEntrypointEncSlice`) are available on real hardware.
- [x] **Tier 1 — Reading/mapping.** Structure of a Codec2 component (using
      [`android_external_v4l2_codec2`](https://gitcode.com/pi-plus/android_external_v4l2_codec2)
      as a reference for the plumbing, not the hardware logic — that's V4L2, this is VA-API).
      The VA-API **encode** API surface (not decode).
- [x] **Tier 2 — Isolated native prototype.** Standalone program (host, no Android) that
      encodes H.264 via VA-API over `/dev/dri/renderD*`.
- [x] **Tier 3 — ⭐ The make-or-break spike.** Does a gralloc buffer export a `dma-buf` fd that
      VA-API can import zero-copy, inside a container with the same privileges as redroid?
      Nobody confirmed this in 4 years of issues. redroid runs as a container (not a VM) — no
      virtio-gpu in the way, a better starting point than the community seems to assume.
      **Confirmed across three real GPUs** (AMD Renoir APU, AMD Polaris10 discrete, Intel
      TigerLake-LP iGPU) with a synthetic dma-buf first — output byte-identical to a fully
      VA-API-native run in every case — then **confirmed again against a real, live dma-buf pulled
      out of a running redroid container's own gralloc-backed process** (via `pidfd_getfd(2)`, the
      correct mechanism for duplicating an anonymous-inode fd like dma-buf across processes):
      `vaCreateSurfaces` imports it cleanly. See
      [tier3-dmabuf-import/README.md](tier3-dmabuf-import/README.md) for the full writeup,
      including the one open nuance (confirming the specific buffer instance held live frame
      content, not just that the import mechanism itself works).
- [x] **Tier 4 — Codec2 skeleton in Android.** A component that Android recognizes and lists
      (`dumpsys media.c2`) as `c2.hardware.encoder.h264`, without real encoding wired in yet.
      **Confirmed on a real running instance**: `service list` shows
      `android.hardware.media.c2.IComponentStore/vaapi` registered, and `dumpsys` on it reports
      `c2.hardware.encoder.h264` (domain video, kind encoder) with `Active components: NONE` —
      recognized and listed, nothing instantiable yet, exactly as scoped. Built from AOSP's own
      official empty-Codec2-service template (`frameworks/av/media/codec2/hal/services/`), not
      from scratch. See `DEVLOG.md` for the full build/deploy story, including three real bugs
      found and fixed along the way (a build-container lifecycle bug, a docker0 bridge networking
      bug, and a missing-shared-library crash loop).
- [ ] **Tier 5 — Real integration.** Tier 3 + Tier 2 wired into the callbacks of the Tier 4
      component. The actual goal. **Architecture pivot**: porting Mesa's VA-API driver stack to
      build against Android's bionic turned out to need a full `radeonsi` + winsys + LLVM port
      (this build's Mesa only compiles the `gfxstream`/ANGLE/Vulkan forwarding-to-host pieces for
      Android, no native GPU driver at all) — realistically weeks of work on its own. Pivoted
      instead to a host-side encode daemon the Android component talks to over a Unix socket,
      reusing Tier 2/3's pipeline unchanged. Sub-goal 5.2 (the daemon itself) is done — see
      [tier5-vaapi-daemon/README.md](tier5-vaapi-daemon/README.md).
- [ ] **Tier 6 (conditional on Tier 0).** If redroid/scrcpy's codec selection turns out to be
      hardcoded instead of capability-based: patch it.

Even if it doesn't go further, each tier on its own is a publishable contribution — none of this
has been documented by anyone until now.

## Current status

Tiers 0-4 done. Codec2 hardware-encode gap root-caused and fixed across AMD/Intel/NVIDIA; a
standalone VA-API H.264 encoder proven working end to end on real hardware (verified decodable
output, not just successful API calls); the Tier 3 make-or-break question — whether a dma-buf
VA-API didn't allocate can be imported and correctly encoded from — confirmed across three real
GPUs (AMD APU, AMD discrete, Intel iGPU) with a synthetic buffer, then confirmed again against a
real, live dma-buf pulled out of a running redroid container's own gralloc-backed process; and a
real Codec2 hardware component (`c2.hardware.encoder.h264`) now registers and lists correctly on
a live redroid instance. **Tier 5 is done**: a host-side daemon reusing Tier 2/3's encode pipeline
unchanged (avoiding a much bigger Mesa/LLVM-for-bionic port) is wired all the way into the Tier 4
component, and **a real, unmodified app (`scrcpy`) now records correct, real hardware-encoded
H.264 video through the genuine Android framework path** (`MediaCodec` → `Codec2Client` → our
component → the daemon → VA-API/VCN) — not just "doesn't crash," visually confirmed correct
picture. Getting there took real work on top of the component itself: four separate bugs in why a
real app couldn't even see the encoder (HAL instance naming vs. the Framework Compatibility
Matrix, a missing boot flag, a missing `media_codecs.xml` entry, and `mediaserver`'s own codec-list
cache going stale before runtime config fixes took effect); then discovering the real buffer a
genuine `Surface`-based capture hands the encoder is RGBA, not NV12 (this Mesa/minigbm stack can't
allocate a buffer that's both GPU-renderable and real YUV at once — ruled out "get real YUV for
free" after checking, not assuming); and finally getting that RGBA buffer correctly into the
encoder: AMD VCN's DCC-compression restriction on real GPU-tiled buffers, a GPU-side RGBA→NV12
conversion stage using VA-API's own video post-processing (VPP) entrypoint (validated correct in
isolation with a known-color synthetic buffer before ever touching the real one again), and
determining the buffer's real (and un-queryable) GPU tiling modifier by reasoning through
minigbm's own allocator decision logic rather than guessing. See [DEVLOG.md](DEVLOG.md) for the
real progress, session by session.

## Contributing

No CLA, no friction — plain Apache-2.0. If this problem interests you, a PR or a comment on an
issue is worth more than asking for permission first.

## License

Apache License 2.0 — see [LICENSE](LICENSE). Same license as AOSP (`frameworks/av`), on
purpose: if any of this is eventually worth upstreaming, there's no legal friction.
