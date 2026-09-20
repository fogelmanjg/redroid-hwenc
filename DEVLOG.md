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
