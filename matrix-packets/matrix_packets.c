/*
 * Matrix Packet Visualizer
 *
 * Displays network packet bytes as falling Matrix-style streams
 * rendered directly on the Wayland desktop background layer.
 *
 * Requires: libpcap, wayland-client, wlr-layer-shell, cairo, pango
 * Run as root or with CAP_NET_RAW capability.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <poll.h>
#include <pthread.h>
#include <grp.h>

#include "capture.h"
#include "streams.h"
#include "render_wayland.h"

#define FRAME_DELAY_US 100000  /* 100ms = 10 FPS */

/* Globals */
volatile sig_atomic_t running = 1;

static void signal_handler(int sig) {
    (void)sig;
    running = 0;
}

int main(int argc, char *argv[]) {
    char errbuf[PCAP_ERRBUF_SIZE];
    pthread_t capture_tid;

    /* Handle arguments */
    if (argc > 1) {
        net_interface = strdup(argv[1]);
        if (!net_interface) { perror("strdup"); return 1; }
    } else {
        net_interface = detect_interface();
    }

    printf("Matrix Packet Visualizer (Wayland)\n");
    printf("Using interface: %s\n", net_interface);

    /* Get local IP addresses */
    get_local_ips(net_interface);
    printf("Detected %d local IP(s)\n", local_ip_count);
    printf("Starting capture (requires root)...\n");

    /* Set up signal handlers */
    struct sigaction sa = { .sa_handler = signal_handler, .sa_flags = 0 };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Ignore SIGPIPE (Wayland socket can trigger it) */
    signal(SIGPIPE, SIG_IGN);

    /* Initialize ring buffer */
    ring_buffer_init(&ring_buffer);

    /* Open pcap handle */
    pcap_handle = pcap_open_live(net_interface, MAX_PACKET_SIZE, 0, PCAP_TIMEOUT_MS, errbuf);
    if (!pcap_handle) {
        fprintf(stderr, "pcap_open_live failed: %s\n", errbuf);
        fprintf(stderr, "Are you running as root or with CAP_NET_RAW?\n");
        return 1;
    }

    /* Apply BPF filter */
    struct bpf_program fp;
    if (pcap_compile(pcap_handle, &fp, "ip", 1, PCAP_NETMASK_UNKNOWN) == 0) {
        pcap_setfilter(pcap_handle, &fp);
        pcap_freecode(&fp);
    }

    /* Set non-blocking mode */
    if (pcap_setnonblock(pcap_handle, 1, errbuf) < 0) {
        fprintf(stderr, "Warning: could not set non-blocking mode: %s\n", errbuf);
    }

    /* Drop root privileges after pcap is set up */
    if (geteuid() == 0) {
        uid_t real_uid = getuid();
        gid_t real_gid = getgid();
        if (real_uid != 0) {
            if (setgroups(0, NULL) != 0) {
                perror("setgroups");
                pcap_close(pcap_handle);
                return 1;
            }
            if (setgid(real_gid) != 0) {
                perror("setgid");
                pcap_close(pcap_handle);
                return 1;
            }
            if (setuid(real_uid) != 0) {
                perror("setuid");
                pcap_close(pcap_handle);
                return 1;
            }
            printf("Dropped privileges to uid=%d gid=%d\n", real_uid, real_gid);
        }
    }

    /* Start capture thread */
    if (pthread_create(&capture_tid, NULL, capture_thread, NULL) != 0) {
        perror("pthread_create");
        pcap_close(pcap_handle);
        return 1;
    }

    /* Initialize Wayland surface on background layer */
    if (wayland_init() != 0) {
        fprintf(stderr, "Failed to initialize Wayland surface\n");
        running = 0;
        pthread_join(capture_tid, NULL);
        pcap_close(pcap_handle);
        return 1;
    }

    int width_cells  = wayland_get_width_cells();
    int height_cells = wayland_get_height_cells();
    printf("Surface: %d x %d cells\n", width_cells, height_cells);

    srand(time(NULL));
    init_streams(width_cells);

    unsigned long frame_count = 0;

    /* Main loop: poll on Wayland fd, clock-gated frame updates */
    int wl_fd = wayland_get_fd();
    struct timespec next_frame;
    clock_gettime(CLOCK_MONOTONIC, &next_frame);

    while (running) {
        /* Compute ms until next frame is due */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long wait_ms = (next_frame.tv_sec - now.tv_sec) * 1000
                     + (next_frame.tv_nsec - now.tv_nsec) / 1000000;
        if (wait_ms < 0) wait_ms = 0;

        struct pollfd pfd = { .fd = wl_fd, .events = POLLIN };
        poll(&pfd, 1, (int)wait_ms);

        /* Always dispatch Wayland events promptly */
        wayland_dispatch();

        /* Handle reconfigure (output resize) */
        if (wayland_check_reconfigure()) {
            width_cells  = wayland_get_width_cells();
            height_cells = wayland_get_height_cells();
            resize_streams(width_cells, height_cells);
        }

        /* Only tick streams and render at the target frame rate */
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > next_frame.tv_sec ||
            (now.tv_sec == next_frame.tv_sec && now.tv_nsec >= next_frame.tv_nsec)) {

            /* Advance next deadline */
            next_frame.tv_nsec += FRAME_DELAY_US * 1000L;
            if (next_frame.tv_nsec >= 1000000000L) {
                next_frame.tv_sec  += next_frame.tv_nsec / 1000000000L;
                next_frame.tv_nsec %= 1000000000L;
            }

            /* Clamp: if we fell behind, don't try to catch up */
            if (now.tv_sec > next_frame.tv_sec ||
                (now.tv_sec == next_frame.tv_sec && now.tv_nsec > next_frame.tv_nsec)) {
                next_frame = now;
            }

            update_streams(height_cells, frame_count);

            if (streams_have_content()) {
                if (render_frame_wayland(frame_count) < 0) {
                    break;
                }
            }

            frame_count++;
        }
    }

    /* Cleanup */
    running = 0;
    pthread_join(capture_tid, NULL);

    wayland_cleanup();
    pcap_close(pcap_handle);
    free(net_interface);

    printf("\nCaptured %lu packets\n", packets_captured);

    return 0;
}
