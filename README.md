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
- [ ] **Tier 3 — ⭐ The make-or-break spike.** Does a gralloc buffer export a `dma-buf` fd that
      VA-API can import zero-copy, inside a container with the same privileges as redroid?
      Nobody confirmed this in 4 years of issues. redroid runs as a container (not a VM) — no
      virtio-gpu in the way, a better starting point than the community seems to assume.
      **Mechanism confirmed working across three real GPUs** (AMD Renoir APU, AMD Polaris10
      discrete, Intel TigerLake-LP iGPU): an externally-allocated (non-libva) dma-buf imports into
      a VA-API surface and encodes correctly on all three — output byte-identical to a fully
      VA-API-native run in every case. Still open: confirming this against a *real* dma-buf pulled
      from redroid's own Android-side gralloc, not a generic DRM dumb buffer. See
      [tier3-dmabuf-import/README.md](tier3-dmabuf-import/README.md).
- [ ] **Tier 4 — Codec2 skeleton in Android.** A component that Android recognizes and lists
      (`dumpsys media.c2`) as `c2.hardware.encoder.h264`, without real encoding wired in yet.
- [ ] **Tier 5 — Real integration.** Tier 3 + Tier 2 wired into the callbacks of the Tier 4
      component. The actual goal.
- [ ] **Tier 6 (conditional on Tier 0).** If redroid/scrcpy's codec selection turns out to be
      hardcoded instead of capability-based: patch it.

Even if it doesn't go further, each tier on its own is a publishable contribution — none of this
has been documented by anyone until now.

## Current status

Tiers 0-2 done: Codec2 hardware-encode gap root-caused and fixed across AMD/Intel/NVIDIA, and a
standalone VA-API H.264 encoder proven working end to end on real hardware (verified decodable
output, not just successful API calls). Tier 3 — whether a dma-buf VA-API didn't allocate can be
imported and correctly encoded from — is confirmed working with a generic DRM buffer across three
real GPUs (AMD APU, AMD discrete, Intel iGPU); what's left is confirming the same result against a
real dma-buf pulled from redroid's own Android-side gralloc. See [DEVLOG.md](DEVLOG.md) for the
real progress, session by session.

## Contributing

No CLA, no friction — plain Apache-2.0. If this problem interests you, a PR or a comment on an
issue is worth more than asking for permission first.

## License

Apache License 2.0 — see [LICENSE](LICENSE). Same license as AOSP (`frameworks/av`), on
purpose: if any of this is eventually worth upstreaming, there's no legal friction.
