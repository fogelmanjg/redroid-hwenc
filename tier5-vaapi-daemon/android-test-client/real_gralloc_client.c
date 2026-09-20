/*
 * Tier 5.4: send a REAL Android gralloc dma-buf (obtained the correct way,
 * via AHardwareBuffer_allocate -- not scavenged from another process's fd
 * table like the Tier 3 real-gralloc spike had to do before this AOSP
 * tree was available) to the host-side encode daemon and confirm it
 * encodes correctly.
 *
 * Real finding from real_gralloc_probe.c, kept here rather than fixed
 * silently: requesting AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420 (flexible
 * YUV420) on this build's Mapper HAL returns a buffer sized like RGBA8888
 * (307200 bytes for 320x240, i.e. width*height*4) instead of NV12's
 * width*height*1.5 (115200) -- the same "this generic graphics stack
 * doesn't really support YUV allocation" pattern the very first Tier 3
 * spike hit with Mesa's GBM (only RGB(A) formats allocatable). Also,
 * AHardwareBuffer_lockPlanes() isn't implemented on this Mapper version
 * (returns -38/ENOSYS) -- worked around by mmap()-ing the dma-buf fd
 * directly instead, same as every other tier's dma-buf handling.
 *
 * None of that blocks this checkpoint's actual goal: the buffer is still
 * large enough, and it's still a genuine dma-buf that Android's own
 * gralloc allocated via the correct stable API. We just fill it as NV12
 * ourselves and send that -- proving a real Android-allocated dma-buf
 * survives the IPC bridge and encodes correctly, which is what Tier 5.4
 * is actually checking.
 *
 * One more real finding along the way: importing with the tightly-packed
 * stride AHardwareBuffer_describe() reported (320, no padding) failed with
 * "resource allocation failed" -- and isolating the variable (same failure
 * on the *known-good* synthetic DRM dumb buffer from tier5-vaapi-daemon,
 * just by lying about its stride) proved this is a VA-API/radeonsi pitch
 * *alignment* requirement, not anything about this dma-buf's origin or
 * allocator. Since we're the ones filling this buffer for the test (a real
 * production encoder's producer wouldn't have this freedom), the fix here
 * is to just fill and describe it using an aligned stride (512, matching
 * what already works) instead of the tight one -- the buffer has more than
 * enough real bytes (307200) to fit that padded layout (184320 needed).
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

#include <android/hardware_buffer.h>
#include <vndk/hardware_buffer.h>

#include "protocol.h"

#define WIDTH 320
#define HEIGHT 240
#define FILL_VALUE 128
#define OUTPUT_FILE "out.h264"

int main(void) {
    AHardwareBuffer_Desc desc = {0};
    desc.width = WIDTH;
    desc.height = HEIGHT;
    desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420;
    desc.usage = AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_VIDEO_ENCODE;

    AHardwareBuffer *buf = NULL;
    if (AHardwareBuffer_allocate(&desc, &buf) != 0) {
        fprintf(stderr, "AHardwareBuffer_allocate failed\n");
        return 1;
    }

    const native_handle_t *handle = AHardwareBuffer_getNativeHandle(buf);
    if (!handle || handle->numFds < 1) {
        fprintf(stderr, "no usable dma-buf fd in the native handle\n");
        return 1;
    }
    int dmabuf_fd = handle->data[0];

    off_t real_size = lseek(dmabuf_fd, 0, SEEK_END);
    lseek(dmabuf_fd, 0, SEEK_SET);
    printf("Real Android gralloc dma-buf: fd=%d, size=%ld bytes (requested %dx%d flexible YUV420)\n",
           dmabuf_fd, (long)real_size, WIDTH, HEIGHT);

    uint32_t stride = 512; /* aligned stride VA-API/radeonsi requires (see file header comment) --
                             * NOT what AHardwareBuffer_describe() reported (320, unpadded); we
                             * choose this layout ourselves since we're the ones filling the buffer */
    uint32_t offset_uv = stride * HEIGHT;
    size_t needed = (size_t)offset_uv + (stride * HEIGHT / 2);
    if (real_size < (off_t)needed) {
        fprintf(stderr, "buffer too small for even a tightly-packed NV12 frame (%zu needed)\n", needed);
        return 1;
    }

    void *map = mmap(NULL, real_size, PROT_READ | PROT_WRITE, MAP_SHARED, dmabuf_fd, 0);
    if (map == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    for (unsigned int row = 0; row < HEIGHT; row++)
        memset((unsigned char *)map + row * stride, FILL_VALUE, WIDTH);
    for (unsigned int row = 0; row < HEIGHT / 2; row++)
        memset((unsigned char *)map + offset_uv + row * stride, FILL_VALUE, WIDTH);
    munmap(map, real_size);
    printf("Filled the first %zu bytes as NV12 via plain mmap() (no AHardwareBuffer lock API involved)\n", needed);

    int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
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
        .stride_uv = stride,
        .offset_uv = offset_uv,
        .dmabuf_size = (uint32_t)real_size,
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
    printf("Sent EncodeRequest + real gralloc dma-buf fd to daemon, waiting for response...\n");

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
        if (n <= 0) { perror("read(body)"); return 1; }
        got += n;
    }
    FILE *out = fopen(OUTPUT_FILE, "wb");
    fwrite(coded, 1, resp.coded_size, out);
    fclose(out);
    printf("\n*** Received %u bytes of H.264 from the daemon, encoded from a REAL Android "
           "gralloc dma-buf. Wrote %s ***\n", resp.coded_size, OUTPUT_FILE);

    free(coded);
    close(sock_fd);
    AHardwareBuffer_release(buf);
    return 0;
}
