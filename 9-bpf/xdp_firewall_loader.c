#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <net/if.h>
#include <linux/if_link.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>

static volatile sig_atomic_t exiting = 0;

static void handle_sigint(int sig)
{
    (void)sig;
    exiting = 1;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s <ifname> <port> [<port> ...]\n"
        "  ifname  network interface to attach XDP to (e.g. lo, ens3)\n"
        "  port    one or more TCP/UDP destination ports to drop (1..65535)\n",
        prog);
}

static int populate_blocked_ports(struct bpf_map *map, int argc, char **argv)
{
    int map_fd = bpf_map__fd(map);
    if (map_fd < 0)
    {
        fprintf(stderr, "bpf_map__fd: %s\n", strerror(-map_fd));
        return -1;
    }

    for (int i = 0; i < argc; i++)
    {
        char *end = NULL;
        long port = strtol(argv[i], &end, 10);

        if (!end || *end != '\0' || port <= 0 || port > 65535)
        {
            fprintf(stderr, "invalid port: %s\n", argv[i]);
            return -1;
        }

        __u32 key = (__u32)port;
        __u8 val = 1;

        if (bpf_map_update_elem(map_fd, &key, &val, BPF_ANY))
        {
            fprintf(stderr, "bpf_map_update_elem(port=%ld): %s\n",
                    port, strerror(errno));
            return -1;
        }

        printf("blocking port %ld\n", port);
    }

    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    const char *ifname = argv[1];
    int ifindex = if_nametoindex(ifname);
    if (!ifindex)
    {
        fprintf(stderr, "if_nametoindex(%s): %s\n", ifname, strerror(errno));
        return EXIT_FAILURE;
    }

    struct sigaction sa = { .sa_handler = handle_sigint };
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) || sigaction(SIGTERM, &sa, NULL))
    {
        perror("sigaction");
        return EXIT_FAILURE;
    }

    struct bpf_object *obj = bpf_object__open_file("xdp_firewall.bpf.o", NULL);
    if (!obj)
    {
        perror("bpf_object__open_file");
        return EXIT_FAILURE;
    }

    if (bpf_object__load(obj))
    {
        perror("bpf_object__load");
        bpf_object__close(obj);
        return EXIT_FAILURE;
    }

    struct bpf_program *prog =
        bpf_object__find_program_by_name(obj, "xdp_firewall");
    if (!prog)
    {
        fprintf(stderr, "program 'xdp_firewall' not found\n");
        bpf_object__close(obj);
        return EXIT_FAILURE;
    }

    struct bpf_map *map = bpf_object__find_map_by_name(obj, "blocked_ports");
    if (!map)
    {
        fprintf(stderr, "map 'blocked_ports' not found\n");
        bpf_object__close(obj);
        return EXIT_FAILURE;
    }

    if (populate_blocked_ports(map, argc - 2, argv + 2))
    {
        bpf_object__close(obj);
        return EXIT_FAILURE;
    }

    int prog_fd = bpf_program__fd(prog);
    __u32 flags = XDP_FLAGS_UPDATE_IF_NOEXIST | XDP_FLAGS_SKB_MODE;

    if (bpf_xdp_attach(ifindex, prog_fd, flags, NULL))
    {
        fprintf(stderr, "bpf_xdp_attach(%s): %s\n", ifname, strerror(errno));
        bpf_object__close(obj);
        return EXIT_FAILURE;
    }

    printf("XDP firewall attached to %s (ifindex=%d)\n",
           ifname, ifindex);

    while (!exiting)
        pause();

    if (bpf_xdp_detach(ifindex, flags, NULL))
        fprintf(stderr, "bpf_xdp_detach: %s\n", strerror(errno));
    else
        printf("XDP firewall detached from %s\n", ifname);

    bpf_object__close(obj);
    return EXIT_SUCCESS;
}
