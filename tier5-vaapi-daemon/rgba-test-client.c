/*
 * Tier 5.8 diagnostic: isolates whether the VPP RGBA->NV12 conversion
 * pipeline itself is correct, independent of any guess about the real
 * Android buffer's stride/tiling. Creates a plain DRM dumb buffer (always
 * linear, per tier3-dmabuf-import's own finding), fills it with a known
 * solid RGBA color via a direct mmap() write (VA-API/the daemon never
 * involved in writing it), and sends it through the exact same daemon
 * protocol a real request would use. If the decoded frame comes back the
 * expected color, the VPP+encode code is correct and the real-buffer bug is
 * specifically the stride/modifier guess for that buffer's actual (unknown)
 * handle format; if it's still wrong here, the bug is in this new
 * VPP/RGBA-import code itself.
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include "protocol.h"

#define WIDTH 320
#define HEIGHT 240
#define DRM_PRIMARY_DEVICE "/dev/dri/card1"
#define OUTPUT_FILE "rgba-test-out.h264"

/* Solid red, Android RGBA_8888 byte order (R,G,B,A per pixel). */
#define FILL_R 255
#define FILL_G 0
#define FILL_B 0
#define FILL_A 255

int main(void) {
    int drm_fd = open(DRM_PRIMARY_DEVICE, O_RDWR);
    if (drm_fd < 0) {
        perror("open " DRM_PRIMARY_DEVICE);
        return 1;
    }

    uint32_t handle = 0, stride = 0;
    uint64_t dumb_size = 0;
    if (drmModeCreateDumbBuffer(drm_fd, WIDTH, HEIGHT, 32, 0, &handle, &stride, &dumb_size) != 0) {
        perror("drmModeCreateDumbBuffer");
        return 1;
    }
    int dmabuf_fd = -1;
    if (drmPrimeHandleToFD(drm_fd, handle, DRM_CLOEXEC | DRM_RDWR, &dmabuf_fd) != 0) {
        perror("drmPrimeHandleToFD");
        return 1;
    }

    void *map = mmap(NULL, dumb_size, PROT_READ | PROT_WRITE, MAP_SHARED, dmabuf_fd, 0);
    if (map == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    for (unsigned int row = 0; row < HEIGHT; row++) {
        unsigned char *rowptr = (unsigned char *)map + row * stride;
        for (unsigned int col = 0; col < WIDTH; col++) {
            rowptr[col * 4 + 0] = FILL_R;
            rowptr[col * 4 + 1] = FILL_G;
            rowptr[col * 4 + 2] = FILL_B;
            rowptr[col * 4 + 3] = FILL_A;
        }
    }
    munmap(map, dumb_size);

    printf("Test buffer: %dx%d RGBA (solid %d,%d,%d,%d), stride=%u, dma-buf fd=%d, size=%lu\n",
           WIDTH, HEIGHT, FILL_R, FILL_G, FILL_B, FILL_A, stride, dmabuf_fd,
           (unsigned long)dumb_size);

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
        fprintf(stderr, "Is the daemon running (listening on %s)?\n", VAAPI_DAEMON_SOCKET_PATH);
        return 1;
    }

    EncodeRequest req = {
        .width = WIDTH,
        .height = HEIGHT,
        .stride_y = stride,
        .stride_uv = 0,
        .offset_uv = 0,
        .dmabuf_size = (uint32_t)dumb_size,
        .drm_format_modifier = 0, /* DRM_FORMAT_MOD_LINEAR -- a dumb buffer always is */
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
    printf("\n*** Received %u bytes of H.264 from the daemon, wrote %s ***\n",
           resp.coded_size, OUTPUT_FILE);
    printf("Verify with: ffmpeg -i %s -vframes 1 -y rgba-test-out.png\n", OUTPUT_FILE);

    free(coded);
    close(sock_fd);
    close(dmabuf_fd);
    return 0;
}
