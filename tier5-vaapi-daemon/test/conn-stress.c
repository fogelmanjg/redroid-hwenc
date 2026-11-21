/*
 * Connection-isolation checks for the daemon (21/11/2026), no GPU work involved:
 *
 *   conn-stress hold  <socket> <n> <seconds>   open n connections, send nothing, keep them open
 *   conn-stress flood <socket> <rps> <seconds> one process, <rps> connections per second, each
 *                                              sends nothing and closes right away
 *
 * "hold" plays a client that connected and stalled: before the isolation work it blocked encode
 * for every other client until it went away; now each connection has its own thread and a
 * timeout, so a real encode (rgba-test-client) must keep working meanwhile, and the daemon must
 * drop these after REDROID_FORGE_DAEMON_IO_TIMEOUT_MS.
 * "flood" is one PID over the rate limit: the daemon must log "rate-limit: pid N ... blocked".
 * It prints how many connections were accepted vs. closed on arrival.
 *
 * Build: cc -O2 -o conn-stress test/conn-stress.c
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>

static int connect_to(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = {0};
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        if (fd >= 0) close(fd);
        return -1;
    }
    return fd;
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* 1 if the daemon closed it (read returns 0/-1 within wait_ms), 0 if still open. */
static int closed_by_peer(int fd, int wait_ms) {
    struct pollfd p = {.fd = fd, .events = POLLIN};
    if (poll(&p, 1, wait_ms) <= 0) return 0;
    char c;
    return read(fd, &c, 1) <= 0;
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: %s hold|flood <socket> <n|rps> <seconds>\n", argv[0]);
        return 2;
    }
    const char *mode = argv[1], *path = argv[2];
    const int n = atoi(argv[3]);
    const double secs = atof(argv[4]);

    if (strcmp(mode, "hold") == 0) {
        int *fds = calloc(n, sizeof(int));
        int open_ok = 0;
        for (int i = 0; i < n; i++) if ((fds[i] = connect_to(path)) >= 0) open_ok++;
        printf("hold: %d/%d connected, holding up to %.1f s\n", open_ok, n, secs);
        fflush(stdout);
        const double t0 = now_s();
        int dropped = 0;
        while (now_s() - t0 < secs && dropped < open_ok) {
            for (int i = 0; i < n; i++) {
                if (fds[i] >= 0 && closed_by_peer(fds[i], 0)) {
                    printf("hold: connection %d closed by the daemon after %.1f s\n", i, now_s() - t0);
                    close(fds[i]);
                    fds[i] = -1;
                    dropped++;
                }
            }
            usleep(100000);
        }
        printf("hold: %d/%d closed by the daemon\n", dropped, open_ok);
        return 0;
    }

    if (strcmp(mode, "flood") == 0) {
        const double t0 = now_s();
        long sent = 0, refused_on_arrival = 0, connect_failed = 0;
        while (now_s() - t0 < secs) {
            const double due = t0 + (double)sent / n;
            const double wait = due - now_s();
            if (wait > 0) usleep((useconds_t)(wait * 1e6));
            int fd = connect_to(path);
            sent++;
            if (fd < 0) { connect_failed++; continue; }
            /* A connection the daemon dropped at accept() is already closed when we look (no waiting here: waiting would
             * cap the flood rate itself). The daemon's own "rate-limit: ... blocked" log line is the real signal. */
            if (closed_by_peer(fd, 0)) refused_on_arrival++;
            close(fd);
        }
        printf("flood: %ld connections in %.1f s, %ld closed on arrival, %ld failed to connect\n", sent, now_s() - t0,
               refused_on_arrival, connect_failed);
        return 0;
    }
    fprintf(stderr, "unknown mode %s\n", mode);
    return 2;
}
