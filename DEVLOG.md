# Devlog

Session-by-session log — what was tried, what worked, what didn't, and why. See the [README](README.md)
for the tier-based roadmap.

## 2026-09-19 — Kickoff

Repo created. The 6-tier roadmap comes from actually investigating the state of the problem: the
official redroid-doc issues (#126, #172, #168, #535) have been open between 1 and 4 years, all
with the same answer from the maintainer ("the VA-API drivers are already there, you write the
Codec2 component") and no published result.

Important starting point that changes the difficulty calculus relative to what the community
seems to assume: redroid runs as a privileged Docker container, not a VM. That means there's no
virtio-gpu in the way at the container boundary — the kernel/DRM is literally the host's. The
Tier 3 spike (does a gralloc buffer export a dma-buf that VA-API can import zero-copy?) starts
from a better position than if redroid ran virtualized.

Next step: Tier 0 — confirm how redroid/scrcpy pick the encoder today, and which VA-API encode
entrypoints the real hardware actually supports.

## 2026-09-19 — Tier 0: more findings than expected

Read-only inspection of a real running redroid container (`redroid-jg-15:wifi-v3`, on an AMD
Radeon Vega "Cezanne"/Renoir APU host). No writes, no restarts — this instance is in active use.

**Hardware confirmed capable.** `vainfo` on the host (radeonsi driver, Mesa 25.2.8) reports full
H.264 (ConstrainedBaseline/Main/High) and HEVC (Main/Main10) hardware **encode**
(`VAEntrypointEncSlice`), not just decode. The GPU was never the blocker.

**Correction to a 2022-era assumption from the upstream issues.** This custom build does *not*
ship `libva`/`vainfo` inside the Android vendor partition at all — no `libva*.so`, no `vainfo`
binary anywhere under `/vendor`. The "VA-API drivers are already bundled" comment from
[remote-android/redroid-doc#172](https://github.com/remote-android/redroid-doc/issues/172)
(2022) apparently applied to the official images of that era, not to this build. Bundling libva
+ Mesa's VA-API Gallium state tracker into the vendor image is real, needed work — not assumed
to already be there.

**`OMX.redroid.h264.encoder` is a decoy.** `media_codecs.xml` declares a vendor-named encoder,
which made it look like there might be a hidden hardware path already. Traced it: it's just a
name registered in `libOmxCore.so`, backed by the same stock software AVC encoder
(`libstagefright_soft_avcenc.so`) everything else uses. Cosmetic branding, not a real lead.

**The actual find: unused boot-time Codec2 activation, already wired.** `/vendor/etc/init/`
has `redroid.c2.rc`, which on `ro.boot.use_redroid_c2=1` runs `redroid.c2.sh` — that script
checks for `/dev/dma_heap/system` or `/dev/ion` and, if present, sets
`debug.stagefright.ccodec=4` (full Codec2 rollout, Android's standard property for this). The
container already has `/dev/dma_heap/system` (confirmed present). Nobody is currently passing
`use_redroid_c2` when creating containers, so `debug.stagefright.ccodec` stays at `0`
(confirmed via `getprop` on the live instance) and everything falls back to legacy OMX,
software-only.

The full stock AOSP Codec2 client/service stack is already compiled into `/system`
(`libcodec2.so`, `libcodec2_client.so`, `libcodec2_aidl_client.so`,
`android.hardware.media.c2@1.x` / the AIDL `-V1-ndk` variant) — this is a normal, mostly-stock
AOSP build. No vendor-side Codec2 *component* is registered yet, but the client/service
plumbing Tier 4 would otherwise need to build doesn't need to be built — it's already there,
just unused because nothing turns it on.

**Also noted, lower priority:** `redroid.omx.rc` references `ro.boot.use_redroid_omx` and
`ro.boot.use_redroid_vaapi` boot properties, but grepping `/vendor` for code that reads the
resulting `sys.use_redroid_omx` / `sys.use_redroid_vaapi` properties came up empty. Either
vestigial, for a subsystem not present in this particular build, or read via a compiled-in
sysprop struct that doesn't show up as a grep-able literal string. Not chased further this
session.

**Next step:** spin up a disposable test instance (not a production one) with
`androidboot.use_redroid_c2=1` added to the boot command line, and check `dumpsys media.c2` /
`debug.stagefright.ccodec` after a fresh boot to see whether Google's stock software Codec2
components (e.g. `c2.android.avc.encoder`) show up. That's real signal on whether the Codec2
path is viable here before touching Tier 1+.
