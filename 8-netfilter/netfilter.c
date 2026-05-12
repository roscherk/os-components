#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/ip.h>
#include <linux/tcp.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Prokhor Arkhipov");
MODULE_DESCRIPTION("netfilter - log outgoing TCP connections, optionally drop by port");

static unsigned short filter_port = 0;
module_param(filter_port, ushort, 0644);
MODULE_PARM_DESC(filter_port, "Drop outgoing TCP packets to this destination port (0 = disabled)");

static unsigned int netfilter_hook_fn(void *priv,
				      struct sk_buff *skb,
				      const struct nf_hook_state *state)
{
	struct iphdr *iph;
	struct tcphdr *th;

	if (!skb)
		return NF_ACCEPT;

	if (!pskb_may_pull(skb, sizeof(struct iphdr)))
		return NF_ACCEPT;

	iph = ip_hdr(skb);
	if (iph->ihl < 5)
		return NF_ACCEPT;

	if (iph->protocol != IPPROTO_TCP)
		return NF_ACCEPT;

	if (!pskb_may_pull(skb, iph->ihl * 4 + sizeof(struct tcphdr)))
		return NF_ACCEPT;

	iph = ip_hdr(skb);
	th = (struct tcphdr *)((u8 *)iph + iph->ihl * 4);

	if (filter_port != 0 && ntohs(th->dest) == filter_port) {
		pr_info("netfilter: DROPPING packet to port %u\n", filter_port);
		return NF_DROP;
	}

	if (th->syn && !th->ack) {
		pr_info("netfilter: OUT TCP %pI4:%u -> %pI4:%u\n",
			&iph->saddr, ntohs(th->source),
			&iph->daddr, ntohs(th->dest));
	}

	return NF_ACCEPT;
}

static struct nf_hook_ops netfilter_ops = {
	.hook     = netfilter_hook_fn,
	.pf       = NFPROTO_IPV4,
	.hooknum  = NF_INET_LOCAL_OUT,
	.priority = NF_IP_PRI_FIRST,
};

static int __init nfmon_init(void)
{
	int ret;

	ret = nf_register_net_hook(&init_net, &netfilter_ops);
	if (ret) {
		pr_err("netfilter: failed to register hook: %d\n", ret);
		return ret;
	}

	pr_info("netfilter: loaded, filter_port=%u\n", filter_port);
	return 0;
}

static void __exit nfmon_exit(void)
{
	nf_unregister_net_hook(&init_net, &netfilter_ops);
	pr_info("netfilter: unloaded\n");
}

module_init(nfmon_init);
module_exit(nfmon_exit);
