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

## 2026-09-19 (same day) — Tier 0 continued: Codec2 activates, but with an empty component list

Booted a disposable throwaway instance (`redroid-jg-15:gapps-official`, not a production one)
with `androidboot.use_redroid_c2=1` added to the boot command line.

**The flag works exactly as found earlier.** After boot: `debug.stagefright.ccodec=4`,
`ro.boot.use_redroid_c2=1`, and a real Codec2 AIDL service is registered and running:
`android.hardware.media.c2.IComponentStore/software` (`service list` confirms it, backed by the
stock `mediaswcodec` process from the `com.android.media.swcodec` APEX, which *is* present and
mounted — full `libcodec2_soft_*.so` set for every standard codec, including
`libcodec2_soft_avcenc.so`, and a complete, unmodified `media_codecs.xml` inside the APEX
correctly declaring `c2.android.avc.encoder` with `<Alias name="OMX.google.h264.encoder" />`).
Everything a stock Android 15 device would have is genuinely there.

**But it reports zero components.** `dumpsys android.hardware.media.c2.IComponentStore/software`
returns:

```
Supported components:
    NONE
Active components:
    NONE
```

**Root cause, found in the service's own logcat (PID of `media.swcodec`):**

```
E CodecServiceRegistrant: listComponents -- not supported.
```

The AIDL `ComponentStore` starts fine and responds to `dump`, but the piece responsible for
walking `media_codecs.xml` and actually registering each declared component
(`CodecServiceRegistrant`) explicitly declines — not a crash, not a missing file, a deliberate
"not supported" from `libmedia_codecserviceregistrant.so`. Only one copy of that library exists
in the whole system (inside the swcodec APEX itself) — there's no device/vendor-specific
registrant overriding it. Reading this as: this AOSP fork ships the generic/default registrant
stub and never wires a real one, so the software Codec2 store — despite being 100% present and
otherwise correctly configured — never actually exposes any component to the rest of the
framework.

This is one layer *before* the hardware question. Before a hardware Codec2 component (Tier 4)
can matter, the basic software Codec2 path needs a working `CodecServiceRegistrant`. Worth
checking whether this is specific to this particular build/lunch target, or a general redroid
(or even AOSP-without-a-real-device) gap — and whether AOSP's `frameworks/av` has a usable
generic registrant implementation that just isn't linked in, or whether one needs to be written.

**Next step:** find (or write) a working `CodecServiceRegistrant` implementation and confirm the
software Codec2 path actually lists components end to end — that's the real prerequisite for
Tier 4, more fundamental than originally scoped.

## 2026-09-19 (same day) — Tier 0 resolved: found and fixed the empty component store

Traced `CodecServiceRegistrant.cpp` against real AOSP source (checked out via a mirror, not
memory — this stuff is easy to misremember). The "not supported" `listComponents()` belongs to
`H2C2ComponentStore`, a thin adapter class — and it gets constructed with a **null** backing
store (`std::make_shared<H2C2ComponentStore>(nullptr)`) specifically when
`RegisterCodecServicesWithExistingThreadpool()` decides `aidlSelected` is false. When that
happens, the AIDL binder name `android.hardware.media.c2.IComponentStore/software` still gets
registered (because the VINTF manifest declares it should exist) — just wired to nothing. That's
exactly the "process running, service present, zero components" state from the previous entry.

`aidlSelected` comes from `c2_aidl::utils::IsSelected()` → `android::IsCodec2AidlHalSelected()`
(`frameworks/av/media/codec2/hal/common/HalSelection.cpp`), which requires **both**:

1. Either the `codec_fwk/aidl_hal` aconfig flag is on, **or** `ro.vendor.api_level >= 202404`
   (a newer, date-based vendor API level scheme — unrelated to `ro.build.version.sdk`).
2. The property `media.c2.hal.selection` is exactly `"aidl"` (defaults to `"hidl"`).

On this build, `ro.vendor.api_level` is `34` (old-style board API level, nowhere near
`202404`), and the aconfig flag isn't set — so the very first condition returns `false`
immediately, and the software store never wires up.

**Fix, confirmed working on a disposable test instance:**

```sh
device_config put codec_fwk aidl_hal true
setprop media.c2.hal.selection aidl
# then restart the service so RegisterCodecServices() re-runs with the new values:
kill -9 $(ps -A | awk '/media\.swcodec/{print $2}')
```

