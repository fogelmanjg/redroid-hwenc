# Tier 4: Codec2 hardware component skeleton

Gets Android to recognize and list `c2.hardware.encoder.h264` (via `dumpsys media.c2`) —
without any real encoding wired in yet. That's Tier 5, where `createComponent()` gets a real
implementation built on the encode/import pipeline already proven in `tier2-vaapi-encode`/
`tier3-dmabuf-import`.

Unlike Tier 2/3, this isn't a standalone program you build with `gcc` — it's an AOSP vendor
module. It needs a full Android source tree to build against (Soong, `libcodec2`, AIDL-generated
headers, etc.), so this directory is the *source of truth* for the module's content; actually
building it means dropping these files into `external/vaapi_codec2/service/` inside a redroid AOSP
checkout.

## Where this came from

AOSP ships an official, maintained example of exactly this: an empty Codec2 service at
`frameworks/av/media/codec2/hal/services/vendor.cpp`, whose own header comment says "make a copy
of this whole directory and rename modules accordingly." This is that copy, with two changes:

1. **HIDL path removed.** Tier 0 found this specific redroid build only activates a Codec2 store
   when `media.c2.hal.selection=aidl` (`IsCodec2AidlHalSelected()` gates on it) — the generic
   template's HIDL fallback branch is dead weight here.
2. **`listComponents()` reports one component** (`c2.hardware.encoder.h264`, `DOMAIN_VIDEO`,
   `KIND_ENCODER`, `rank=1`, `video/avc`) instead of the template's empty list.
   `createComponent()`/`createInterface()` still return `C2_NOT_FOUND` — Tier 4 is about
   enumeration, not a working encoder.

The instance name is `vaapi` (checked against a live instance first — only `software` was
registered, `vaapi` was free).

## Build & deploy

Inside a redroid AOSP checkout:

```sh
mkdir -p external/vaapi_codec2/service
cp <these files> external/vaapi_codec2/service/
```

Add to `hardware/redroid/c2/c2.mk` (or wherever the target device's product makefile pulls in
Codec2 packages):

```makefile
PRODUCT_PACKAGES += \
    android.hardware.media.c2-vaapi-service \
```

Then:

```sh
. build/envsetup.sh
lunch redroid_x86_64-ap3a-userdebug   # or your actual target
m android.hardware.media.c2-vaapi-service   # targeted build, ~10 min from a warm /out
m vendorimage                                # repackage vendor.img, seconds once the above is cached
```

**Deploying into a running container without a full image re-import**: these redroid Docker
images are a flattened rootfs (`docker import` of combined system+vendor content), not a mounted
`.img` redroid reads at runtime. To test a change without rebuilding the whole Docker image:

```sh
mount -o loop,ro out/target/product/<target>/vendor.img /mnt/vendor-check
```

then `docker cp` the new/changed files from there into a redroid container's matching `/vendor`
paths — **before** `docker start`, not after (Android's init only parses
`/vendor/etc/init/*.rc` once, early in boot; injecting into an already-booted container won't be
picked up without a full container restart).

## Real bugs hit getting a clean boot (see `DEVLOG.md` for the full account)

- **Only copying the module's own 4 files (binary, `.rc`, manifest fragment, seccomp policy)
  isn't enough.** Adding this module to the build pulled in 16 shared libraries that weren't in
  the stock image before (`libcodec2.so`, `libcodec2_aidl.so`, `libcodec2_vndk.so`, the AIDL
  bufferpool/bufferqueue libs, `libavservices_minijail.so`, `libminijail.so`, `libcap.so`,
  `libdmabufheap.so`, `libion.so`, etc.) — the service crash-loops with `CANNOT LINK EXECUTABLE:
  library "libcodec2.so" not found` until all of them are copied in too. Diff the mounted
  `vendor.img`'s `/vendor/lib64` against the target container's to find exactly which ones are
  new.
- **Never `docker stop` + `docker start` the same container to re-inject files.** That changes
  the container's veth MAC, and the host bridge can be left with stale FDB entries pointing at the
  old one — ARP for the container's IP fails (`No route to host`) even though the container
  itself is completely healthy (`docker exec` still works fine). Recreate instead: `docker rm` +
  fresh `docker create` (inject files while stopped) + one `docker start`. Use a fresh, dedicated
  Docker network per attempt rather than reusing one containers have already churned through.
- **The AOSP build container's own lifecycle matters.** If its entrypoint is an interactive shell
  with no real tty attached (`chroot ... /bin/bash -i`), it can exit on its own eventually — and
  Docker tears down every process in that container's namespace when PID 1 dies, killing any
  build still genuinely in progress (not hung) inside it. Use a durable PID 1 instead
  (`--entrypoint /bin/sleep infinity`) and run actual build commands via `docker exec`.

## Confirmed result (real redroid instance, `redroid-jg-15:wifi-v3` base image)

```
$ service list | grep media.c2
17  android.hardware.media.c2.IComponentStore/software: [...]
18  android.hardware.media.c2.IComponentStore/vaapi: [...]

$ dumpsys android.hardware.media.c2.IComponentStore/vaapi
Beginning of dump -- C2ComponentStore: vaapi

  Supported components:

    name: c2.hardware.encoder.h264
    domain: 1
    kind: 2
    rank: 1
    mediaType: video/avc
    aliases:

  Active components:

    NONE

End of dump -- C2ComponentStore: vaapi
```

## What's next (Tier 5)

- Give `createComponent()`/`createInterface()` a real `C2Component`/`C2ComponentInterface`
  implementation, wiring in the VA-API encode call sequence already proven in
  `tier2-vaapi-encode` and the dma-buf import path proven in `tier3-dmabuf-import`.
- This vendor image doesn't ship `libva`/Mesa's VA-API Gallium state tracker at all today
  (confirmed absent back in Tier 0) — bundling that into the vendor partition is real, necessary
  work before Tier 5's encode logic has anything to call.
