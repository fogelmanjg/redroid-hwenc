/*
 * Tests the daemon's EGL bridge (Tier 5.10) against the same *class* of
 * buffer the real Android pipeline actually hands it -- a
 * GBM_BO_USE_RENDERING allocation (opaque tiling on this GPU), not a plain
 * KMS dumb buffer (which may not be GPU-texture-sample-able at all,
 * regardless of the bridge code's correctness -- dumb buffers are a
 * display/CPU-map mechanism, not necessarily wired into the 3D sampling
 * path). Fills it with a known solid color via gbm_bo_map() (same-process,
 * so this driver's own detile-aware write path handles the opaque tiling
 * correctly), exports it, and sends it through the exact same daemon
 * protocol a real request would use.
 *
 * Also includes a same-process self-check (re-importing its own exported
 * fd via EGL_LINUX_DMA_BUF_EXT before ever sending it anywhere) -- this is
 * what caught the Tier 5.10 bridge's real problem: that self-check passes
 * on Polaris/GFX8 while the daemon's genuinely cross-process import of the
 * identical fd fails, proving the earlier "confirmed working" standalone
 * probe had tested the wrong scenario (same-process, which Mesa apparently
 * shortcuts) instead of the one that actually matters. See DEVLOG.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <gbm.h>
#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>

#include "protocol.h"

#define WIDTH 320
#define HEIGHT 240
#define DRM_RENDER_DEVICE "/dev/dri/renderD128"
#define OUTPUT_FILE "gbm-render-test-out.h264"

#define FILL_R 255
#define FILL_G 0
#define FILL_B 0
#define FILL_A 255

int main(void) {
    int fd = open(DRM_RENDER_DEVICE, O_RDWR);
    if (fd < 0) {
        perror("open " DRM_RENDER_DEVICE);
        return 1;
    }
    struct gbm_device *gbm = gbm_create_device(fd);
    if (!gbm) {
        fprintf(stderr, "gbm_create_device failed\n");
        return 1;
    }

    struct gbm_bo *bo = gbm_bo_create(gbm, WIDTH, HEIGHT, GBM_FORMAT_ABGR8888,
                                       GBM_BO_USE_RENDERING);
    if (!bo) {
        fprintf(stderr, "gbm_bo_create(RENDERING) failed\n");
        return 1;
    }

    uint32_t map_stride;
    void *map_data = NULL;
    void *mapped = gbm_bo_map(bo, 0, 0, WIDTH, HEIGHT, GBM_BO_TRANSFER_WRITE, &map_stride,
                               &map_data);
    if (!mapped) {
        fprintf(stderr, "gbm_bo_map failed\n");
        return 1;
    }
    for (int y = 0; y < HEIGHT; y++) {
        unsigned char *row = (unsigned char *)mapped + y * map_stride;
        for (int x = 0; x < WIDTH; x++) {
            row[x * 4 + 0] = FILL_R;
            row[x * 4 + 1] = FILL_G;
            row[x * 4 + 2] = FILL_B;
            row[x * 4 + 3] = FILL_A;
        }
    }
    gbm_bo_unmap(bo, map_data);

    int dmabuf_fd = gbm_bo_get_fd(bo);
    if (dmabuf_fd < 0) {
        fprintf(stderr, "gbm_bo_get_fd failed\n");
        return 1;
    }
    uint32_t stride = gbm_bo_get_stride(bo);
    printf("Test buffer: %dx%d RGBA (solid %d,%d,%d,%d) on a GBM_BO_USE_RENDERING bo, "
           "stride=%u, dma-buf fd=%d\n",
           WIDTH, HEIGHT, FILL_R, FILL_G, FILL_B, FILL_A, stride, dmabuf_fd);

    /* Self-check: does plain EGL_LINUX_DMA_BUF_EXT (no modifier) import of
     * THIS SAME fd work within THIS SAME process, same as the earlier
     * standalone probe found? This isolates same-process vs cross-process
     * (the daemon receiving the fd via SCM_RIGHTS from a different
     * process, which is what actually matters) as the daemon's failure
     * cause. */
    {
        EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL);
        EGLint major, minor;
        eglInitialize(dpy, &major, &minor);
        eglBindAPI(EGL_OPENGL_ES_API);
        EGLint cfg_attribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
        EGLConfig cfg;
        EGLint num_cfg = 0;
        eglChooseConfig(dpy, cfg_attribs, &cfg, 1, &num_cfg);
        EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
        EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
        PFNEGLCREATEIMAGEKHRPROC pCreate =
            (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
        PFNGLEGLIMAGETARGETTEXTURE2DOESPROC pTarget =
            (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
        EGLint attribs[] = {
            EGL_WIDTH, WIDTH, EGL_HEIGHT, HEIGHT,
            EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ABGR8888,
            EGL_DMA_BUF_PLANE0_FD_EXT, dmabuf_fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)stride,
            EGL_NONE,
        };
        EGLImageKHR img = pCreate(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, (EGLClientBuffer)NULL, attribs);
        printf("SELF-CHECK (same process): eglCreateImageKHR -> %s\n",
               img == EGL_NO_IMAGE_KHR ? "FAILED" : "ok");
        if (img != EGL_NO_IMAGE_KHR) {
            GLuint tex;
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            pTarget(GL_TEXTURE_2D, img);
            printf("SELF-CHECK (same process): glEGLImageTargetTexture2DOES -> err=0x%x\n", glGetError());
        }
    }

    int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("socket");
        return 1;
    }
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, VAAPI_DAEMON_SOCKET_PATH, sizeof(addr.sun_path) - 1);
    if (connect(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("connect");
        return 1;
    }

    EncodeRequest req = {
        .width = WIDTH,
        .height = HEIGHT,
        .stride_y = stride,
        .stride_uv = 0,
        .offset_uv = 0,
        .dmabuf_size = stride * HEIGHT,
        .drm_format_modifier = 0,
    };

    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    struct iovec iov = {.iov_base = &req, .iov_len = sizeof(req)};
    struct msghdr msg = {0};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &dmabuf_fd, sizeof(int));

    if (sendmsg(sock_fd, &msg, 0) < 0) {
        perror("sendmsg");
        return 1;
    }
    printf("Sent EncodeRequest + dma-buf fd to daemon, waiting for response...\n");

    EncodeResponse resp;
    if (read(sock_fd, &resp, sizeof(resp)) != (ssize_t)sizeof(resp)) {
        perror("read(response header)");
        return 1;
    }
    if (resp.status != 0) {
        fprintf(stderr, "Daemon reported an error (status=%d)\n", resp.status);
        return 1;
    }

    unsigned char *coded = malloc(resp.coded_size);
    size_t got = 0;
    while (got < resp.coded_size) {
        ssize_t n = read(sock_fd, coded + got, resp.coded_size - got);
        if (n <= 0) {
            perror("read(response body)");
            return 1;
        }
        got += n;
    }

    FILE *out = fopen(OUTPUT_FILE, "wb");
    fwrite(coded, 1, resp.coded_size, out);
    fclose(out);
    printf("\n*** Received %u bytes of H.264 from the daemon, wrote %s ***\n", resp.coded_size,
           OUTPUT_FILE);

    free(coded);
    close(sock_fd);
    close(dmabuf_fd);
    return 0;
}