After that, `dumpsys android.hardware.media.c2.IComponentStore/software` goes from `NONE` to a
real dump: **32 registered components**, `android.componentStore.platform`, including
`c2.android.avc.encoder` and `c2.android.hevc.encoder`.

This isn't the hardware encoder yet — these are still the stock software Codec2 encoders. But
it's the actual prerequisite Tier 4/5 depend on: there was no working Codec2 *framework* to
register a hardware component into. There is now. The same `aidl_hal`/`media.c2.hal.selection`
gate will very likely also apply to whatever store name a future hardware component gets
registered under (e.g. a `.../vendor` instance) — now we know exactly how to satisfy it instead
of hitting the same "service present, zero components" dead end again.

**Open question for later:** whether this needs to be set on every boot (boot script /
`redroid.c2.sh` addition) or can be baked into `media_codecs.xml`/build config permanently.
Not chased tonight — noting it so it isn't lost.

## 2026-09-19 (same day) — Cross-hardware validation: AMD Vega APU → AMD Polaris discrete GPU

Ran the exact same test on a second, different machine: an AMD Radeon RX 480 (Polaris10,
discrete, not an APU), on a Debian 13 host with its own quirk — this kernel doesn't have
`CONFIG_ANDROID_BINDERFS` compiled in at all (confirmed via `/boot/config-$(uname -r)`), unlike
the first host. `binder_linux` was already loaded there with fixed legacy device nodes
(`devices=binder,hwbinder,vndbinder,binder1,...,binder2,...` module parameter) instead of
binderfs — an older-style setup. Used one of the two free legacy slots (a third was already in
active use by a real running instance on that host; left it untouched).

`vainfo` on this GPU: same result shape as the Vega APU — full H.264 (Baseline/Main/High) and
HEVC Main hardware **encode** (`VAEntrypointEncSlice`).

Applied the identical fix from the previous entry (`device_config put codec_fwk aidl_hal true`
+ `setprop media.c2.hal.selection aidl` + restart `media.swcodec`) on a disposable test instance
on this second machine. **Identical result:** 0 → 32 components, same `c2.android.avc.encoder`
and `c2.android.hevc.encoder`. This isn't a quirk of one particular AMD chip or kernel build —
it's a generic redroid/AOSP build configuration gap, reproducible across an APU and a discrete
GPU, two different kernels, two different binder setups (binderfs vs. legacy fixed nodes).

Next hardware to try: an NVIDIA GPU (4060), to see whether the same gap and fix apply there too,
or whether NVIDIA's proprietary driver path changes anything in this part of the picture (it
shouldn't — this fix is entirely on the Android/Codec2 side, before VA-API/the GPU driver even
enters the picture — but worth confirming rather than assuming).

## 2026-09-19 (same day) — NVIDIA: real architecture finding, plus an unrelated blocker

Set up a fresh machine from scratch for this one: Docker CE, `nvidia-container-toolkit`
(confirmed working — `docker run --gpus all ... nvidia-smi` succeeds inside a container), and
`nvidia-vaapi-driver` (Debian packages it: `nvidia-vaapi-driver 0.0.13-1`).

**Real finding, worth designing around now rather than discovering later:** unlike Mesa
(AMD/Intel), which bundles VA-API encode support automatically, `nvidia-vaapi-driver` is
**decode-only**. `vainfo` with it loaded lists plenty of decode profiles
(H.264/HEVC/VP8/VP9/AV1, all `VAEntrypointVLD`) and **zero** `VAEntrypointEncSlice` entries. The
RTX 4060 (Ada Lovelace) obviously has strong hardware encode — but it's exposed through NVENC,
NVIDIA's own proprietary API, not through VA-API. This means the future hardware Codec2
component (Tier 4/5) can't be a single VA-API backend for every vendor — it needs a vendor
split: VA-API for AMD/Intel, a separate NVENC-based path for NVIDIA. Worth stating explicitly in
the roadmap once Tier 1 starts taking real shape.

