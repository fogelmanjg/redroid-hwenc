/*
 * Tier 5.4, step 1: pure diagnostic. Allocate a REAL Android gralloc buffer
 * via AHardwareBuffer (the stable, correct way to get one from native code
 * -- no need to scavenge another process's fd via pidfd_getfd like the
 * Tier 3 real-gralloc spike had to, now that we have the full AOSP tree
 * and can link against libnativewindow directly) with
 * AHARDWAREBUFFER_USAGE_VIDEO_ENCODE, and print exactly how this gralloc
 * implementation lays out a flexible YUV420 buffer: plane count, strides,
 * and the underlying native_handle_t's fd count/values. AHardwareBuffer
 * only exposes one generic "Y8Cb8Cr8_420" format -- whether that's
 * semi-planar (NV12-like, what our daemon protocol currently expects) or
 * fully planar (I420/YV12-like) is a gralloc implementation detail we need
 * to observe empirically before deciding whether protocol.h needs to
 * change.
 */

#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
#include <android/hardware_buffer.h>
#include <vndk/hardware_buffer.h>

#define WIDTH 320
#define HEIGHT 240

int main(void) {
    AHardwareBuffer_Desc desc = {0};
    desc.width = WIDTH;
    desc.height = HEIGHT;
    desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420;
    desc.usage = AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_VIDEO_ENCODE;

    AHardwareBuffer *buf = NULL;
    int ret = AHardwareBuffer_allocate(&desc, &buf);
    if (ret != 0) {
        fprintf(stderr, "AHardwareBuffer_allocate failed: %d\n", ret);
        return 1;
    }
    printf("Allocated a real Android gralloc buffer (AHARDWAREBUFFER_USAGE_VIDEO_ENCODE)\n");

    AHardwareBuffer_Desc real_desc = {0};
    AHardwareBuffer_describe(buf, &real_desc);
    printf("Real desc: %ux%u, format=0x%x, stride=%u (pixels), usage=0x%llx\n",
           real_desc.width, real_desc.height, real_desc.format, real_desc.stride,
           (unsigned long long)real_desc.usage);

    AHardwareBuffer_Planes planes;
    ret = AHardwareBuffer_lockPlanes(buf, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL, &planes);
    if (ret != 0) {
        /* -38 (ENOSYS) confirmed on this build: this Mapper HAL version
         * (android.hardware.graphics.mapper@2.0-impl-2.1) doesn't
         * implement the flexible-layout/lockYCbCr verb lockPlanes needs.
         * Not fatal -- we don't actually need AHardwareBuffer's own CPU
         * access API at all. The dma-buf fd we get from
         * getNativeHandle() below supports a plain mmap() directly,
         * completely bypassing this broken/unimplemented API, the same
         * way the Tier 3 spike wrote into a foreign dma-buf without any
         * Android involvement at all. */
        fprintf(stderr, "AHardwareBuffer_lockPlanes failed: %d (expected on this Mapper "
                         "version -- falling back to plain mmap() of the dma-buf fd)\n", ret);
    } else {
        printf("planeCount=%u\n", planes.planeCount);
        for (uint32_t i = 0; i < planes.planeCount; i++) {
            printf("  plane[%u]: data=%p pixelStride=%u rowStride=%u\n",
                   i, planes.planes[i].data, planes.planes[i].pixelStride, planes.planes[i].rowStride);
        }
        AHardwareBuffer_unlock(buf, NULL);
    }

    const native_handle_t *handle = AHardwareBuffer_getNativeHandle(buf);
    if (!handle) {
        fprintf(stderr, "AHardwareBuffer_getNativeHandle returned NULL\n");
        return 1;
    }
    printf("native_handle: numFds=%d numInts=%d\n", handle->numFds, handle->numInts);
    for (int i = 0; i < handle->numFds; i++) {
        printf("  data[%d] (fd) = %d\n", i, handle->data[i]);
    }

    if (handle->numFds >= 1) {
        int dmabuf_fd = handle->data[0];
        off_t size = lseek(dmabuf_fd, 0, SEEK_END);
        lseek(dmabuf_fd, 0, SEEK_SET);
        printf("dma-buf fd[0] size (via lseek) = %ld bytes (WIDTH*HEIGHT*1.5 = %ld)\n",
               (long)size, (long)(WIDTH * HEIGHT * 3 / 2));
        void *map = mmap(NULL, size > 0 ? (size_t)size : WIDTH * HEIGHT * 3 / 2,
                          PROT_READ | PROT_WRITE, MAP_SHARED, dmabuf_fd, 0);
        if (map == MAP_FAILED) {
            perror("mmap(dma-buf fd)");
        } else {
            printf("Plain mmap() of the dma-buf fd succeeded: %p -- this is what we'll use "
                   "to fill and send this buffer in the next step, no lockPlanes needed.\n", map);
            munmap(map, size > 0 ? (size_t)size : WIDTH * HEIGHT * 3 / 2);
        }
    }

    AHardwareBuffer_release(buf);
    return 0;
}
