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
- [x] **Tier 5 — ⭐ Real integration.** Tier 3 + Tier 2 wired into the callbacks of the Tier 4
      component. The actual goal, and it's done. **Architecture pivot**: porting Mesa's VA-API
      driver stack to build against Android's bionic turned out to need a full `radeonsi` +
      winsys + LLVM port (this build's Mesa only compiles the `gfxstream`/ANGLE/Vulkan
      forwarding-to-host pieces for Android, no native GPU driver at all) — realistically weeks of
      work on its own. Pivoted instead to a host-side encode daemon the Android component talks to
      over a Unix socket, reusing Tier 2/3's pipeline unchanged — see
      [tier5-vaapi-daemon/README.md](tier5-vaapi-daemon/README.md). **Confirmed working end to
      end, on three different GPUs across two vendors** (AMD Renoir, Intel Iris Xe, AMD Polaris —
      see the hardware compatibility table below): a real, unmodified app (`scrcpy`) records
      correct, real hardware-encoded H.264 through the genuine `MediaCodec`/`Codec2Client`
      framework path, visually confirmed correct picture, not just "doesn't crash." Getting there
      needed real per-GPU work (DCC handling, modifier-aware import, vendor-specific tiling
      selection, a legacy import path for pre-modifier hardware) — none of it touching the
      Android-side component, all of it in the host-side daemon. See `DEVLOG.md` for the full
      story, tier by tier.
- [x] **Tier 6 (conditional on Tier 0).** If redroid/scrcpy's codec selection turns out to be
      hardcoded instead of capability-based: patch it. **Turned out unnecessary** — confirmed
      throughout Tier 5 that selection is genuinely capability-based (`MediaCodecList`/
      `Codec2InfoBuilder`, the standard AOSP framework mechanism): `scrcpy` picks up
      `c2.hardware.encoder.h264` automatically once it's correctly registered, with no changes to
      redroid or scrcpy needed at all.
