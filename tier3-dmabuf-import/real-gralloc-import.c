/*
 * Tier 3 closing spike: import a dma-buf that is a REAL Android gralloc
 * buffer -- not a synthetic DRM dumb buffer -- pulled live from a running
 * redroid container's video pipeline, and confirm VA-API accepts it.
 *
 * The buffer comes from `media.codec` (the OMX video codec HAL process)
 * inside a redroid instance actively running `screenrecord`: it holds the
 * dma-buf fd(s) for the gralloc buffer(s) it receives from SurfaceFlinger's
 * virtual-display BufferQueue before doing its own (software) color
 * conversion and encode. Confirmed via ffprobe on the actual recorded
 * stream that this is a 720x1280 capture; the dma-buf's own size
 * (3,932,160 bytes = 3072-byte stride x 1280 rows) matches RGBA8888 with a
 * stride padded to a 256-byte-aligned 3072 (720px x 4 bytes/px = 2880,
 * rounded up) -- i.e. this is SurfaceFlinger's composited RGBA output
 * buffer, not a YUV buffer already prepared for the encoder. Real gralloc
 * buffers this project's Tier 5 encoder would actually consume would need
 * this same RGBA-in path (or a NV12 buffer if fed post-conversion) -- this
 * spike targets the import mechanism itself, not a full CSC+encode pass.
 *
 * A plain `open("/proc/PID/fd/N")` does NOT work for dma-buf fds -- dma-buf
 * is backed by an anonymous inode, and the anon_inode file type's default
 * f_ops->open returns -ENXIO specifically to block that reopen trick
 * (confirmed empirically: this is what a first attempt via that route hit).
 * The correct, intended mechanism for duplicating an fd into another
 * process by number is pidfd_getfd(2) (Linux 5.6+), which explicitly
 * supports anonymous-inode fds -- it's what CRIU and debuggers use for
 * exactly this. That's what this program uses.
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>

#define DRM_RENDER_DEVICE "/dev/dri/renderD128"

#define CHECK_VA(status, msg)                                                \
    do {                                                                     \
        if ((status) != VA_STATUS_SUCCESS) {                                 \
            fprintf(stderr, "%s failed: %s (0x%x)\n", (msg),                 \
                    vaErrorStr(status), (status));                           \
            exit(1);                                                        \
        }                                                                    \
    } while (0)

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <host-pid> <target-fd-number>\n", argv[0]);
        return 1;
    }
    pid_t target_pid = (pid_t)atoi(argv[1]);
    int target_fd = atoi(argv[2]);

    long pidfd = syscall(SYS_pidfd_open, target_pid, 0);
    if (pidfd < 0) {
        perror("pidfd_open");
        return 1;
    }
    long dmabuf_fd = syscall(SYS_pidfd_getfd, pidfd, target_fd, 0);
    if (dmabuf_fd < 0) {
        perror("pidfd_getfd");
        return 1;
    }
    close(pidfd);

    off_t size = lseek(dmabuf_fd, 0, SEEK_END);
    lseek(dmabuf_fd, 0, SEEK_SET);
    printf("Cloned a REAL gralloc dma-buf from PID %d fd %d -> our fd %ld, size=%ld bytes\n",
           target_pid, target_fd, dmabuf_fd, (long)size);

    /* Confirmed via ffprobe on the live recording: 720x1280. Stride derived
     * from the dma-buf's own size assuming RGBA8888 (4 bytes/px): a
     * 256-byte-aligned row stride of 3072 for a 720px-wide row (720*4=2880,
     * rounded up) x 1280 rows = exactly 3,932,160 bytes -- matches. */
    const uint32_t width = 720, height = 1280, stride = 3072;

    int drm_fd = open(DRM_RENDER_DEVICE, O_RDWR);
    if (drm_fd < 0) {
        perror("open " DRM_RENDER_DEVICE);
        return 1;
    }
    VADisplay dpy = vaGetDisplayDRM(drm_fd);
    if (!dpy) {
        fprintf(stderr, "vaGetDisplayDRM failed\n");
        return 1;
    }
    int major, minor;
    CHECK_VA(vaInitialize(dpy, &major, &minor), "vaInitialize");
    printf("VA-API version %d.%d, driver: %s\n", major, minor,
           vaQueryVendorString(dpy));

    uintptr_t buffer_handles[1] = {(uintptr_t)dmabuf_fd};
    VASurfaceAttribExternalBuffers ext_buf = {0};
    ext_buf.pixel_format = VA_FOURCC_RGBA; /* matches Android's RGBA_8888 byte order */
    ext_buf.width = width;
    ext_buf.height = height;
    ext_buf.data_size = (uint32_t)size;
    ext_buf.num_planes = 1;
    ext_buf.pitches[0] = stride;
    ext_buf.offsets[0] = 0;
    ext_buf.buffers = buffer_handles;
    ext_buf.num_buffers = 1;
    ext_buf.flags = 0;

    VASurfaceAttrib import_attribs[2];
    import_attribs[0].type = VASurfaceAttribMemoryType;
    import_attribs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    import_attribs[0].value.type = VAGenericValueTypeInteger;
    import_attribs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;

    import_attribs[1].type = VASurfaceAttribExternalBufferDescriptor;
    import_attribs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    import_attribs[1].value.type = VAGenericValueTypePointer;
    import_attribs[1].value.value.p = &ext_buf;

    VASurfaceID surface;
    VAStatus import_status = vaCreateSurfaces(dpy, VA_RT_FORMAT_RGB32, width, height,
                                               &surface, 1, import_attribs, 2);
    if (import_status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "\n*** REAL GRALLOC IMPORT: FAILED -- %s (0x%x) ***\n",
                vaErrorStr(import_status), import_status);
        return 1;
    }
    printf("\n*** REAL GRALLOC dma-buf IMPORTED into a VA-API surface SUCCEEDED (surface %u) ***\n",
           surface);

    VAImage image;
    VAStatus derive_status = vaDeriveImage(dpy, surface, &image);
    if (derive_status != VA_STATUS_SUCCESS) {
        printf("vaDeriveImage on the imported real surface failed (non-fatal, same as the "
               "synthetic-buffer spike on this GPU family): %s (0x%x)\n",
               vaErrorStr(derive_status), derive_status);
    } else {
        void *p = NULL;
        CHECK_VA(vaMapBuffer(dpy, image.buf, &p), "vaMapBuffer");
        unsigned char *bytes = (unsigned char *)p + image.offsets[0];
        printf("First 16 bytes of the imported REAL screen buffer (via VA-API readback): ");
        for (int i = 0; i < 16; i++) printf("%02x ", bytes[i]);
        printf("\n");
        int all_zero = 1;
        for (unsigned int i = 0; i < image.data_size && all_zero; i++) {
            if (bytes[i] != 0) all_zero = 0;
        }
        printf("Buffer content: %s\n", all_zero ? "ALL ZERO (suspicious -- not real content)"
                                                  : "non-zero (looks like real screen content)");
        vaUnmapBuffer(dpy, image.buf);
        vaDestroyImage(dpy, image.image_id);
    }

    vaDestroySurfaces(dpy, &surface, 1);
    vaTerminate(dpy);
    close(dmabuf_fd);
    close(drm_fd);
    return 0;
}
