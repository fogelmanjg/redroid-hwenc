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

**Scope decision on NVIDIA (2026-09-19):** the crash isn't encoder-specific — it's in
SurfaceFlinger's core render engine, so `gpuMode=host` on NVIDIA has no working 3D acceleration
at all, and the system doesn't even finish booting there (not "everything works except encode",
literally nothing boots). `gpuMode=guest` does boot, and since Codec2/MediaCodec encoders can
consume plain `ByteBuffer` input (not just a GPU-native `Surface` — this is exactly the pattern
`screenrecord`/scrcpy's own capture already uses), an NVENC-backed encoder could plausibly work
on top of `gpuMode=guest` without needing the gralloc/EGL fix at all — one memcpy CPU→GPU instead
of true zero-copy, but real hardware encode either way. Decided to still treat full `gpuMode=host`
3D acceleration on NVIDIA as the real goal, not settle for guest-mode-only — without it, most
APKs (especially games) won't run at acceptable performance. It stays a separate side project,
doesn't block Tier 1+ on the main roadmap.

**Practical detour, worth documenting even though it doesn't fix anything upstream:** on any
machine with an NVIDIA discrete GPU *and* a working integrated GPU (iGPU/APU) — Intel CPUs with
graphics (most of them), AMD Ryzen "G" APUs, and most laptops with hybrid graphics — the iGPU is
a completely separate DRM device from the dGPU on Linux, nothing fuses them. redroid already has
a property for pinning the target explicitly:

```
androidboot.redroid_gpu_node=/dev/dri/renderD128   # whichever node is the iGPU, not NVIDIA
```

(from `redroid.legacy.rc`: `ro.kernel.redroid.gpu.node` → `ro.boot.redroid_gpu_node`). Skip
`--gpus all` entirely (that's specifically what injects NVIDIA's libraries) — just
`--privileged`, then point at the iGPU's render node. This goes through the exact same working
Mesa/DRI/GBM path already confirmed on Intel (`n02`) — no gralloc/EGL crash, and since that same
Intel iGPU already showed solid VA-API encode too, you can get *both* accelerated rendering and
accelerated encode through the iGPU, untouched by any of the NVIDIA-specific problems above.
Weaker than the discrete GPU for heavy 3D, but real hardware acceleration instead of
`gpuMode=guest`'s pure software path.

**Doesn't apply to plain AMD Ryzen desktop CPUs without the "G" suffix** (3600, 5600X, 5700X,
non-G 7000-series, etc.) — those have no integrated graphics at all, nothing to fall back to.

## 2026-09-19 (same day) — Tier 1: mapped the Codec2 component structure, and Tier 3 got a lot less scary

Read `android_external_v4l2_codec2` in detail (LineageOS's mirror, current AOSP-tracking fork —
`components/`, `service/`, `plugin_store/`). The goal was to extract the *structure*, not the
V4L2 logic itself. Turned out to split much more cleanly than expected into "generic C2 glue"
(fully reusable) vs. "the one thing that's actually backend-specific" (has to be written fresh
for VA-API).

**Service registration — a direct, tiny template.** `service/service.cpp` is ~50 lines: build a
`ComponentStore`, wrap it in `aidl::...::utils::ComponentStore`, register it with
`AServiceManager_addService()` under an instance name (`IComponentStore/default` in this
example — matches exactly the `IComponentStore/software` naming we saw registered for the stock
software store back in the Tier 0 investigation). Comes with its own `.rc` (start the service as
a `hal`-class process) and a VINTF manifest `.xml` fragment declaring the AIDL instance exists —
this `.xml` is the missing piece that would need adding to redroid's own device manifest for a
future hardware store to be *declared*, the same gate that `IsCodec2AidlHalSelected()` checks
before wiring up the real store instead of the null one.

**`ComponentStore.cpp` — generic, not V4L2-specific at all.** Implements the standard
`C2ComponentStore` interface (`createComponent`, `createInterface`, `listComponents`, etc.) plus
a small `Builder` (`.encoder(name, codec, factory)` / `.decoder(...)`) to declare components. None
of this cares what's behind the factory — directly reusable as-is for a VA-API-backed store.

**`EncodeComponent.cpp` (1078 lines) is almost entirely reusable too.** It's the C2/Android-facing
glue — `queue_nb`, the work queue, block pool management, reporting work back to the framework —
and it never touches V4L2 directly. It drives an abstract `VideoEncoder` interface instead.

**The actual backend-specific surface turned out to be small and precise — `VideoEncoder.h`:**

```cpp
virtual bool encode(std::unique_ptr<InputFrame> buffer) = 0;
virtual void drain() = 0;
virtual void flush() = 0;
virtual bool setBitrate(uint32_t bitrate) = 0;
virtual bool setPeakBitrate(uint32_t peakBitrate) = 0;
virtual bool setFramerate(uint32_t framerate) = 0;
virtual void requestKeyframe() = 0;
```

plus three simple getters. That's the entire contract a VA-API backend needs to implement —
everything upstream of it (interface param validation, work queue, block pool, service
registration) is reusable structure, not backend-specific.

**Tier 3's question got answered halfway, as a side effect.** `InputFrame` already carries raw
file descriptors + per-plane stride/offset, and `EncodeComponent.cpp` shows exactly where they
come from — a completely standard, already-shipped AOSP pattern, not something exotic to
reverse-engineer:

```cpp
const C2Handle* const handle = block.handle();
for (int i = 0; i < handle->numFds; i++) {
    fds.emplace_back(handle->data[i]);
}
```

A `C2ConstGraphicBlock`'s native handle just *is* a small struct of file descriptors — for a
gralloc buffer backed by dma-buf, `handle->data[i]` are the dma-buf fds directly, no special
mapper call needed. V4L2 hands these straight to `VIDIOC_QBUF` with `V4L2_MEMORY_DMABUF`. This
narrows Tier 3 down to specifically: does `vaCreateSurfaces` with
`VASurfaceAttribExternalBuffers` (`VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME`) accept *this exact* fd
as a valid VA-API surface — the "does redroid's own gralloc produce something VA-API-importable"
question — not "how do I even get a dma-buf fd out of Android's buffer system" (solved, stock
AOSP, verified against shipped code).

**VA-API encode call sequence, confirmed against the canonical reference**
(`intel/libva-utils`, `encode/h264encode.c`): `vaCreateConfig` (profile +
`VAEntrypointEncSlice`/`VAEntrypointEncSliceLP`) → `vaCreateSurfaces` → `vaCreateContext` → per
frame: build `VAEncSequenceParameterBufferH264`/`VAEncPictureParameterBufferH264`/
`VAEncSliceParameterBufferH264` buffers, `vaRenderPicture` each, bracket with
`vaBeginPicture`/`vaEndPicture`, then `vaSyncSurface` + `vaMapBuffer` on the coded buffer to read
the bitstream out. This basic example allocates its own surfaces internally rather than importing
external ones — the external-buffer/PRIME-import variant needed for Tier 3 isn't shown here, still
to be confirmed hands-on in Tier 2/3.

**Where this leaves Tier 4/5:** looking a lot more like "port `ComponentStore.cpp` + `service.cpp`
+ `EncodeInterface.cpp` with light renaming, write one new class implementing `VideoEncoder`
against VA-API instead of V4L2 ioctls" than "build an AOSP HAL component from a blank page." The
real unknowns left are entirely inside that one class: the Tier 3 zero-copy import, and the
VA-API rate-control/slice parameter details for H.264/HEVC.

## 2026-09-19 (same day) — Tier 2: standalone VA-API encode works, but not on the first, second, or third try

Built `tier2-vaapi-encode/main.c` — no Android, no redroid, just a C program that opens
`/dev/dri/renderD128` on server01 (AMD Renoir APU, radeonsi driver, already confirmed working for
`vainfo` back in Tier 0) and drives the encode side of VA-API directly: `vaCreateConfig` with
`VAEntrypointEncSlice`, two surfaces (input + reconstructed), a context, sequence/picture/slice
parameter buffers, `vaRenderPicture`, `vaSyncSurface`, then read the coded buffer back out and
write it to a file. Exactly the call sequence mapped against `libva-utils` during Tier 1.

**First cut "worked" (`VA_STATUS_SUCCESS` on every call, wrote a 49-byte file) but produced
garbage.** `ffprobe`/`ffmpeg` both rejected it: "Invalid data found when processing input." The
coded buffer contained a single NAL-shaped blob with no SPS, no PPS — just raw slice bytes. The
driver does **not** synthesize parameter sets on its own; it only encodes what you explicitly hand
it. This is the same class of gap the NVIDIA collaborator's spec doc (see below) also didn't
anticipate — it's easy to assume "the driver just gives you an H.264 file" when actually VA-API's
contract is much thinner than that.

**Fix, part 1 — packed headers.** `va_enc_h264.h` documents `VAEncPackedHeaderSequence`/`Picture`/
`Slice`: the application builds the SPS/PPS/slice-header RBSPs itself, byte for byte, and submits
them via `VAEncPackedHeaderParameterBuffer` + `VAEncPackedHeaderDataBuffer` pairs. Wrote a minimal
MSB-first bitstream writer from scratch (`bs_put_bit`/`bs_put_ue`/`bs_put_se`, Exp-Golomb per H.264
spec 9.1) and hand-built the SPS and PPS RBSPs field-by-field, mirroring the same struct values
already being sent as `VAEncSequenceParameterBufferH264`/`VAEncPictureParameterBufferH264` — the
two have to be kept in sync by hand, there's no library doing this for you here.