**Unrelated blocker, not chased to a conclusion tonight:** redroid itself won't boot on this
particular fresh install. `vold` and `blank_screen` — and only those two, both perfectly normal
64-bit x86-64 PIE binaries with the same `/system/bin/linker64` interpreter as everything else —
fail with `cannot execv(...): No such file or directory` moments after their process is forked,
while `hwservicemanager`/`servicemanager` start fine. Reproduces identically with `--gpus all`
removed and with `androidboot.redroid_gpu_mode=guest` (pure software rendering, no GPU
dependency at all) — so it's not GPU/NVIDIA-toolkit related. Ruled out: image corruption (layer
ID matches the source host exactly), binder device permissions (fixed a real `chmod 666` miss
along the way, different bug, didn't fix this one), storage driver (identical overlay2-on-btrfs
setup works fine on another machine), 32-bit/IA32 emulation (both binaries are 64-bit; kernel
has `CONFIG_IA32_EMULATION=y` anyway), and container-level AppArmor confinement (profile is
correctly `unconfined` for this `--privileged` container). Leading suspicion: a mount-namespace
propagation quirk specific to this fresh Debian trixie + Docker 29.8.1 + containerd 2.3.5
install, since both failing services do their own mount-namespace work at start. Not resolved —
parking it here so the investigation isn't lost, separate from the (already answered) VA-API
question above.

## 2026-09-19 (same day) — Hunted the boot blocker down: two missing host kernel modules, then the real NVIDIA issue

Picked this back up on a second, independent fresh host (a GTX 1050 Ti machine, also a from-
scratch Docker + nvidia-container-toolkit install) specifically to tell apart "quirk of one
machine" from "something structural." Same crash, identical `cannot execv` signature. Confirmed
this had nothing to do with Docker/containerd version either — downgraded to the exact versions
running on the machine where redroid boots fine (Docker 29.7.2, containerd 2.3.3); crash
persisted unchanged.

The real trail was earlier in `dmesg`, above where the `execv` errors show up (easy to miss if
you only grep for the visible symptom):

```
apexd-bootstrap: Failed to activate .../com.android.tzdata.apex: Could not create loop device
  for .../com.android.tzdata.apex: Failed to open loop-control: No such device
init: Service apexd-bootstrap has 'reboot_on_failure' option and failed, shutting down system.
```

`apexd-bootstrap` — the thing that mounts Android's APEX modules, itself a hard boot dependency
— was failing because **the `loop` kernel module wasn't loaded on the host.** `/dev/loop-control`
existed as a stale device node, but nothing backed it (`lsmod | grep loop` was empty), so opening
it returned ENODEV. That failure trips `apexd-bootstrap`'s `reboot_on_failure`, which makes
Android's init do its own internal soft-reboot — and it's numbers from a strictly worse state
*after* that soft-reboot that produce the `vold`/`blank_screen` `execv` errors seen in the
earlier entries. Those were downstream noise, not the cause.

`modprobe loop` got loop devices created, but the APEX *mount* still failed
(`Mounting failed for .../com.android.tzdata.apex: No such device`) — this time because
**`ext4` wasn't registered as a filesystem in the kernel either** (`cat /proc/filesystems` had
no `ext4` line at all, vs. a working host which listed it with `ext4`/`mbcache`/`jbd2` loaded).
`modprobe ext4` fixed that too.

With both modules loaded, redroid boots almost all the way: `vold`, `apexd`, `adbd`,
`gpuservice`, the graphics composer service — all come up clean. **The actual remaining failure
is now squarely an NVIDIA/Mesa incompatibility, not a host config gap:**

```
MESA: Using gralloc header from libdrm/android/gralloc_handle.h. [...] Initializing a fallback
  gralloc as a helper: Using fallback gralloc implementation
libc: Fatal signal 6 (SIGABRT) [...] in tid ... (surfaceflinger)
  #03 SkiaGLRenderEngine::chooseEglConfig
  #04 SkiaGLRenderEngine::create
```

redroid's own vendor gralloc (`gralloc.redroid.so`, built against Mesa/GBM assumptions) isn't
being used — Mesa's client library falls back to a generic gralloc implementation instead, and
SurfaceFlinger's Skia-based render engine then aborts trying to pick an EGL config against
whatever that fallback actually hands it through NVIDIA's proprietary EGL/GL stack. This crashes
`surfaceflinger` in a loop (`exited 4 times before boot completed`), which is why
`sys.boot_completed` never gets set.

