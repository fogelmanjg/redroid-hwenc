# Intel Haswell / HD Graphics 4400

This fork adds an opt-in **i965/Haswell H.264 encoding profile** to
[fogelmanjg/redroid-hwenc](https://github.com/fogelmanjg/redroid-hwenc).
Upstream already supports Intel Iris Xe. These changes address a different,
older GPU and an Android 16 image using the cros/minigbm allocator.

## What was verified

The deployed combination used an Intel Core i3-4030U / HD Graphics 4400,
i915, the i965 VA-API driver, Android 16 x86_64 and the upstream v0.2.0
Android 15 AIDL service with the four-byte adapter below. Hardware H.264
capture produced non-black video that passed strict FFmpeg decoding.
The unmodified cros allocator completed a 305-frame allocation probe; the
GBM allocator used in the original setup crashed with larger BLOB buffers.

The deployed daemon was built from redroid-forge commit
`d711e9c292e0b7013918681633e88c02d72a3387` plus the two overrides below.
This fork ports those overrides onto redroid-hwenc commit `98f9f42`.
Its daemon Docker image has been built successfully, but **this new fork
image has not replaced or been hardware-tested in the deployed instance**.

The C++ input-layout helper has standalone tests. The **complete modified
AOSP component has not been rebuilt or tested on a device**. For the
previously verified guest implementation, use the pinned binary adapter,
not an unverified claim that this source tree produced that guest binary.

Only H.264 encoding on this Haswell machine is covered by these additions.
They do not establish hardware decode, HEVC encode, or compatibility with
every Intel GPU. Do not apply this i965 profile blindly to newer iHD GPUs.

## Changes

* `REDROID_VAAPI_LEGACY_PRIME=1` forces the existing legacy PRIME import
  path after modifier discovery. On the tested Haswell GPU, PRIME2 accepted
  the dma-buf but encoded black output. Other GPUs retain upstream defaults.
* `REDROID_VAAPI_FULL_RANGE=1` sets FFmpeg's JPEG/full color range and
  SMPTE170M colorspace. This corrects washed-out levels on the tested i965
  conversion path. It is not a universal color setting for every driver.
* The source encoder reads cros width, height and byte stride at offsets
  **84, 88 and 32**, including the native-handle header, when
  `ro.hardware.gralloc=cros`. cros reserves five fd slots, so these offsets
  do not depend on `numFds`. Other allocator names retain the original GBM
  layout. The helper rejects short handles and invalid RGBA dimensions.
* `zz-redroid-hwenc.rc` selects gralloc-backed BLOB output allocation,
  avoiding a dependency on ion/dma_heap. It does not replace gralloc or
  install a patched GBM allocator.

The two daemon overrides are enabled by **presence** of their environment
variables, matching the tested patches. Unset them to disable them;
setting a variable to `0` does not disable it.

## Host daemon build

Run from the repository root. The Dockerfile uses relative project inputs;
it has no dependency on the checkout's location.

```sh
docker build -f intel/Dockerfile -t redroid-hwenc:haswell .
```

The image builds the daemon with `HWDEC=1`, which also enables the FFmpeg
encode backend. It includes i965 and selects `ffmpeg`, legacy PRIME and
full range. Inclusion of decode code is not a decode verification claim.

To connect it to an **already configured** Android container:

```sh
mkdir -p run
docker run --rm --name redroid-vaapi --network none \
  --device /dev/dri/renderD128 \
  --mount "type=bind,src=$(pwd)/run,dst=/app/run" \
  redroid-hwenc:haswell
```

Check which render node belongs to Intel first, especially on hybrid-GPU
machines; `renderD128` is an example, not a discovery algorithm. If using a
different node, pass that device and set `REDROID_FORGE_DRM_NODE` to it.
The Android container must see the **same `run` directory** at
`/dev/vaapi-helper`; its Codec2 service connects to
`/dev/vaapi-helper/socket`. This Unix socket passes DMA-BUF file descriptors;
the daemon and Android must run on the same host.

The example does not launch Android, grant GPU access automatically, or
modify a running deployment. See the upstream
[Tier 5 instructions](../tier5-vaapi-daemon/README.md) for installing the
Codec2 service, VINTF declarations, codec XML and library dependencies.
Start with an Android image that already boots and renders on your host.
Host kernel/Binder setup and Android-version compatibility remain separate
prerequisites, not changes made by this fork.

## Guest option A: reproduce the verified binary adaptation

This option supports **only** the original v0.2.0 x86_64 AIDL service.
It replaces two instruction operands (four bytes total) to load fixed cros
width/height/stride offsets. It does not patch the allocator itself.

Download the public upstream artifact and verify its digest before
extracting it into a new directory:

```sh
mkdir -p .tmp
curl -fL -o .tmp/vaapi-v0.2.0.tar.gz \
  https://github.com/fogelmanjg/redroid-hwenc/releases/download/v0.2.0/redroid-hwenc-vaapi-artifacts-v0.2.0.tar.gz
printf '%s  %s\n' \
  8263a2e50084b39648150c202e4633d4aeec21d6f6dd2eada392ec6c2742b9a6 \
  .tmp/vaapi-v0.2.0.tar.gz | sha256sum -c -
mkdir .tmp/vendor-v0.2.0
tar -xzf .tmp/vaapi-v0.2.0.tar.gz -C .tmp/vendor-v0.2.0
find .tmp/vendor-v0.2.0 -type f -name '*vaapi*'
```

Pass the extracted **original service binary** and a new output filename:

```sh
python3 intel/patch_vendor.py \
  .tmp/vendor-v0.2.0/bin/hw/android.hardware.media.c2-vaapi-service \
  .tmp/service.cros
```

The original service SHA-256 must be
`8dd025fdbfc281341f92e2d858ce661eb11e1f454d255274568da064fdcdad9a`.
The patcher rejects other binaries, checks the original instructions and
refuses to overwrite an existing output. It never installs files.

The patched service SHA-256 is
`efbe598b6cb0683e9791adea36efb1fba3057c8cdb9215e03a26e7f537e0fccc`.
The patcher's output was compared byte-for-byte with the existing deployment
builder and its digest matched the service in the running Android container.

Install the output in place of the upstream AIDL service when building
your Android vendor overlay. Keep the original cros allocator and boot
Android with `ro.hardware.gralloc=cros`. The binary adapter **unconditionally
expects cros**; unlike the source implementation, it has no GBM fallback.
Use `zz-redroid-hwenc.rc` in the vendor init directory if the BLOB/ion
allocation settings are needed on your image. Preserve the rest of the
upstream vendor service, manifest, codec XML and dependencies.

## Guest option B: rebuild the source component

Use the upstream AOSP build procedure with this checkout's
`tier5-vaapi-daemon/codec2-component` directory. `libcutils` is already an
upstream dependency; the source uses it to read `ro.hardware.gralloc`.
Set that property to `cros` only when cros is the actual allocator. Test
the resulting service before substituting it for the verified binary
adapter. This fork does not ship a rebuilt AOSP service or Android image.

## Checks

From the repository root, with Python 3 and a C++ compiler installed:

```sh
python3 -m unittest discover -s intel -v
mkdir -p .tmp
c++ -std=c++17 -Wall -Wextra -Werror intel/test_handle_layout.cpp \
  -o .tmp/test-handle-layout
.tmp/test-handle-layout
```

For runtime acceptance, select `c2.hardware.encoder.h264` explicitly in
your capture client, confirm the daemon receives frames and verify that
the service stays alive. Record a moving screen and decode it strictly:

```sh
ffmpeg -v error -err_detect explode -i capture.h264 -f null -
```

Successful decoding alone does not rule out an all-black stream; inspect
decoded frames and their range against the Android screen. Test a sustained
capture, not just a codec-list entry. No software fallback is configured
by this profile. Client-side encoder selection is still your responsibility.

The Intel work does **not** include private deployment files, Android
userdata, credentials, network-specific MTU/route hooks or changes to
EasyControlNext. Those are separate from GPU support. In particular,
capture scaling and transport latency must not be reported as an Intel
driver fix or a universal latency benchmark.

## Credits and license

The Codec2/VA-API implementation and original research are by fogelmanjg
and upstream contributors. This fork preserves their history and
[Apache-2.0 license](../LICENSE). The deployed daemon reference above is
from their redroid-forge project. The cros handle layout was checked against
[AOSP minigbm, Android 15](https://android.googlesource.com/platform/external/minigbm/+/refs/tags/android-15.0.0_r1/cros_gralloc/cros_gralloc_handle.h).