**That triggered a real crash, not a logic bug: `*** buffer overflow detected ***: terminated`
inside `vaRenderPicture`.** No `gdb` on this box, so bisected with `printf`+`fflush` markers (first
lesson: stdout was fully buffered since output was piped — `setvbuf(stdout, NULL, _IONBF, 0)` was
needed just to see how far execution actually got before the abort ate the buffer). Narrowed it to
the exact `vaRenderPicture` call — batching all 7 buffers (seq + packed-SPS pair + pic +
packed-PPS pair + slice) into one call crashes this radeonsi driver. Splitting into one
`vaRenderPicture` call per buffer "family" (matching the pattern real implementations use, not an
arbitrary workaround) avoided that — but the *real* bug turned out to be one line up: the packed
header data buffer is supposed to contain "the start code prefix 0x000001 followed by the complete
NAL unit" per the header comment in `va_enc_h264.h` — the driver does not add the start code
itself. Missing it wasn't just cosmetically wrong output, it was malformed enough to make the
driver's own parsing overflow a fortified buffer. Once the start code was prepended, the crash
was gone regardless of how the buffers were batched — so the batching split stayed as good
practice, but the start-code omission was the actual root cause.

**Fix, part 2 — the coded buffer still only had the raw slice, headerless, with an invalid
`0x00` NAL header byte where a real IDR slice needs `0x65`.** Turned out CABAC slices need a third
packed header: `VAEncPackedHeaderSlice`, containing the `slice_header()` syntax (not
`slice_data()`, not `rbsp_trailing_bits()` — the driver's hardware entropy coder continues writing
macroblock bits immediately after, non-byte-aligned, using the exact `bit_length` declared in the
packed header parameter). Wrote `build_slice_header_bits()` covering the IDR/I-slice path only
(`first_mb_in_slice`, `slice_type`, `frame_num`, `idr_pic_id`, the IDR form of
`dec_ref_pic_marking()`, `slice_qp_delta`, deblocking params — skipping every P/B-only branch of
7.3.3 since this prototype only ever emits one all-intra frame).

**Once all three packed headers (SPS + PPS + slice header) were present, the driver's behavior
changed completely** — not just "stopped crashing," but started actually synthesizing correct
output: the coded buffer came back as *three* separate segments (SPS NAL, PPS NAL, properly
headered `0x65` IDR slice NAL) instead of one broken blob. With packed SPS+PPS submitted but no
packed slice header, the coded buffer was unchanged from the very first broken attempt — the
driver silently no-ops on partial packed-header submissions rather than erroring or partially
applying them. That's a sharp edge worth remembering: "some packed headers accepted, no error,
but nothing happens" is a real failure mode here, not a hypothetical one.

**Final result, confirmed both ways:** `ffprobe` reports a clean stream — `codec_name=h264`,
`profile=Constrained Baseline`, `320x240`, `yuv420p` — and `ffmpeg -i out.h264 -f null -` decodes
it with zero errors. Extracted the decoded frame back to raw grayscale and checked pixel values:
the fed-in flat 128 came back as 130, exactly the kind of small rounding you'd expect from
lossy quantization (QP 26) and in-loop deblocking on a real hardware encode — not a red flag, a
sign the round-trip is genuinely doing lossy video compression rather than passing data through
unchanged.

**What this de-risks going into Tier 3:** the VA-API encode side is now proven working end to end
on real AMD hardware, including the non-obvious parts (packed headers, the exact bitstream layout,
the multi-call `vaRenderPicture` pattern). Tier 3 swaps the synthetic NV12 test surface for a
dma-buf imported from a redroid Codec2 buffer (`VASurfaceAttribExternalBuffers` +
`VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME`) — everything downstream of surface creation in this file
carries over unchanged.

## 2026-09-20 — Tier 3: dma-buf import confirmed working end to end on real hardware

Built `tier3-dmabuf-import/main.c` to answer the actual make-or-break question: can VA-API import
a dma-buf it did **not** allocate, and actually encode from it correctly.

**First attempt used Mesa GBM as the stand-in for "a gralloc buffer" and hit a wall immediately.**
`gbm_bo_create(gbm, W, H, GBM_FORMAT_NV12, GBM_BO_USE_LINEAR)` failed outright. A quick diagnostic
sweep (`gbm_device_is_format_supported` across `0`, `LINEAR`, `RENDERING`, `SCANOUT`, and
combinations) showed NV12 is unsupported for *any* usage flags on this radeonsi/Mesa gbm backend —
only RGB8888/ARGB8888 are allocatable through generic libgbm on this GPU. Real finding, not a dead
end: whatever allocator redroid's Android-side gralloc uses for YUV buffers, it is **not** the same
generic libgbm path used here, since that path can't produce NV12 on this hardware at all. Still
open which allocator gralloc actually uses and whether it hits the same wall — next session's
"pull a real dma-buf out of a running container" step should answer this directly.

**Pivoted to a plain DRM dumb buffer** (`DRM_IOCTL_MODE_CREATE_DUMB` on the primary node,
`/dev/dri/card1`) instead — allocator-agnostic, always linear, available on any DRM driver
regardless of what Mesa's gbm winsys supports. Laid out as `WIDTH x (HEIGHT * 3/2)` at 8bpp, which
is exactly NV12's Y+interleaved-UV byte layout. Exported via `drmPrimeHandleToFD` — the same
mechanism a gralloc buffer's native handle uses per the Tier 1 finding that `handle->data[i]` on a
`C2ConstGraphicBlock` are dma-buf fds directly.

**The import itself succeeded on the first working try**: `vaCreateSurfaces` with
`VASurfaceAttribExternalBuffers` + `VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME`, pointed at a dma-buf fd
VA-API had zero part in creating, returned `VA_STATUS_SUCCESS`.

**One planned check didn't work**: `vaDeriveImage` on the imported surface fails with
`VA_STATUS_ERROR_OPERATION_FAILED`, so the originally planned "write via plain mmap of the
dma-buf fd, read back via VA-API" CPU-side coherency check had to be skipped. This radeonsi driver
apparently doesn't support direct CPU-mapping of a PRIME-imported surface, even though — as the
next result shows — the surface is completely usable as encode input. Worth remembering as its own
sharp edge: "import succeeds, but you can't `vaDeriveImage` it" is a real, non-fatal driver
limitation, not a sign the import is broken.

**The actual test that matters: feeding the imported surface through the exact Tier 2 encode
sequence (packed SPS/PPS/slice headers, the multi-call `vaRenderPicture` split, unchanged) produced
output that is byte-for-byte identical to Tier 2's fully VA-API-native run** — same 69-byte
Annex-B stream. Decoding both and comparing pixel values confirms the same result too: 130,
Tier 2's documented QP26-quantization rounding of the fed-in flat 128. The hardware encoder
genuinely read real pixel data out of a dma-buf it never allocated and produced a result
indistinguishable from the "normal" path — not zeros, not garbage, not a silent no-op.