**Where this leaves NVIDIA support:** it's not a missing package or a config flag this time — it's
a real vendor HAL incompatibility between redroid's Mesa-oriented gralloc/hwcomposer and NVIDIA's
driver stack. `gpuMode=guest` (pure software rendering, no GPU driver in the loop) should sidestep
this entirely and is worth confirming as a fallback; getting `gpuMode=host` genuinely working on
NVIDIA would mean either an NVIDIA-aware gralloc/hwcomposer HAL (nobody seems to have published
one for redroid) or something narrower fixing just this EGL config negotiation — not yet
investigated further.

**Worth carrying back to `redroid-manager`'s Doctor, independent of the NVIDIA question:** missing
`loop`/`ext4` kernel modules is a real, generic footgun for anyone deploying redroid on a host
that's never needed them before — cheap, high-value checks to add.

## 2026-09-19 (same day) — NVIDIA options, tried in order: guest mode works, two real sub-bugs found, core issue confirmed

Went through the cheap options before assuming a full vendor HAL rewrite is necessary.

**`gpuMode=guest` (pure software rendering) confirmed working on NVIDIA** — boots clean in ~14s,
completely sidesteps the gralloc/EGL crash since no GPU driver is involved at all. Not a real fix
(no hardware acceleration), but a legitimate practical fallback, and useful confirmation that
everything *else* (loop/ext4/binder/Codec2) is solid on this hardware — the remaining problem is
narrowly scoped to `gpuMode=host`'s graphics HAL.

**Checked whether NVIDIA's GBM/Vulkan pieces are actually reaching the container** (they need to,
for any Mesa-side fix to have a chance): confirmed complete. `nvidia-container-toolkit`'s CDI
spec (`/var/run/cdi/nvidia.yaml`) injects `libnvidia-egl-gbm.so`, `nvidia-drm_gbm.so`, the EGL
external platform configs, and the Vulkan ICD (`nvidia_icd.json`) into the container. Nothing
missing here — ruling this out as the cause narrows the problem to an actual negotiation failure,
not an incomplete environment.

**Tried forcing Zink** (Mesa's OpenGL-over-Vulkan driver, hoping to route around NVIDIA's
native EGL/GBM path entirely) via `MESA_LOADER_DRIVER_OVERRIDE=zink` / `GALLIUM_DRIVER=zink` env
vars on the container. Inconclusive as a fix — the crash signature didn't change, and log
evidence suggests these env vars don't actually get honored by the code path in play here
(Android's `platform_android` EGL backend, not a normal desktop Mesa app), so this wasn't a real
test of Zink, just a reminder that the override point needs to be found more precisely if this
route gets revisited.

**Found and fixed a real, separate bug along the way:** the crash logs (once actually read
closely, past the vold/blank_screen detour) showed `EGL-MAIN: failed to open
/dev/dri/renderD128: Permission denied` before the "fallback gralloc" messages. The device node
`--gpus all` creates inside the container is `crw-rw---- root:992` — and `surfaceflinger` runs as
Android's `system` AID, which isn't a member of that group inside the container's (Android-only)
user model. Chmod'ing the device from the *host* has no effect (nvidia-container-toolkit
recreates the node fresh inside the container, not a true bind-mount of the host's permission
bits) — fixed it with `docker exec ... chmod 666 /dev/dri/*` from inside the running container
instead.

**With that permission bug fixed, the actual root cause is confirmed, cleanly, with no more
noise in the way:** `RenderEngine: no suitable EGLConfig found, giving up`, in
`SkiaGLRenderEngine::chooseEglConfig`. Device access is no longer the problem — this is a genuine
EGL config negotiation failure between whatever Mesa's Android-platform fallback gralloc offers
and what NVIDIA's EGL implementation is willing to hand back. redroid's own vendor gralloc
(`gralloc.redroid.so`) apparently isn't even the one being used here (Mesa's log explicitly says
it's using a "fallback gralloc" instead) — so there are really two layered questions now: (1) why
does `gralloc.redroid.so` decline to handle NVIDIA in the first place, and (2) why does Mesa's
own fallback then fail to agree on an EGL config with NVIDIA specifically. Neither answered yet.

