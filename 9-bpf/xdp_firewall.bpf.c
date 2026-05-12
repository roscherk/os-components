#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/in.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

char LICENSE[] SEC("license") = "GPL";

#define MAX_PORTS 65536

struct
{
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, MAX_PORTS);
    __type(key, __u32);
    __type(value, __u8);
} blocked_ports SEC(".maps");

static __always_inline int port_is_blocked(__u16 port)
{
    __u32 key = port;
    __u8 *val = bpf_map_lookup_elem(&blocked_ports, &key);
    return val && *val;
}

SEC("xdp")
int xdp_firewall(struct xdp_md *ctx)
{
    void *data = (void *)(long)ctx->data;
    void *data_end = (void *)(long)ctx->data_end;

    struct ethhdr *eth = data;
    if ((void *)(eth + 1) > data_end)
        return XDP_PASS;

    __u16 h_proto = eth->h_proto;
    __u8 l4_proto;
    void *l4;

    if (h_proto == bpf_htons(ETH_P_IP))
    {
        struct iphdr *iph = (void *)(eth + 1);
        if ((void *)(iph + 1) > data_end)
            return XDP_PASS;

        l4_proto = iph->protocol;
        l4 = (void *)iph + (iph->ihl * 4);
    }
    else if (h_proto == bpf_htons(ETH_P_IPV6))
    {
        struct ipv6hdr *ip6h = (void *)(eth + 1);
        if ((void *)(ip6h + 1) > data_end)
            return XDP_PASS;

        l4_proto = ip6h->nexthdr;
        l4 = ip6h + 1;
    }
    else
    {
        return XDP_PASS;
    }

    __u16 dport;

    if (l4_proto == IPPROTO_TCP)
    {
        struct tcphdr *tcph = l4;
        if ((void *)(tcph + 1) > data_end)
            return XDP_PASS;
        dport = bpf_ntohs(tcph->dest);
    }
    else if (l4_proto == IPPROTO_UDP)
    {
        struct udphdr *udph = l4;
        if ((void *)(udph + 1) > data_end)
            return XDP_PASS;
        dport = bpf_ntohs(udph->dest);
    }
    else
    {
        return XDP_PASS;
    }

    if (port_is_blocked(dport))
    {
        bpf_printk("xdp_firewall: DROP proto=%u dport=%u\n", l4_proto, dport);
        return XDP_DROP;
    }

    return XDP_PASS;
}