**What this settles, and what's still open:** on this AMD/radeonsi hardware, the *mechanism* Tier 3
exists to check — VA-API importing and correctly encoding from a foreign dma-buf — is confirmed
working. What's not yet confirmed is that the buffer came from redroid's actual Android gralloc:
a DRM dumb buffer is linear and allocator-agnostic, not necessarily the same tiling/layout a real
`AHardwareBuffer` allocation would produce. Next step: get a real dma-buf fd out of a running
redroid container (allocate an `AHardwareBuffer` via NDK inside the container, extract its native
handle's fd, hand it to this same import path from the host) and confirm it behaves the same way.
See `tier3-dmabuf-import/README.md` for the full writeup.

## 2026-09-20 (same day) — Cross-hardware validation: Tier 3 mechanism reproduces on a discrete AMD GPU too

Ran the exact same `tier3-dmabuf-import` spike (only change: `/dev/dri/card1` → `/dev/dri/card0`,
this host's primary node) on `jgustavo46`, which has a genuinely different GPU generation — a
discrete **AMD Radeon RX 480 (Polaris10)**, not the Renoir APU used for the original spike. Ran
Tier 2's standalone encoder first as a same-host baseline (70 bytes, decodable), then Tier 3.

**Same result as server01, exactly:** dma-buf import via `VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME`
succeeded, `vaDeriveImage` on the imported surface failed the same way (non-fatal), and the encode
from the imported surface produced output **byte-for-byte identical** to this machine's own Tier 2
baseline (70 bytes here, vs. 69 on server01's Renoir APU — the 1-byte difference is expected, it's
a different GPU/driver build, not a discrepancy within the same host). Confirms this isn't a
Renoir-specific fluke: the same PRIME-import mechanism, the same "import works but derive doesn't"
sharp edge, and the same "encoder genuinely reads real imported pixel data" result hold across two
different radeonsi-supported GPU generations (Vega-based APU vs. Polaris discrete).

## 2026-09-20 (same day) — Cross-hardware validation: reproduces on Intel iGPU too, and the CPU-readback check actually passes there

Same spike, third GPU: `jfogelman-n02`'s Intel Iris Xe (TigerLake-LP, `iHD` driver), woken via
WoL for this test. Same procedure (Tier 2 baseline first, then Tier 3), same result pattern —
`vaCreateSurfaces` with `VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME` imports the DRM dumb buffer, and the
resulting encode is byte-for-byte identical to this host's own Tier 2 baseline (63 bytes both
times). Uses `VAEntrypointEncSliceLP` as expected (Tier 0's Intel/AMD entrypoint split, confirmed
again here).

**One difference from both AMD/radeonsi runs (Renoir APU and Polaris10 discrete): `vaDeriveImage`
on the imported surface actually *succeeded* here**, so the originally-planned CPU-side coherency
check (write via plain `mmap()` of the dma-buf fd, read back via `vaMapBuffer`) ran to completion
and reported PASS — direct confirmation the VA-API surface aliases the exact same memory the mmap
write touched, not a driver-side copy. On radeonsi this check had to be skipped because
`vaDeriveImage` errored on an imported surface there (see the two entries above) — worth noting as
a real per-vendor difference: Intel's `iHD` driver supports direct CPU-mapping of a PRIME-imported
surface, this AMD radeonsi build (both GPU generations tested) does not, even though both are
equally capable of *encoding* from that same imported surface correctly.

**Net result across three real GPUs (AMD Renoir APU, AMD Polaris10 discrete, Intel TigerLake-LP
iGPu):** the Tier 3 mechanism — importing a dma-buf VA-API never allocated and correctly hardware-
encoding from it — holds on every one of them. The remaining open item is unchanged from the first
entry today: confirming this against a real dma-buf pulled from redroid's own Android-side gralloc,
not a generic DRM dumb buffer.

## 2026-09-20 (same day) — Closing Tier 3: import against a REAL, live redroid gralloc dma-buf

Everything above used a synthetic DRM dumb buffer as the "foreign dma-buf" — allocator-agnostic
and useful for isolating the import mechanism, but not proof that redroid's *actual* Android-side
gralloc produces something compatible. Closed that gap today: pulled a live dma-buf fd out of a
running redroid container's own process table and ran it through the same import path.

**Test instance, created the right way.** Rather than hand-roll another `docker run` (the source of
the earlier docker0-bridge bug), used `redroid-manager`'s own instance-creation code
(`dockerRuntime.create` + `binder.binderBinds` + the same `androidboot.*` Cmd construction
`routes/instances.js` uses) via a one-off script, on a throwaway `android-redroid:11-gapps`
instance — explicitly *not* touching the real in-use containers on ports 5555-5562. First attempt
hit the exact same docker0 ARP-never-resolves bug as the very first manual attempt (see below) —
confirmed as a real, pre-existing fault in this host's default `bridge` network's FDB (one veth
port was missing the `permanent`/vlan-tagged entries every working port has), not something caused
by the creation method. Worked around by giving the test instance its own dedicated Docker network
(`jg-redroid-test-net`) instead of touching `docker0`, which is shared with real, in-use instances.

**First manual attempt (before switching to the real creation flow) also surfaced the same bug**,
plus a discovery along the way: `portAllocator.nextPort()`/`binder.nextFreeSlot()` only look at
`redroid-manager`'s own store, which was empty (the real running instances were never registered in
it) — so the very first real-flow attempt picked port 5555 and binder slot 1, both already in use,
and Docker only surfaces that collision at container *start*, not *create*. Worth fixing in the
manager itself at some point (reconcile the store against `docker ps` on startup) — noted here, not
fixed today, out of scope for this spike.