**Where this leaves it:** the cheap options (1 and 2) are exhausted and didn't produce a fix —
this really does look like it needs actual HAL-level work (option 3 from the plan), not a
config/env-var nudge. `gpuMode=guest` stays the practical fallback for NVIDIA hosts in the
meantime. Given the actual goal of this repo is encode (NVENC, unrelated to this rendering path),
not full 3D acceleration, this doesn't block Tier 1+ — parking NVIDIA `gpuMode=host` as a
separate, harder side quest rather than a blocker.

## 2026-09-19 (same day) — Prior art exists, but it doesn't fit redroid's headless model

Before assuming a HAL rewrite from scratch, searched for whether anyone solved the equivalent
problem in a similar project. They have:
[**waydroid-nvidia**](https://github.com/Shiro836/waydroid-nvidia) gets full GPU-accelerated
Android-in-container on NVIDIA working — real, verified (Minecraft Bedrock, 2ms present-to-present
latency benchmarks). The approach: proxy Vulkan (Mesa Venus) from the Android guest over a unix
socket to a host-side renderer, allocate buffers host-side as NVIDIA block-linear images, and
hand them to the consumer as native NVIDIA dmabufs — no cross-vendor EGL/gralloc negotiation at
all, which is exactly the class of problem we hit.

**The catch that matters for us:** this architecture requires a real Wayland compositor (KWin/
Plasma, verified) already running on the host — the Android container renders *through* that
existing desktop session, it isn't headless. That's the opposite of what `redroid-manager` is
for (a host with nothing but Docker, no desktop session required). Porting this approach as-is
would mean giving up headless operation specifically on NVIDIA hosts — a real, new constraint
that doesn't exist for AMD/Intel today.

Other requirements worth noting if this gets revisited: `nvidia-open`/`nvidia-open-dkms` kernel
modules specifically (not the classic proprietary blob) — not yet confirmed which one is running
on the test machines. Driver 595.71+ with `nvidia-drm.modeset=1`. Turing (RTX 20/GTX 16) or
newer GPUs — the GTX 1050 Ti used for our tests is Pascal, older than what's verified to work;
the RTX 4060 (Ada) would qualify. Separately, NVIDIA's developer forums note they don't build
`nvidia-utils` against bionic libc — a known ABI friction point in this space generally, distinct
from our specific bug but the same swampy territory.

Not a drop-in fix, but the most concrete lead that exists for this specific problem today.

## 2026-09-19 (same day) — Intel iGPU: clean pass, confirms the AMD/Intel-vs-NVIDIA split

Third GPU vendor, third architecture family: Intel Iris Xe (TigerLake-LP), integrated. Fresh
machine, same drill as before (`modprobe loop ext4` up front this time, learned that lesson).

**`gpuMode=host` boots clean, first try** — no gralloc/EGL crash at all, unlike NVIDIA. Confirms
the earlier read: this is specifically an NVIDIA-vs-Mesa problem, not a general "any non-AMD GPU"
problem. Intel, like AMD, goes through Mesa's real DRI/GBM path and redroid's own vendor gralloc
handles it fine.

**`vainfo` on the host:** rich H.264 (Main/High/ConstrainedBaseline) and HEVC support (Main/
Main10/Main12/Main422/Main444, including SCC profiles) — broader HEVC coverage than the AMD
hosts tested so far. One naming detail worth remembering for Tier 2: Intel reports the encode
entrypoint as **`VAEntrypointEncSliceLP`** (low-power path), not `VAEntrypointEncSlice` like AMD.
Any capability probe needs to check for both, not just one.

**Codec2 `aidl_hal` fix confirmed a third time**, identical result to AMD APU and AMD discrete:
0 → 32 components, `c2.android.avc.encoder` and `c2.android.hevc.encoder` present. Same fix,
same mechanism, three completely different GPU vendors/architectures. This part of the roadmap
(Tier 0's Codec2 registration prerequisite) is now about as validated as it can get without
touching AOSP source — safe to treat as solved and move on from.

**State of play across all vendors tested:**
- AMD (Vega APU, Polaris discrete): full pass, `gpuMode=host` works, encode via VA-API.
- Intel (Iris Xe): full pass, `gpuMode=host` works, encode via VA-API (`EncSliceLP`).
- NVIDIA (Ada 4060, Pascal 1050 Ti): `gpuMode=host` blocked on a real gralloc/EGL HAL
  incompatibility (separate side quest); `gpuMode=guest` works as a fallback; encode would need
  NVENC, not VA-API, regardless of the gralloc question.
