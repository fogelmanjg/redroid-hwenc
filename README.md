# redroid-hwenc

**Languages:** English | [Español](README.es.md) | [中文](README.zh-CN.md)

> This project's official language is **English**. The other language files are provided for
> convenience and may not be perfectly accurate or up to date — if in doubt, this file is the
> source of truth.

Hardware video encoding (H.264/H.265, via VA-API) for [redroid](https://github.com/remote-android/redroid-doc).

> **Just want this working?** This project's code is what powers the `hwenc`/`hwdec` modules of
> [**redroid-forge**](https://github.com/fogelmanjg/redroid-forge), a full Android-container
> manager built on top of it (GApps, ARM translation, WiFi, backups, a real UI). If you want a
> working instance, start there — this repo is where the underlying reverse-engineering gets done
> and documented, tier by tier, for anyone who wants to understand or build on the research
> itself, not just use the result.

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

Each tier assumes the previous one is done. ⭐ marks the highest-leverage checkpoint. This list
says what was expected and what came of it — for the actual bugs, dead ends, and how each result
was reached, see [DEVLOG.md](DEVLOG.md) (session by session) and, where one exists, the tier's own
subfolder README, linked below.

- [x] **Tier 0 — Pure research.** Confirmed codec selection is capability-based, not hardcoded, and real VA-API encode entrypoints exist on this hardware.
- [x] **Tier 1 — Reading/mapping.** Mapped a Codec2 component's structure and the VA-API encode API surface.
- [x] **Tier 2 — Isolated native prototype.** Standalone host program encodes real H.264 via VA-API. → [tier2-vaapi-encode/](tier2-vaapi-encode/)
- [x] **Tier 3 — ⭐ The make-or-break spike.** Confirmed a gralloc dma-buf imports zero-copy into VA-API inside a real redroid container — across three GPUs — the exact question 4 years of upstream issues never answered. → [tier3-dmabuf-import/README.md](tier3-dmabuf-import/README.md)
- [x] **Tier 4 — Codec2 skeleton in Android.** A component Android recognizes and lists via `dumpsys media.c2`, no real encoding wired in yet. → [tier4-codec2-skeleton/README.md](tier4-codec2-skeleton/README.md)
- [x] **Tier 5 — ⭐ Real integration. The actual goal.** A real, unmodified app (`scrcpy`) records correct hardware-encoded H.264 through the genuine framework path — confirmed on 3 GPUs across AMD/Intel (see Hardware compatibility below). Later additions: real P frames (5.14, broken on Intel), vendor-agnostic hardware decode for apps (5.15), and a second, libavcodec-based encode backend that fixes the Intel P-frame bug (5.16). → [tier5-vaapi-daemon/README.md](tier5-vaapi-daemon/README.md)
- [x] **Tier 6 (conditional on Tier 0).** Turned out unnecessary — Tier 0 already confirmed codec selection is capability-based, so `scrcpy` picks up the new encoder automatically.
- [x] **Tier 7 — ⭐ Hardware video decode, a fourth GPU (NVIDIA).** Bit-exact NVDEC decode via VA-API, fully wired into the daemon and a real `c2.hardware.decoder.h264` Codec2 component (see Hardware compatibility below). **Next**: the real-device `MediaCodec` test, the same milestone Tier 5 needed for encode. → [tier6-vaapi-decode/README.md](tier6-vaapi-decode/README.md), [tier5-vaapi-daemon/README.md](tier5-vaapi-daemon/README.md)
- [x] **Tier 8 — Android 11 (HIDL Codec2).** AIDL Codec2 (what Tiers 4–7 are built on) does not exist before Android 14 — confirmed against AOSP source at android-11.0.0_r48, 12.0.0_r34 and 13.0.0_r83. A HIDL port of the same component, plus a kernel-level `libion.so` fix these older AOSP versions need on any modern host kernel, gets the same hardware encode working — **confirmed end to end on Android 11, on both AMD (Ryzen 5700G/Renoir) and Intel (Iris Xe)**. Intel needed the `ffmpeg` encode backend (5.16) to fix a malformed-bitstream bug (`Overread SPS by N bits`) on the default backend — confirmed 09/11/2026 to be the same class of Intel-specific issue as 5.14's P-frame bug, not a separate one as first suspected. **Android 12/13 are assumed to work the same way, not separately tested** — same AOSP-source gap, same fix, no version-specific logic in the component itself, so there's no reason to expect it to behave differently there. It just hasn't been run on real 12/13 hardware yet. If you do and hit something that doesn't work, that's exactly what the assumption could be missing — open an issue with what you saw and it'll be looked at specifically. The HIDL decode side (`VaapiDecComponent`) also hasn't been exercised yet, same situation. → [tier8-android11-13-hidl/README.md](tier8-android11-13-hidl/README.md)

Even if it doesn't go further, each tier on its own is a publishable contribution — none of this
has been documented by anyone until now.

## Hardware compatibility

Real results on real hardware, not speculation. "Full pipeline" means a real, unmodified app
records correct video through the genuine Android framework path — see the linked tier READMEs
and DEVLOG.md for what each GPU's own fix actually was.

| GPU | Architecture | Status | Notes |
|---|---|---|---|
| AMD Renoir (Ryzen APU, integrated) | GFX9 | **Full pipeline works** | Reference implementation. P frames (5.14) work; `ffmpeg` backend (5.16) also validated here. |
| AMD Radeon RX 480 (Polaris, discrete) | GFX8 | **Full pipeline works** | No DRM modifier available for this GPU/format at all — needed VA-API's old pre-modifier import path. P frames (5.14) work. **Avoid the `ffmpeg` backend (5.16) here**: real GPU hangs during validation (timeout, full reset, one needing a host reboot) — stay on the default (`legacy`) backend. |
| Intel Iris Xe (TigerLake-LP, integrated) | Gen12 | **Full pipeline works** | Same unmodified vendor image as AMD, no rebuild. One vendor-specific tiling bug, fixed in the daemon. P frames on the default (`legacy`) backend are **broken** (5.14: ~60% of frames fail to decode) — use the `ffmpeg` backend (5.16) for working P frames here. |
| Intel Haswell / HD Graphics 4400 (i965, integrated) | Gen7.5 | **Unconfirmed** | Community-contributed, ported from [artempavlov/redroid-hwenc](https://github.com/artempavlov/redroid-hwenc) — no Haswell hardware in this project's own reach to verify it on. i965 profile, cros/minigbm handle adaptation, a tested v0.2.0 binary patcher; see [intel/README.md](intel/README.md) for the actual build/verification status, which is more limited than this table's usual bar. |

Vendor-agnostic hardware **decode for apps** (5.15, `hwdec.c` — H.264/HEVC/VP9, separate from the
encode table above and from Tier 7's NVIDIA-only decode): confirmed bit-exact against software
decode on all three GPUs above, see [tier5-vaapi-daemon/README.md](tier5-vaapi-daemon/README.md#515-vendor-agnostic-hardware-decode-hwdecc).

This list is 3 machines because that's what's in this project's own reach, not a completeness
claim. If you've got other hardware and want to help fill this in, see Contributing below — a PR
against this table with your own findings is exactly the kind of contribution this project can use.

### Decode (Tier 7, NVIDIA)

A separate table — decode is a different pipeline (`tier5-vaapi-daemon`'s decode backend,
`nvidia-vaapi-driver`/NVDEC via VA-API) from the encode table above, and currently NVIDIA-only.

| GPU | Architecture | Status | Notes |
|---|---|---|---|
| GTX 1050 Ti | Pascal | **Confirmed bit-exact** | Verified via PSNR against software decode (`y:inf u:inf v:inf average:inf`), through the real daemon/Codec2 stack, not just the standalone spike. |
| RTX 4060 | Ada Lovelace | **Not yet run here** | This project's own decode code hasn't actually been exercised on this GPU yet. Reasonable to assume it works (same mechanism as Pascal above), but per this project's own "real results, not speculation" standard, treat it as unverified until it's actually run and PSNR-checked. |

**Why NVIDIA isn't in the encode table above**: `nvidia-vaapi-driver` is decode-only (`vainfo`
lists zero `VAEntrypointEncSlice` entries) — a mechanism mismatch, not a maturity gap. NVIDIA is
**not** stuck without acceleration or encode overall, though: both live in the sibling project,
[**redroid-nvidia**](https://github.com/fogelmanjg/redroid-nvidia), via a different mechanism
(a custom Venus-proxy pipeline + NVENC) — confirmed working on both GPUs in the table above, with
one open, GPU-specific driver limitation on the RTX 4060 (see that repo for the detail).

## Contributing

No CLA, no friction — plain Apache-2.0. If this problem interests you, a PR or a comment on an
issue is worth more than asking for permission first.

## License

Apache License 2.0 — see [LICENSE](LICENSE). Same license as AOSP (`frameworks/av`), on
purpose: if any of this is eventually worth upstreaming, there's no legal friction.