**Getting the real dma-buf fd out of the container: `/proc/PID/fd/N` reopen does NOT work for
dma-buf.** First instinct was the classic trick (open the magic symlink at `/proc/<pid>/fd/<n>`
from the host to get a dup of another process's fd). That returns `ENXIO` for dma-buf specifically
— dma-buf is backed by an anonymous inode, and anon_inode's default `f_ops->open` deliberately
returns `-ENXIO` to block exactly this reopen pattern. The correct mechanism is `pidfd_getfd(2)`
(Linux 5.6+: `pidfd_open()` then `pidfd_getfd(pidfd, target_fd, 0)`), which explicitly supports
anonymous-inode fds — it's what CRIU and debuggers use for this. Confirmed working via a quick
`ctypes` syscall probe before writing it into `real-gralloc-import.c` directly in C (needs root /
`CAP_SYS_PTRACE` to target another user's process, same `sudo -u claude sudo` pattern as everything
else on this host).

**Where to find a real dma-buf inside a live redroid container**: swept every process's
`/proc/*/fd` for `dmabuf:` symlinks while `screenrecord` was running. Real dma-bufs turned up in
`surfaceflinger`, `composer@2.1-service` (the HWC HAL), `media.codec` (the OMX video HAL), and app
processes like `systemui` — `screenrecord`'s own process only held `AshmemAllocator_hidl` memfds,
not dma-bufs at all (the software colour-conversion/encode working buffers apparently live in
ashmem-backed memory for this build, not gralloc dma-bufs).

**The import itself succeeded, on the first real try.** Grabbed a dma-buf fd from `media.codec`
(`omx@1.0-service`, the process consuming frames from `screenrecord`'s virtual-display
`BufferQueue`), sized 3,932,160 bytes. `ffprobe` on the actual concurrent recording confirmed the
true coded resolution was 720x1280; 3,932,160 bytes matches RGBA8888 with a 256-byte-aligned row
stride of 3072 (720px × 4B/px = 2880, rounded up) × 1280 rows — i.e. this is SurfaceFlinger's
composited RGBA output, not a pre-converted YUV buffer (the software encoder does its own RGBA→YUV
conversion afterward, in that separate ashmem memory, not gralloc). Imported via
`VASurfaceAttribExternalBuffers` + `VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME` with
`VA_RT_FORMAT_RGB32`/`VA_FOURCC_RGBA` — `vaCreateSurfaces` returned `VA_STATUS_SUCCESS` immediately,
no format/layout complaints. Bonus: `vaDeriveImage` on this imported surface *also* succeeded here
(unlike the synthetic-NV12 spike on this same AMD driver, where it failed) — direct CPU readback of
a real, externally-allocated dma-buf worked end to end.

**What didn't fully close: content verification.** Read back all zero bytes across four different
buffer instances captured this way (two fds from one recording session, two more from a second
session with the Settings app open on screen for visible content) — while a `screencap` taken in
parallel confirmed the real screen was NOT blank (mostly dark but with real non-zero pixel
variation, opaque alpha). Most likely explanation: these particular `media.codec`-held dma-bufs are
pre-allocated pool/reserve buffers not currently holding the "front" composited frame, rather than
a sign the import or format math is wrong (the size math checks out exactly against the
independently-confirmed resolution). Chasing this further would need instrumenting the actual
`BufferQueue` producer/consumer state or targeting a build whose encode path is actually
dma-buf/GPU-backed rather than this Android 11 image's software-only OMX encoder. Left open
deliberately — full content verification is naturally covered once the wifi-v3 (Android 15,
GPU-backed) image gets the same end-to-end treatment planned for later.

**Bottom line for Tier 3**: the mechanism question the whole tier exists to answer — can VA-API
import a dma-buf it never allocated, of the exact kernel object type Android's gralloc actually
produces, obtained from a real running Android system process via the correct kernel mechanism —
is answered **yes**, confirmed against a live object, not just a synthetic stand-in. See
`tier3-dmabuf-import/README.md` for the consolidated writeup.

## 2026-09-20 (same day) — Tier 4: hardware Codec2 component registers and lists, confirmed on real hardware

Closed Tier 4: got Android to actually recognize and list `c2.hardware.encoder.h264` via
`dumpsys` on a real, running redroid instance — no real encoding wired in yet (that's Tier 5),
but the registration/enumeration plumbing works end to end.

**The AOSP source tree was gone.** `~/aosp-redroid-15` (the checkout used to build the
`redroid-jg-15` images) had been deleted after the last build to free NVMe space — deliberate,
since it's 130GB+ and there was no known date it'd be needed again. A full backup existed on the
SATA disk (`aosp-redroid-15.tar`, 134.7GB, plus `aosp-out-redroid15.tar`, 111GB of build
artifacts/ccache) with sha256 checksums. Verified both, restored both to NVMe (234GB combined,
294GB free afterward) — the `/out` restore in particular meant the rebuild could be incremental
rather than from-scratch.

**Found the exact starting point: AOSP ships an official empty-Codec2-service template.**
`frameworks/av/media/codec2/hal/services/vendor.cpp` is a real, maintained example with a literal
comment: "make a copy of this whole directory and rename modules accordingly." It has a stub
`StoreImpl : public C2ComponentStore` returning empty/`C2_NOT_FOUND` from everything, with a
`// TODO: Replace this with store = new utils::ComponentStore(...)` marking exactly where a real
implementation plugs in. This is a much better foundation than reference-reading
`external/v4l2_codec2` from scratch (Tier 1) — it's the same shape, official, and already wired
into the AIDL registration path this specific build needs (confirmed via checking the live
instance: only `IComponentStore/software` is registered, `vaapi` was free to use).

**New module: `external/vaapi_codec2/service/`.** Copied the template, made two changes:
stripped the HIDL half entirely (Tier 0 already confirmed this build only activates a Codec2
store when `media.c2.hal.selection=aidl`, so the generic template's dual HIDL/AIDL branching is
dead weight here), and made `listComponents()` return one `C2Component::Traits` entry
(`c2.hardware.encoder.h264`, `DOMAIN_VIDEO`, `KIND_ENCODER`, `rank=1`, `video/avc`) instead of an
empty list. `createComponent()`/`createInterface()` still return `C2_NOT_FOUND` — Tier 4's whole
point is enumeration, not a working encoder. Wired into the build via
`hardware/redroid/c2/c2.mk` (the same file that already carried the `redroid.c2.rc`/`.sh` hooks
found in Tier 0), `PRODUCT_PACKAGES += android.hardware.media.c2-vaapi-service`.

**First build attempt taught a real lesson about the build container.** `redroid-rebuild`'s
entrypoint is `chroot ... /bin/bash -i` — an interactive shell with no real tty attached. It
eventually exited (nothing was feeding it stdin), and Docker tore down every process in that
container's namespace when its PID 1 died — including a `soong_build`/ninja run that was still
genuinely working (not hung), killing ~10 minutes of the first-ever Soong analysis pass on the
restored `/out`. Fixed by replacing it with a new container (`redroid-build-persist`) using
`--entrypoint /bin/sleep infinity` as PID 1 instead, running actual build commands via
`docker exec --user jgustavo` — durable regardless of how long any single command takes, no
dependency on an interactive shell surviving.

**The targeted module build worked on the first real try afterward**: `m android.hardware.media.c2-vaapi-service`
compiled clean, 293 targets, ~9.5 minutes, zero errors — installed the binary, its `.rc`, the
seccomp policy, and (confirming Tier 1's exact prediction) the VINTF manifest fragment at
`vendor/etc/vintf/manifest/manifest_media_c2_vaapi.xml`. `m vendorimage` afterward repackaged
`vendor.img` in 20 seconds (all the real compilation was already done and cached).

**Deploying into a live container without a full image re-import.** These redroid Docker images
are a flattened rootfs (`docker import` of combined system+vendor content), not a mounted `.img`
file redroid reads at runtime — so getting the new component into a real container meant mounting
the freshly-built `vendor.img` (plain ext4, `mount -o loop`) and `docker cp`-ing the relevant
files directly into a redroid container's `/vendor`, matching the wifi-v2/v3 image history's own
apparent pattern of incremental runtime patches rather than full rebuilds each time.

**Two more real bugs surfaced and fixed getting a clean boot:**
1. Files must be injected while the container is *created but not yet started* — Android's init
   only parses `/vendor/etc/init/*.rc` once, early in boot. Copying into an already-booted
   container is too late; it won't pick up the new service without a full restart of the
   container (which reintroduces problem #2 below).
2. `docker stop` + `docker start` on the *same* container changes its veth's MAC address, but the
   bridge's FDB can be left with stale "permanent" entries pointing at the old MAC — ARP for the
   container's IP then fails outright (`No route to host` / neighbor state `FAILED`/`INCOMPLETE`),
   even though `docker exec` (which doesn't go through the network stack) shows the container is
   completely healthy inside. This is the *same* bug class as the very first manual test-instance
   attempt earlier today, now confirmed to trigger on stop/start of a single container too, not
   just on first creation. Workaround used both times: never stop/start the same container: 
   `docker rm` + fresh `docker create` (inject files) + `docker start`, and use a brand-new
   dedicated Docker network each time rather than reusing one that's already had containers churn
   through it. A real docker0-adjacent host issue worth investigating properly at some point, not
   yet root-caused.
3. First successful boot crash-looped anyway: `CANNOT LINK EXECUTABLE
   ".../android.hardware.media.c2-vaapi-service": library "libcodec2.so" not found`. Only copied
   the 4 files directly tied to our module at first, forgetting that adding this module pulled 16
   *new* shared libraries into `vendor.img` that weren't in the stock wifi-v3 image before
   (`libcodec2.so`, `libcodec2_aidl.so`, `libcodec2_vndk.so`, the AIDL bufferpool/bufferqueue libs,
   `libavservices_minijail.so`, `libminijail.so`, `libcap.so`, `libdmabufheap.so`, `libion.so`,
   etc.) — diffed the mounted `vendor.img`'s `/lib64` against the running container's to find
   exactly which 16 were missing, copied those in too.

**Confirmed working, on a real running container**: `service list` shows
`android.hardware.media.c2.IComponentStore/vaapi` registered alongside the stock `/software`
store, and `dumpsys android.hardware.media.c2.IComponentStore/vaapi` reports exactly the intended
result:

```
Supported components:
    name: c2.hardware.encoder.h264
    domain: 1        # DOMAIN_VIDEO
    kind: 2          # KIND_ENCODER
    rank: 1
    mediaType: video/avc

Active components:
    NONE
```

"Active: NONE" is expected and correct — recognized and listed, nothing instantiable yet.

**What Tier 5 needs, and what carries over:** the test container (`jg-redroid-tier4-test`, on its
own dedicated Docker network, `--restart no`) is left running as a testbed. Iterating on
`createComponent()`'s real implementation doesn't need a fresh container each time — rebuilding
just this module and replacing the binary, then restarting only the Android-level service (not
the Docker container), avoids the stop/start networking bug entirely since it never touches the
container's network namespace. The one thing that *will* require repeating today's
build-vendor.img-and-inject-new-libs dance is any genuinely new shared-library dependency Tier 5
pulls in — most notably `libva`/Mesa's VA-API Gallium state tracker itself, confirmed back in
Tier 0 to be completely absent from this vendor image today. That's real, known work ahead, not a
surprise.

## 2026-09-20 (same day) — Tier 5.1: libva builds and installs, first Tier 5 sub-goal closed

Started the Tier 5 plan (broken into sub-goals: 5.1 libva, 5.2 Mesa's VA-API state tracker ported
to Soong, 5.3 the loadable driver .so, 5.4 a standalone smoke test, 5.5 the real Codec2 component,
5.6 real end-to-end encode).

**5.1 turned out to be nearly free.** `external/libva` is already AOSP's own official mirror
(`android.googlesource.com/platform/external/libva`) with a real, working `Android.bp` --
`va/drm/va_drm.c` (the exact `vaGetDisplayDRM()` code Tier 2/3 already use) is compiled directly
into the main `libva` module and linked against `libdrm`, no separate `libva-drm` split like the
desktop packaging convention. The module has `enabled: false` at the top level (same
disabled-by-default pattern as the Tier 4 service template), but also an
`arch: { x86_64: { enabled: true } }` override -- and redroid_x86_64 already builds exactly that
arch, so `m libva` built and installed `vendor/lib64/libva.so` on the very first try, no
`Android.bp` edit needed at all. Added `PRODUCT_PACKAGES += libva` to `hardware/redroid/c2/c2.mk`
so it's part of the real product build going forward, not just a one-off `m libva` invocation.
Skipped `libva-android` (the Gralloc/ANativeWindow display path) -- not needed, Tier 2/3 already
proved `vaGetDisplayDRM()` works and `/dev/dri/renderD128` is present and usable inside these
containers.

**One real limitation found trying to hot-deploy into the already-running Tier 4 test
container**: `docker cp` into a *running* container fails outright
(`Error response from daemon: openat dev/binder: read-only file system`) when that container has
external bind-mounted device nodes like the `/dev/binder`/`/dev/vndbinder` binderfs mounts these
redroid containers all use -- confirmed it's not `/vendor`-specific by testing a trivial file copy
into `/data` too, same failure. Every successful file injection so far (Tier 4, and this one)
happened on a container that was `docker create`d but not yet `docker start`ed -- docker cp
clearly handles that case through a different code path that doesn't trip on the bind mounts.
Practical rule going forward: file injection into these containers always means
create-(inject)-start once, never cp into an already-running one.

Next: 5.2, porting Mesa's VA-API Gallium frontend (`external/mesa3d/src/gallium/frontends/va` +
`targets/va`) to Soong -- real, from-scratch build-file work, unlike 5.1's already-there module.

## 2026-09-20 (same day) — Tier 5 architecture pivot, and 5.2 closed: the host-side encode daemon works

**Found a major scope surprise going into 5.2/5.3 as originally planned.** The plan was to port
Mesa's VA-API Gallium frontend (`gallium/frontends/va` + `targets/va`, ~40 files) to Soong and link
it against this build's existing Android-side `radeonsi` driver. Turns out there is no
Android-side `radeonsi` at all: `external/mesa3d`'s real `Android.bp` (26 files, checked
systematically -- none under `gallium/drivers` or `gallium/winsys`) only builds the
`gfxstream`/ANGLE/Vulkan pieces. Real GLES rendering in this build is *forwarded to the host* the
same way ChromeOS ARC++/the Android Emulator do it -- Android never runs a hardware GPU driver of
its own. The `radeonsi` source is present (full upstream Mesa mirror), just never wired into the
Android build at all. Porting the *actual* driver stack (radeonsi + winsys + likely LLVM for its
shader compiler) to bionic would be a vastly bigger undertaking than the VA-API frontend alone --
realistically weeks, not sessions.

**Pivoted the whole Tier 5 architecture instead of attempting that port**: since Android's own
graphics stack already forwards the hard part to the host, do the same thing for encode. A small
host-side (glibc) daemon does the real VA-API work using the exact pipeline already proven in
Tier 2/3; the Codec2 component running inside Android (bionic) just becomes a thin client that
hands it a dma-buf fd over a Unix socket and gets encoded bytes back. This avoids needing *any*
Mesa/LLVM porting to bionic at all -- the real encode literally runs the same already-working code,
just called via IPC instead of a direct function call. Revised sub-goal plan: 5.2 the daemon itself
(this entry), 5.3 bridging the socket into a redroid container + a tiny Android-side test client,
5.4 the same test against a real Android gralloc buffer, 5.5 wiring this into the Tier 4 component,
5.6 a real end-to-end app-driven encode.

**5.2 confirmed working on the first real try.** `tier5-vaapi-daemon/daemon.c` wraps the exact
encode/import logic from `tier2-vaapi-encode`/`tier3-dmabuf-import` (packed SPS/PPS/slice headers,
`VASurfaceAttribExternalBuffers` + `VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME`) in a persistent
`AF_UNIX`/`SOCK_STREAM` server: one connection = one `EncodeRequest` (dimensions + NV12 plane
strides) delivered via `sendmsg()` with the dma-buf fd as `SCM_RIGHTS` ancillary data, one
`EncodeResponse` + Annex-B H.264 bytes back. `tier5-vaapi-daemon/test-client.c` (host-side only,
no Android yet) builds the same synthetic DRM dumb buffer as the Tier 3 spike, sends it to the
daemon, and got back **69 bytes, byte-for-byte identical to Tier 2's own reference output** --
confirmed decodable via `ffprobe`/`ffmpeg`. Ran the client twice in a row against the same daemon
process to confirm the persistent VA-API context (config/context created once at startup, reused
across requests) survives repeated requests without issue.

## 2026-09-20 (same day) — Tier 5.3: the Android/host IPC bridge works, confirmed end to end

Closed 5.3: a binary compiled for Android (bionic, via Soong) and run *inside* a redroid
container talked to the host-side daemon over the shared Unix socket and got back a real,
correct H.264 encode.

**Bridging the socket into the container**: bind-mounted the daemon's socket directory
(`/dev/vaapi-helper`) into a fresh test container the same way these containers already bind-mount
`/dev/binder` — no surprises there, plain Docker bind mount.

**The Android-side test client is nearly the exact same C code as the host-side one**
(`external/vaapi_codec2/test-client/test_client.c`, copied from
`tier5-vaapi-daemon/test-client.c`): same synthetic DRM dumb buffer creation, same `sendmsg()` +
`SCM_RIGHTS` protocol. Confirms `AF_UNIX`/`SCM_RIGHTS`/`libdrm` all behave identically under
bionic vs glibc for this — no bionic-specific surprises here, unlike the Mesa/radeonsi situation.

**Two more real deployment lessons, both about `/vendor` specifically:**
1. `docker cp` into an *already-booted* container fails outright for any path once Android has
   fully started -- not just the binder-bind-mount issue found in Tier 4, but specifically because
   `/vendor` itself becomes genuinely read-only at the Android/kernel level after boot (confirmed:
   `mount -o remount,rw /vendor` reports `/vendor` isn't even a separate entry in
   `/proc/mounts` -- it's baked into the same flattened rootfs, and something in the boot process
   still marks it read-only). `/data` stays writable throughout, matching real device behavior.
   Practical rule: `/vendor` changes always need a full recreate (create → inject while stopped →
   start once); `/data` can be hot-patched via `docker exec -i ... sh -c "cat > path" < localfile`
   at any time, including while the container is already running and booted.
2. `docker exec -i <container> sh -c "cat > /path/to/file" < local-file` is a reliable substitute
   for both `docker cp` (broken on these containers per Tier 4's finding) and `adb push` (broken by
   the recurring adb "device offline" flakiness this host has been hitting all session) for getting
   a file into a *writable* path on an already-running container. Used to push the test binary to
   `/data/local/tmp` and run it directly via `docker exec`, sidestepping adb entirely.
3. Forgot to also inject `libdrm.so` alongside `libva.so` on the first attempt at this container
   (it's a new dependency `libva`'s build itself pulled in, same "diff the mounted vendor.img
   against the running container" lesson as Tier 4's 16 missing libraries, just one file this
   time) -- caught it via `CANNOT LINK EXECUTABLE ... library "libdrm.so" not found`, same failure
   signature as before.

**Result, byte-for-byte identical to the very first Tier 2 reference output**: 69 bytes, decodes
cleanly, confirmed via `diff` against `tier2-vaapi-encode/out.h264`. The IPC bridge across the
Android/host boundary is solid.

Next: 5.4, the same round trip but with a dma-buf fd that actually came from an Android-allocated
gralloc buffer instead of this checkpoint's synthetic DRM dumb buffer.

## 2026-09-20 (same day) — Tier 5.4: a real Android gralloc dma-buf encodes correctly too

Closed 5.4: sent a dma-buf fd that actually came from an Android-allocated gralloc buffer
(not a synthetic DRM dumb buffer) to the daemon and got back a correct, decodable encode.

**Getting the buffer the right way this time.** Tier 3's real-gralloc spike had to scavenge a
dma-buf fd out of a live process's fd table via `pidfd_getfd` because there was no way to write
and build native Android code at that point in the project. Now that the full AOSP tree is
available, this is much cleaner: `AHardwareBuffer_allocate()` with
`AHARDWAREBUFFER_USAGE_VIDEO_ENCODE` allocates a real gralloc buffer directly, and
`AHardwareBuffer_getNativeHandle()` (from `vndk/hardware_buffer.h`, the vendor-facing half of the
API) gives the dma-buf fd straight from the native handle -- no scavenging needed.
`libnativewindow` (which implements both) is LLNDK, explicitly meant to be linked from vendor
code, so this needed no special Soong permissions.

**Two real, worth-remembering findings hit along the way, neither of them blockers:**

1. **`AHardwareBuffer_lockPlanes()` isn't implemented on this build's Mapper HAL** (returns -38/
   ENOSYS) -- this build only registers `android.hardware.graphics.mapper@2.0-impl-2.1`, and that
   version apparently doesn't implement the flexible-layout/lockYCbCr verb the newer API needs.
   Not fatal: we don't actually need AHardwareBuffer's own CPU-access API at all -- the dma-buf fd
   from `getNativeHandle()` supports a plain `mmap()` directly, exactly the same way every other
   tier has filled a foreign dma-buf without any higher-level API involvement.
2. **Requesting `AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420` (flexible YUV420) returns a buffer sized
   like RGBA8888** (307200 bytes for 320x240, i.e. `width*height*4`) instead of NV12's expected
   `width*height*1.5` (115200) -- the same "this generic graphics stack doesn't really support YUV
   allocation" pattern the very first Tier 3 spike hit with Mesa's GBM (only RGB(A) formats
   allocatable there too). Not chased further since the buffer is still large enough and still a
   genuine gralloc-backed dma-buf -- just filled the first `width*height*1.5` bytes of it as NV12
   ourselves.
3. **The dma-buf import itself failed on the first attempt** with the exact tightly-packed stride
   (320, no padding) `AHardwareBuffer_describe()` reported: `"resource allocation failed"`.
   Isolated the actual cause carefully before assuming it was about this buffer's origin: forced
   the *known-good* synthetic DRM dumb buffer (which normally imports and encodes fine) to lie
   about its own stride as 320 instead of its real 512, and got the identical failure. This proves
   it's a VA-API/radeonsi pitch *alignment* requirement, not anything about gralloc-sourced buffers
   being fundamentally incompatible for import. Fix (valid for this test since we control the
   fill): use an aligned stride (512, matching what already works) instead of the tight one when
   filling and describing the buffer -- the real buffer has more than enough bytes (307200) for
   that padded layout (184320 needed). **Worth remembering for Tier 5.5/5.6**: a real Codec2
   encoder input buffer's actual stride is decided by whatever gralloc/BufferQueue producer created
   it, not by us -- if a real production buffer ever arrives with a misaligned stride, the daemon
   will need to either reject it clearly or do an internal copy into an aligned staging buffer
   before import, rather than assuming Tier 5.4's "we control the fill" luxury.

**Result**: 69 bytes, byte-for-byte identical to Tier 2's reference output, confirmed decodable --
from a real Android-allocated dma-buf this time, obtained the correct way.

Next: 5.5, wiring this client logic into `tier4-codec2-skeleton`'s `createComponent()` for a real
Codec2 component.

## 2026-09-21 — Tier 5.5: a real C2Component processes real work and produces correct H.264

Closed 5.5: c2.hardware.encoder.h264 went from "registered and listed" (Tier 4) to "actually
processes a real C2Work item through the genuine Codec2 framework machinery and returns correct,
decodable H.264" (Tier 5.5) -- the literal "wire Tier 2/3 into the callbacks of the Tier 4
component" goal from the original roadmap.

**Picked SimpleC2Component over reusing v4l2_codec2's EncodeComponent verbatim.** The original
Tier 1 plan assumed porting `external/v4l2_codec2/components/EncodeComponent.cpp` (1078 lines)
wholesale, swapping only its `VideoEncoder` backend. Looked at it seriously this session and found
two real reasons not to: its `VideoEncoder` interface is built around V4L2's async,
interrupt-driven hardware model (`base::Callback`-based `InputBufferDoneCB`/`OutputBufferDoneCB`),
which doesn't fit our daemon's simple synchronous request/response round trip at all; and even the
*reference* project doesn't show a complete example wiring `EncodeComponent` + a concrete
`VideoEncoder` + `ComponentStore` together (no `V4L2Encoder::create()` callers anywhere in this
tree) -- meaning "verbatim reuse" wasn't actually a complete, working example to copy from either.

Found a much better fit in `frameworks/av/media/codec2/components/base/`:
`SimpleC2Component` (used by all of AOSP's own software codecs, e.g. the exact
`c2.android.avc.encoder` Tier 0's fix unlocked) exposes a single synchronous
`process(work, pool)` you override, plus five trivial lifecycle hooks
(`onInit`/`onStop`/`onReset`/`onRelease`/`onFlush_sm`) -- it handles the entire `C2Component`
contract (`queue_nb`, threading, work ordering) internally. Paired with
`SimpleC2Interface<T>::BaseParams` (wraps any `C2InterfaceHelper` -- exactly the pattern Tier 4's
`StoreImpl::Interface` already used -- into a full `C2ComponentInterface`), this let the whole
component come together as ~250 lines instead of needing v4l2_codec2's much larger dependency
closure (`libchrome`, its own `VideoFramePool`, `FormatConverter`, etc.).

**`external/vaapi_codec2/component/VaapiEncComponent.{h,cpp}`**: `VaapiEncInterface` (a
`PictureSize` param, default 320x240, on top of `BaseParams`' standard identity/media-type
plumbing) and `VaapiEncComponent : public SimpleC2Component`. `process()` extracts the input
`C2GraphicBlock`'s dma-buf fd via `block.handle()->data[0]` -- the *exact* pattern
`external/v4l2_codec2`'s own `createInputFrame()` uses (confirmed by reading it this session,
Tier 1's citation held up) -- deliberately skipping `block.map()`/`layout()` entirely, since
Tier 5.4 already proved this build's Mapper HAL doesn't implement the flexible-layout verb that
needs (ENOSYS). Sends the fd to the daemon over the same socket protocol as every 5.2-5.4 client,
gets back H.264 bytes, and writes them into a `C2LinearBlock` fetched from the framework's own
`pool` via `createLinearBuffer()` -- a protected helper `SimpleC2Component` itself provides (found
by reading `C2SoftAvcEnc.cpp`'s own `finishWork()`), so no buffer-wrapping code needed writing.

**`service.cpp`'s `StoreImpl::createComponent()`/`createInterface()`** now construct real
`VaapiEncComponent`/`VaapiEncInterface` instances for `"c2.hardware.encoder.h264"` instead of
returning `C2_NOT_FOUND`.

**Two real Soong linkage lessons, both non-obvious:**
1. A `cc_library_static`'s own `shared_libs` don't automatically make their *headers* available to
   whatever links against that static library -- needed an explicit
   `export_shared_lib_headers: ["libcodec2_soft_common"]` on `libvaapi_codec2_component`, or
   `service.cpp` couldn't find `<SimpleC2Component.h>` at all (`fatal error: file not found`)
   despite the static lib compiling fine on its own.
2. Even with headers fixed, the actual *symbols* from `libcodec2_soft_common.so` and
   `libstagefright_foundation.so` (for `MEDIA_MIMETYPE_VIDEO_AVC`) didn't make it onto the final
   binary's link line either -- Soong didn't propagate them transitively through the static-lib
   dependency in this build config. Fixed by listing both directly in the *binary's own*
   `shared_libs` (`service/Android.bp`), not just the static library's.

**Test harness, not a hand-rolled shortcut**: `component_test.cpp` drives `VaapiEncComponent`
directly (bypassing the AIDL layer, which Tier 4 already confirmed separately) with a **real**
`C2Work` wrapping a **real** `C2GraphicBlock` -- built via
`_C2BlockFactory::CreateGraphicBlock(AHardwareBuffer*)`, a genuine AOSP API for wrapping an
app-provided `AHardwareBuffer` into Codec2, the same mechanism real Surface-based encoder input
eventually goes through. Sets a `Listener`, calls `start()` → `setListener_vb()` → `queue_nb()`,
waits on a condition variable for `onWorkDone_nb()`, and reads the output linear block directly.

**Confirmed working, deployed on a real redroid instance** (diffed two new libraries pulled in by
this change, `libcodec2_soft_common.so`/`libsfplugin_ccodec_utils.so`, into the usual
create-inject-start-once cycle; `libstagefright_foundation.so` turned out already present in the
base image): `queue_nb()` → `process()` → the daemon round trip → `onWorkDone_nb()` produced 83
bytes of valid H.264 (`ffprobe`/`ffmpeg` confirm: H.264, Constrained Baseline, 320x240, decodes
clean). Different byte count than every prior tier's 69/70 bytes is expected and correct here --
this test didn't fill the gralloc buffer with the usual flat-128 pattern before wrapping it, so
the encoder genuinely compressed whatever uninitialized content was actually in that fresh
allocation, not a controlled test pattern.

**What's left for Tier 5.6**: a real app (or `screenrecord`) driving this component through the
actual framework (`MediaCodec`/`Codec2Client`, not a hand-built `C2Work`), which will be the first
real test of the fixed 512-byte stride assumption against a genuinely externally-produced buffer
whose actual layout this component doesn't control.

## 2026-09-21 (same day) — Tier 5.6: getting a real app to even see the encoder took four separate bugs

Picked `scrcpy` as the real-app test (drives `MediaCodec`/`MediaCodecList` exactly like any other
Android app would, via genuine framework APIs, no hand-built `C2Work`). It couldn't see
`c2.hardware.encoder.h264` in `--list-encoders` at all. Four independent, real bugs, found and
fixed one at a time by tracing the actual framework code path (not guessing):

1. **Instance name didn't match the FCM pattern.** The service registered itself as
   `IComponentStore/vaapi`. The Framework Compatibility Matrix only accepts instance names
   matching `default[0-9]*` or `vendor[0-9]*_software` for this HAL — `vaapi` matched neither, so
   it was correctly declared in the vendor manifest but silently unusable framework-side. Renamed
   the service's `getName()`, the AIDL registration path, and `manifest_media_c2_vaapi.xml`'s
   `<fqname>` all to `default`.
2. **`debug.stagefright.ccodec` was never set.** It only gets set to `4` (all Codec2 components
   available) by `redroid.c2.rc`, gated on the `androidboot.use_redroid_c2=1` boot cmdline flag —
   which the test containers this session weren't passing. Without it Codec2 components are
   filtered out of `MediaCodecList` entirely regardless of anything else being correct.
3. **No `media_codecs.xml` entry.** `Codec2InfoBuilder::buildMediaCodecList()` (the actual code
   behind `MediaCodecList`) requires *both* a successful `Codec2Client::CreateInterfaceByName()`
   *and* a matching entry in `media_codecs.xml` — confirmed by reading the source, not guessing.
   Added `<MediaCodec name="c2.hardware.encoder.h264" type="video/avc" />` to
   `hardware/redroid/omx/media_codecs.xml`.
4. **Missing `C2StreamProfileLevelInfo` param.** Wrote a diagnostic (`client_test.cpp`, a *system*
   binary calling `Codec2Client::CreateInterfaceByName`/`ListComponents` directly, bypassing
   `MediaCodecList` entirely) that proved the interface and the component were both discoverable
   and correct at that layer — meaning the bug was specifically inside
   `Codec2InfoBuilder::addSupportedProfileLevels()`, which queries
   `C2StreamProfileLevelInfo::profile` and silently drops a component with zero reported
   profile/levels before it ever reaches `MediaCodecList`. `VaapiEncInterface` never declared this
   param at all. Added it (Constrained Baseline / Level 3, matching what the daemon actually
   encodes).

All four fixed, `c2.hardware.encoder.h264` still didn't appear in `scrcpy --list-encoders`. Found
the real reason chasing it further: `MediaCodecList::getInstance()`
(`frameworks/av/media/libstagefright/MediaCodecList.cpp`) doesn't build the list itself — it asks
`mediaserver` for it over binder (`getCodecList()`) and caches the *reply* in a function-local
static for the calling process's lifetime. `mediaserver` builds its own copy lazily, once, on
first request. Since `media.c2.hal.selection=aidl` / `device_config codec_fwk aidl_hal=true` were
being set with `setprop`/`device_config put` commands run *after* boot completed, `mediaserver`
had already built and cached its list using the *old* (HIDL-only, no `default` AIDL store)
`Codec2Client::GetServiceNames()` result before those fixes took effect — confirmed directly via
`logcat`'s own `Codec2Client: Available Codec2 services: "software"` line, timestamped seconds
after boot, well before any of the runtime `setprop` calls ran. No amount of relaunching `scrcpy`
(a genuinely fresh process each time) could ever see the fix, because it was never mediaserver's
per-process cache that needed refreshing — `Codec2Client::GetServiceNames()` itself is a
function-local `static`, but each NEW process (scrcpy-server's own `app_process`, not forked from
zygote) re-evaluates it fresh from current properties; the actual stale cache lived in
*mediaserver*, which every process's `MediaCodecList::getInstance()` defers to over binder instead
of building its own copy. `kill -9`'ing `mediaserver` (letting `init` restart it, now with correct
properties already in place) was what finally worked: `c2.hardware.encoder.h264 (hw) [vendor]`
appeared in `scrcpy --list-encoders`.

**Practical fallout**: the boot sequence needs `media.c2.hal.selection=aidl` /
`device_config codec_fwk aidl_hal=true` set *before* mediaserver's first `getCodecList()` call, not
just before an app queries it — either via an early-boot property (not currently how the test
containers are set up) or by killing/restarting mediaserver after applying the runtime fixes, which
is what every rebuild-and-retest cycle in this tier actually did.

## 2026-09-21 (same day) — Tier 5.7: real hardware encode runs end to end for a real app, two more real bugs found

With `c2.hardware.encoder.h264` finally visible, pointed `scrcpy` at it for real:
`--video-encoder=c2.hardware.encoder.h264`. First attempt crashed immediately
(`IllegalStateException: Pending dequeue output buffer request cancelled`) — the daemon's own log
showed why: `radeonsi: error: ... VCN - DCC surfaces not supported`. A genuine Surface-sourced
frame (SurfaceFlinger's virtual-display capture, via `GraphicBufferSource`) is a real gralloc
allocation that can be GPU-tiled *and* DCC-compressed; every earlier tier's test buffers (dumb
buffers, self-filled `AHardwareBuffer`s) happened to be plain and linear, so this never came up
until a real app exercised the real path.

**Bug 1 — DCC.** Declared the standard `C2StreamUsageTuning::input = BufferUsage::VIDEO_ENCODER`
param (same pattern `external/v4l2_codec2`'s `EncodeInterface` uses) so gralloc knows this buffer
feeds a hardware encoder — confirmed via a diagnostic query (`vaapi_c2client_test` extended to
query `C2StreamUsageTuning::input` directly) that the value (`65536` = `VIDEO_ENCODER`) does reach
`CCodec`'s `config->mISConfig->mUsage`. Didn't help: VCN's DCC restriction still fired, because
`GRALLOC_USAGE_HW_VIDEO_ENCODER` on this stack (`external/minigbm`'s `amdgpu.c`) affects buffer
*height alignment* and *tiling combination selection*, not compression directly. The actual fix
was blunter: minigbm's Mesa/radeonsi driver respects `AMD_DEBUG=nodcc` (confirmed present as a
literal string in `libgallium_dri.so`, paired with `"Disable DCC."`). Since Mesa reads this from
the environment of whichever process allocates the buffer (`surfaceflinger`, compositing the
capture), and `/system`'s `surfaceflinger.rc` is read-only at runtime (can't add `setenv` there
directly), added a small vendor init script instead —
`/vendor/etc/init/redroid-nodcc.rc`:
```
on early-init
    export AMD_DEBUG nodcc
```
`init` parses `/vendor/etc/init/*.rc` automatically (no manifest registration needed, confirmed by
reading `LoadBootScripts()` in `system/core/init/init.cpp`) and `export` calls plain `setenv()` in
init's own process — inherited by every service init `fork()`s afterward. First attempt at this
silently failed to take effect at all (confirmed via `/proc/<surfaceflinger-pid>/environ`, which
lacked the variable): `init` refuses to parse any `.rc` file that's group- or world-writable for
security (`system/core/init/util.cpp`), and the file had landed as `-rw-rw-r--` after `docker cp`
instead of the `-rw-r--r--` the other `.rc` files in that directory have. Fixing the source file's
permissions on the host before `docker cp`-ing it into the stopped-but-not-yet-started container
fixed it — confirmed `AMD_DEBUG=nodcc` present in `surfaceflinger`'s own `/proc/PID/environ`.

With DCC disabled, the crash was gone — scrcpy displayed real decoded video for the first time.
But the picture was corrupted (vertical striping), which is the classic symptom of GPU-tiled
memory being read as if it were plain linear raster: `AMD_DEBUG=nodcc` only disables
*compression*, not *tiling* itself, and the daemon's VA-API import
(`VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME`) has no way to describe a tiling/modifier at all — it
always assumes linear.

**Bug 2 — tiling/stride guessing.** Rather than fight Mesa into forcing fully linear allocation,
used the real metadata gralloc already attaches to the buffer: `external/minigbm`'s
`cros_gralloc_handle` (`cros_gralloc/cros_gralloc_handle.h`) packs the actual width, height,
per-plane strides/offsets, and the DRM format modifier directly into the buffer's own
`native_handle_t`. Read it directly in `process()` (`reinterpret_cast` the `C2Handle*` to
`cros_gralloc_handle_t`, `header_libs: ["minigbm_headers"]` in `Android.bp` for the include path),
extended `protocol.h`'s `EncodeRequest` with a `drm_format_modifier` field, and switched the
daemon from the old modifier-blind `VASurfaceAttribExternalBuffers` (`DRM_PRIME`) import to the
modifier-aware `VADRMPRIMESurfaceDescriptor` (`DRM_PRIME_2`), which lets the driver interpret
whatever tiling layout the buffer actually has instead of assuming linear.

**Confirmed real hardware H.264 encode now runs end to end for a genuine, unmodified app** — no
crash, valid decodable video displayed by `scrcpy` via the real `MediaCodec` framework path. This
is the actual Tier 5 milestone.

**New, different bug found closing this one out**: the `cros_gralloc_handle` cast that worked for
every earlier tier's *self-allocated* test buffers does NOT hold for this real Surface-sourced
buffer — a raw dump of the handle's ints showed only 23 payload ints (92 bytes) where the full
struct needs 36 (144 bytes), and the values present (a 2560-byte row stride against a 640-pixel
width — exactly 4 bytes/pixel) point to the real buffer being **RGBA, not NV12**. Our interface
never declared an expected pixel format, so `CCodec` left the input as
`OMX_COLOR_FormatAndroidOpaque` (confirmed in logcat: `color-format = 2130708361`) and
`GraphicBufferSource` handed over SurfaceFlinger's raw GL-composited output instead of running its
own RGBA→YUV conversion pass first. Next tier: either find what makes `GraphicBufferSource` do
that conversion (the "correct", standard path real hardware encoders rely on), or accept RGBA and
convert to NV12 before handing frames to the daemon (VA-API can also take an RGB32 surface
directly and do the colorspace conversion itself during encode).

## 2026-09-21 (same day) — Ruling out "make Android give us real YUV" before trying it

Before writing any conversion code, checked whether the RGBA-vs-NV12 problem the previous entry
ended on could be solved for free by getting `GraphicBufferSource` to hand over real YUV instead
of RGBA — the assumption being that real hardware encoders on real phones receive YUV this way, so
surely AOSP has a standard mechanism for it. Two findings kill that assumption for this specific
stack, both worth having on record so a future session (or someone else hitting this) doesn't
re-walk the same dead end:

1. **No such automatic-conversion code exists in the encoder-input path.** Grepped all of
   `frameworks/av/media/codec2/` for the color-conversion helpers AOSP does have
   (`ConvertRGBToPlanarYUV`, `GraphicView2MediaImageConverter` in
   `sfplugin/Codec2Buffer.cpp`/`sfplugin/utils/Codec2BufferUtils.cpp`) and traced every call site.
   All four are on the *decoder output* side (`GraphicBlockBuffer`/`ConstGraphicBlockBuffer`,
   which expose a decoded `C2GraphicBlock` to the app as a `ByteBuffer`/`MediaImage2`). Nothing in
   this tree automatically converts an encoder's Surface-sourced *input* from RGBA to YUV. The
   "real encoders just get YUV" behavior on real phones apparently comes from their vendor
   gralloc/GPU actually being able to allocate and render into a genuine YUV buffer directly, not
   from a framework-level conversion pass — which is the second finding:

2. **This driver stack cannot allocate a YUV buffer that's also GPU-renderable, full stop.**
   `external/minigbm/amdgpu.c`'s own format-combination table (`amdgpu_add_combinations`, the code
   that decides what pixel format + tiling a given usage-bit combination is allowed to produce)
   registers `DRM_FORMAT_NV12` as `TILE_TYPE_LINEAR` only for the combination
   `BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE | BO_USE_SCANOUT | BO_USE_HW_VIDEO_DECODER |
   BO_USE_HW_VIDEO_ENCODER | BO_USE_PROTECTED` — notably *without* any GPU-rendering usage bit
   (`BO_USE_RENDERING`/texture usage) in that set. Our buffer needs both at once (SurfaceFlinger's
   GL compositor draws into it *and* our encoder reads it), so no registered combination matches,
   and `cros_gralloc_driver::get_resolved_format_and_use_flags()` falls back to an RGBA render
   target — independent of anything our C2 interface declares. This isn't a config mistake on our
   side to fix; it's this desktop Mesa/radeonsi driver stack (built for GPUs that never need to
   render straight to a YUV target, unlike a real mobile SoC's GPU+display pipeline) not having
   that capability registered at all — and it's unclear it's even physically possible on this
   hardware, not just unregistered.

**Conclusion**: "get real YUV for free" is not a live option here without patching minigbm's own
driver (and possibly running into a genuine Mesa/hardware limitation underneath that). Ruled out
in favor of accepting RGBA and converting it explicitly — either in software or via VA-API's own
video post-processing (VPP) pipeline, investigation continuing.

## 2026-09-21 (same day) — Tier 5.8: real hardware H.264 encode works end to end for a real app, closing the loop

Between CPU-side conversion and VA-API's own video post-processing (VPP) entrypoint, checked VPP
first: `vainfo` already showed `VAProfileNone : VAEntrypointVideoProc` on this driver, and a small
probe (`vaQuerySurfaceAttributes`/`vaQueryVideoProcFilters` against a `VAEntrypointVideoProc`
config) confirmed its surface attributes list both the RGBA family (input) and the YUV family
(output) simultaneously in the same context, with format/colorspace conversion being inherent to
VPP rather than gated behind one of its optional filters (only deinterlacing is listed as a
filter). Chose VPP over CPU conversion for two reasons: it reuses the modifier-aware `DRM_PRIME_2`
import Tier 5.7 already built (a CPU path would still need the driver to detile the buffer first
anyway, since a real Surface-sourced frame can be GPU-tiled), and it keeps every pixel on the GPU,
never touched by the CPU.

**Implementation** (`tier5-vaapi-daemon/daemon.c`): a second `VAConfigID`/`VAContextID` pair for
`VAEntrypointVideoProc`, and a third persistent surface (`surfaces[2]`, VA-allocated internally —
no gralloc tiling/modifier question for this one, the driver owns it end to end) that VPP writes
NV12 into and the existing encode context then reads from directly, replacing the old "encode
straight from the imported surface" flow. `encode_one_frame()` now: imports the client's dma-buf as
RGBA (`VA_FOURCC_RGBA`/`DRM_FORMAT_ABGR8888` — confirmed back in tier3-dmabuf-import that
`VA_FOURCC_RGBA`'s byte order matches Android's `RGBA_8888` directly; the DRM name is the *opposite*
letter order because DRM format names describe bit-packing in a little-endian word, not byte
order), runs one `vaBeginPicture`/`vaRenderPicture(VAProcPipelineParameterBuffer)`/`vaEndPicture`
pass against the VPP context to convert it, then hands the result to the unchanged encode sequence.

**Validated the new VPP code in isolation before ever touching the real buffer again**: wrote
`rgba-test-client.c`, a host-side client that allocates a genuinely linear DRM dumb buffer (same
technique as `tier3-dmabuf-import`), fills it with solid RGBA red via a direct `mmap()` write (the
daemon never involved in writing it), and sends it through the exact same protocol. Decoded the
response: `(231, 0, 1)` — correct red, off only by the expected H.264/BT601 quantization loss. This
isolated the VPP+encode code itself as correct, narrowing the remaining bug specifically to the
stride/modifier assumptions for the *real* Android buffer.

**Used `adb shell screencap` to see what the real content actually was**, comparing it against the
corrupted decode from the real pipeline (the first attempt, before the modifier fix below, showed
a dominant blue background with faint white horizontal noise). Woke the device first
(`input keyevent KEYCODE_WAKEUP` + `svc power stayon true` — it had gone to sleep, screencap of a
sleeping display is just black). The real content was the Android setup wizard's "Hi there" screen
— a blue gradient background with white text and a yellow button. The dominant color matched
exactly; only fine spatial detail (the text, the button) was destroyed. That specific failure
mode — correct color, scrambled fine detail — is the signature of GPU-tiled memory read as if it
were linear raster (a pure stride mistake instead produces diagonal shearing, not this), which
reopened the modifier question rather than closing it: `AMD_DEBUG=nodcc` only disables
*compression*, and the buffer is still tiled underneath.

**Determining the real modifier without being able to read it from the buffer.** The buffer's own
native handle isn't gralloc's `cros_gralloc_handle` at all (confirmed authoritatively via that
struct's own validation logic, `cros_gralloc_convert_handle()` in `cros_gralloc_helpers.cc`, which
requires the handle's total allocated size to exactly equal `sizeof(cros_gralloc_handle)`; this
one is 108 bytes against the 156 required) — some other, unidentified, more generic
`native_handle_t` this specific pipeline uses instead, with no modifier field to read at all.
Tried querying the kernel/driver directly a few ways: `amdgpu_bo_query_info()`'s `tiling_info` is
legacy metadata minigbm's `amdgpu.c` never populates (confirmed: no
`amdgpu_bo_set_metadata`/`amdgpu_bo_query_info` calls anywhere in that file — this driver stack
uses the modern per-buffer modifier mechanism exclusively, not that sideband channel), and
`gbm_bo_get_modifier()` returned `DRM_FORMAT_MOD_INVALID` even via `gbm_bo_create_with_modifiers2`
with an unconstrained modifier list — this minigbm build's `gbm_bo_get_modifier()` just doesn't
surface it, tried or not. Solved it by reasoning about the *allocator's own decision process*
instead of querying the buffer: minigbm's `amdgpu_add_kms_item()` in `amdgpu.c` registers one combo
per modifier Mesa reports via `dri_query_modifiers()` for a GPU-render-target `ABGR8888` buffer,
all at the same priority; `drv_get_combination()` (`drv.c`) breaks priority ties by keeping
whichever combo was registered *first*, so whichever modifier Mesa's query returns first is
deterministically the one minigbm picks. Queried that same order directly with
`eglQueryDmaBufModifiersEXT` for `DRM_FORMAT_ABGR8888` on this exact GPU, `AMD_DEBUG=nodcc` set the
same way `surfaceflinger` has it: four modifiers, all with `DCC=0` in their `AMD_FMT_MOD` bits
(confirms `nodcc` really does affect this list, not just the encode-time DCC check) — the first,
`0x0200000000401a01` (`AMD_FMT_MOD_TILE_VER_GFX9`, tile `GFX9_64K_D_X`), is therefore the real one,
not a guess among the four.

**Confirmed working**: hardcoded that modifier, rebuilt, redeployed, and had the real app re-tested
against the real pipeline — clean, correctly-colored, correctly-detailed video, confirmed visually.
Real hardware H.264 encode now runs end to end, from a genuine, unmodified Android app
(`scrcpy`, via the real `MediaCodec`/`Codec2Client` framework path) capturing real screen content,
through VA-API/VCN on real AMD hardware, decoded correctly on the other end. This is the actual
Tier 5 milestone, fully closed — not just "doesn't crash" but the picture is right.

**Known limitation to flag honestly**: the modifier is a hardcoded constant, correct for this
specific GPU family (GFX9/Renoir) and this specific driver's modifier-ordering behavior today. It
is not derived from anything portable — a different AMD GPU generation, a Mesa update that
reorders `dri_query_modifiers()`'s results, or Intel/NVIDIA entirely would need this re-derived the
same way (query `eglQueryDmaBufModifiersEXT` for the target format/GPU, take the first non-DCC
entry). Worth eventually replacing with something that determines this at runtime rather than at
build time, once there's a second GPU target to prove a general mechanism against.

## 2026-09-21 (same day) — Tier 5.9: the daemon determines its own GPU's modifier instead of hardcoding it

That "second GPU target" showed up immediately: extending to a machine with a discrete AMD card
(Polaris/GFX8, a completely different tiling generation from server01's Renoir/GFX9) made the
hardcoded-modifier limitation from the previous entry a real, immediate problem rather than a
someday one — and pointed at a real architecture mistake worth fixing before copying anything:
the modifier had been hardcoded into `VaapiEncComponent.cpp`, the *Android-side* component, which
only builds inside the one AOSP tree on server01. Every new host GPU would have needed a
cross-machine AOSP rebuild just to change one constant, even though the value is purely a host-GPU
fact the *daemon* (which already runs once per host, already talks to that host's GPU directly)
is in a much better position to know.

Moved the whole determination into `daemon.c` instead: `rgba_modifier_init()`, called once at
daemon startup, opens the render node via GBM, gets an EGL display from it, and calls
`eglQueryDmaBufModifiersEXT` for `DRM_FORMAT_ABGR8888` — the same technique the previous entry
used from a throwaway probe, now permanent code. `VaapiEncComponent.cpp` now just sends 0 for
`drm_format_modifier`, and the daemon ignores whatever it's sent, using its own
`st->rgba_modifier` instead. Net effect: a new host GPU now only ever needs this daemon
recompiled locally (plain `gcc`, no AOSP, seconds not tens of minutes) — the Android-side vendor
image is unchanged and identical across every machine.

**Two real bugs surfaced getting this right, both about *when* the query runs relative to when
Mesa reads `AMD_DEBUG`:**
1. First version called `eglQueryDmaBufModifiersEXT` without setting `AMD_DEBUG=nodcc` for the
   daemon's own process at all. Server01 abruptly reported *8* modifiers instead of the expected
   4-5, and the first one decoded to `DCC=1` — because nothing here disables DCC for the daemon's
   own queries, only `surfaceflinger` (inside the Android container) has that env var exported to
   it. Querying without it answers a different question than "what modifier will the real buffer
   actually have" (the real buffer's allocator, minigbm inside Android, *does* have nodcc active).
2. Fixed that by calling `setenv("AMD_DEBUG", "nodcc", 1)` right before the query — and got the
   *same* wrong 8-modifier, `DCC=1` answer again. Mesa parses its debug env vars once per process
   and caches the result (a `static` guard inside its debug-string parsing, shared across every
   Mesa entry point in that process, not per-driver-instance) — by the time `rgba_modifier_init()`
   ran, `vaInitialize()` had already loaded the VA-API driver and Mesa had already read (and
   cached) the process's original environment. Moved the `setenv()` call to the very first line of
   `main()`, before *any* Mesa/VA-API/EGL call — confirmed fixed: back to 5 modifiers, first one
   `0x0200000000401a01`, matching the value hand-derived and hardcoded in the previous entry
   exactly. Re-tested against the real pipeline end to end on server01 afterward to confirm the
   refactor changed nothing observable — same correct picture as before.