- [x] **Tier 7 — ⭐ Hardware video decode, and a fourth GPU (NVIDIA).** A new direction, not a
      continuation of Tier 0-6's encode work, and motivated by a sibling project
      ([redroid-nvidia](https://github.com/fogelmanjg/redroid-nvidia)) needing the same
      capability. Unlike encode, NVIDIA genuinely speaks VA-API for *decode* (`nvidia-vaapi-driver`,
      confirmed via `vainfo`: real `VAEntrypointVLD` for H.264/HEVC/VP9) - not a vendor-specific
      fork of this project's approach, the fourth GPU for the same mechanism. **Standalone spike
      first, bit-exact**: `tier6-vaapi-decode/main.c` (mirroring Tier 2's own rigor - a hand-written
      H.264 bitstream parser, no Android involved) decodes a real IDR frame via actual NVDEC
      hardware with output confirmed **byte-for-byte identical** to software decode via PSNR
      (`y:inf u:inf v:inf`), not just visually correct — six real bugs found and fixed on the way,
      including the two biggest: VA-API's slice-data buffer needs the NAL unit's own header byte
      included (not just the RBSP payload after it - found by reading `nvidia-vaapi-driver`'s own
      source after `NVD_LOG=1 NVD_LOG_VERBOSE=1` traced the failure to *before* any real decode
      call), and `chroma_qp_index_offset` parsed from the PPS but never carried through to the
      picture parameters (left luma bit-exact throughout while chroma stayed consistently, but not
      randomly, off). **Then folded into the real stack, same day**: `tier5-vaapi-daemon` now
      dispatches encode and decode as two independent backends on one socket (a 4-byte command tag
      picked before either request struct), and `codec2-component/VaapiDecComponent.{h,cpp}` is a
      real `c2.hardware.decoder.h264` component, the structural mirror of the existing encoder.
      Verified on two real hosts in one session: a GTX 1050 Ti (decode-only, no AMD/Intel GPU)
      served a real decode request through the daemon's own socket protocol bit-exact
      (`y:inf u:inf v:inf average:inf`), and an AMD Renoir APU (encode-only, no NVIDIA GPU)
      confirmed the new command tag doesn't disturb the existing encode wire format. See
      `DEVLOG.md`'s 2026-09-27 entries for the full trail, bug by bug and host by host. **Next**:
      the real-device test - driving `VaapiDecComponent` through the genuine `MediaCodec`/
      `Codec2Client` framework path, the way encode was eventually proven against a real
      `Surface`-fed `GraphicBufferSource`.

Even if it doesn't go further, each tier on its own is a publishable contribution — none of this
has been documented by anyone until now.

## Current status

Tiers 0-5 done. Codec2 hardware-encode gap root-caused and fixed across AMD/Intel/NVIDIA; a
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
minigbm's own allocator decision logic rather than guessing. That modifier turned out to be
GPU-generation-specific (confirmed extending to a second, different AMD GPU), so it now lives in
the host-side daemon (determined once at startup by asking that host's own driver directly) rather
than hardcoded into the Android-side component — a new host GPU only needs this small daemon
recompiled locally, never a cross-machine AOSP rebuild. **Confirmed on a second GPU vendor**:
Intel Iris Xe, using the *identical* unmodified vendor image built for AMD — one more real,
vendor-specific bug (Intel's driver prefers tiled over linear for this usage class, unlike AMD,
so naively trusting the first modifier the driver reports was silently wrong), fixed the same way,
in the daemon only. **Then confirmed on a third GPU, an older AMD generation this project had
previously written off as blocked** (Radeon RX 480/Polaris, GFX8): that GPU can't describe its
tiling as a DRM modifier at all, and a GL-based bridge that seemed to solve it turned out to be a
Mesa same-process shortcut, not a real fix — the actual answer was simpler than any of that,
VA-API's own *old*, pre-modifier import path resolves this GPU's opaque tiling correctly even
across processes, unlike EGL's equivalent. Tier 5 is now 3-for-3 on every GPU this project has
tried. See [DEVLOG.md](DEVLOG.md) for the real progress, session by session.

**Tier 7 (hardware decode, NVIDIA) done as of 2026-09-27**: unlike encode, NVIDIA speaks real
VA-API decode (`nvidia-vaapi-driver`) - bit-exact output (`PSNR y:inf u:inf v:inf`) from real
NVDEC hardware, first in a standalone spike and then through the real stack: `tier5-vaapi-daemon`
dispatches encode and decode as two independent backends on one socket, and
`c2.hardware.decoder.h264` (`VaapiDecComponent`) is a real Codec2 component alongside the existing
encoder. Verified on two real hosts (a decode-only NVIDIA GPU and an encode-only AMD GPU) in the
same session. See the roadmap entry above and DEVLOG.md's 2026-09-27 entries for the details.

## Hardware compatibility

Real results on real hardware, not speculation. "Tier 5" here means the full stack: a real,
unmodified app records correct video through the genuine Android framework path. "Tier 3 only"
means the underlying import+encode mechanism was proven on that GPU with a synthetic buffer, but
the full real-app pipeline hasn't been (or can't yet be) run on it.

| GPU | Architecture | Status | Notes |
|---|---|---|---|
| AMD Renoir (Ryzen APU, integrated) | GFX9 | **Tier 5 — full pipeline works** | Reference implementation. Needed `AMD_DEBUG=nodcc` (VCN can't encode DCC-compressed surfaces) and a modifier-aware (`DRM_PRIME_2`) import once a real Surface-sourced buffer turned out to be GPU-tiled. |
| AMD Radeon RX 480 (Polaris, discrete) | GFX8 | **Tier 5 — full pipeline works** | Mesa reports **zero** DRM format modifiers for this GPU/format/usage combo at all — minigbm falls back to opaque, non-modifier-describable tiling (`TILE_TYPE_DRI`) that `DRM_PRIME_2` has no way to express. A GL-based bridge (import via plain EGL, GPU-blit into a fresh linear buffer) seemed to work but turned out to only resolve the tiling within the *same process* that allocated the buffer — a Mesa shortcut, not a real fix, confirmed broken for the actual cross-process case. The real fix: VA-API's own *old*, pre-modifier import path (`VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME`) resolves this GPU's opaque tiling correctly even across processes, unlike EGL's equivalent — libva is built around cross-process IPC as the normal case to begin with. Confirmed with a real GPU-tiled buffer handed cross-process, then with the real app. |
| Intel Iris Xe (TigerLake-LP, integrated) | Gen12 | **Tier 5 — full pipeline works** | Same unmodified vendor image as the AMD reference, no rebuild needed. Hit one real vendor-specific bug: this GPU's driver prefers Y-tiled over linear for the real buffer's usage class (confirmed by reading minigbm's `i915.c` — unlike AMD, Intel registers linear/X-tiled/Y-tiled at different explicit priorities, and tiled always wins for this usage when no CPU-read/write hint is set), so blindly trusting the first modifier the driver reports (which happened to work for AMD) silently picked the wrong one. Fixed with vendor-string detection in the daemon. |

This list is 3 machines because that's what's in this project's own reach, not a completeness
claim. If you've got other hardware (a different AMD generation, a different Intel generation)
and want to help fill this in, see Contributing below — a PR against this table with your own
findings is exactly the kind of contribution this project can use.

### Decode (Tier 7, NVIDIA)

A separate table on purpose — decode is a different pipeline (`tier5-vaapi-daemon`'s own decode
backend, `nvidia-vaapi-driver`/NVDEC via VA-API) from the encode table above, and currently
NVIDIA-only (see "NVIDIA isn't in this table" below for why encode is a different story).

| GPU | Architecture | Status | Notes |
|---|---|---|---|
| GTX 1050 Ti | Pascal | **Confirmed bit-exact** | The Tier 7 reference hardware — see the roadmap entry and `DEVLOG.md`'s 2026-09-27 entries. Standalone spike and the full daemon/Codec2 integration both verified against software decode via PSNR (`y:inf u:inf v:inf average:inf`), not just visually correct. |
| RTX 4060 | Ada Lovelace | **Not yet run through this project's own decode path** | This project's own VA-API decode code hasn't actually been exercised on this GPU yet — the two-GPU confirmation above is for redroid-nvidia's own 3D acceleration/NVENC work (see below), a separate pipeline. Reasonable to assume this works too, since decode goes through the same `nvidia-vaapi-driver`/VA-API mechanism on both GPUs and that mechanism doesn't touch the specific driver-level limitation found on this card (see below) — but per this project's own "real results, not speculation" standard, treat this row as **unverified** until it's actually run and PSNR-checked here, the same way Pascal was. |

**NVIDIA isn't in the encode table above** because this project's own VA-API encode mechanism
doesn't apply to NVIDIA at all — `nvidia-vaapi-driver` is decode-only (`vainfo` lists zero
`VAEntrypointEncSlice` entries), not a matter of this GPU needing more work to catch up to
AMD/Intel here. That said, NVIDIA is **not** stuck without acceleration or encode overall: the
sibling project, [**redroid-nvidia**](https://github.com/fogelmanjg/redroid-nvidia), built its own
path to both — a custom Venus-proxy 3D acceleration pipeline (bypassing the still-broken native
`gpuMode=host`/Mesa-GBM path entirely) confirmed booting to a real, rendered Android home screen
with real GPU acceleration on **both** a GTX 1050 Ti and an RTX 4060, and real hardware video
encode via NVENC (a different mechanism from this project's VA-API daemon, since NVIDIA doesn't
expose encode through VA-API) confirmed producing correct, decodable H.264 on both GPUs too. One
open item there as of 2026-09-27: a low-severity scanline-dropout artifact on the RTX 4060
specifically, root-caused to a real driver-level limitation (a "prime fence" import failure) in
driver 595.91.07 on Ada Lovelace — confirmed absent on the same 1050 Ti with an older driver, and
confirmed to also affect Android's own compositor independent of NVENC, so it's a driver gap, not
a bug in either project's own code. See that repo's own README/DEVLOG for the full trail.

## Contributing

No CLA, no friction — plain Apache-2.0. If this problem interests you, a PR or a comment on an
issue is worth more than asking for permission first.

## License

Apache License 2.0 — see [LICENSE](LICENSE). Same license as AOSP (`frameworks/av`), on
purpose: if any of this is eventually worth upstreaming, there's no legal friction.
