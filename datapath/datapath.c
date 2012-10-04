/*
 * Copyright (c) 2007-2013 Nicira, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/module.h>
#include <linux/if_arp.h>
#include <linux/if_vlan.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/jhash.h>
#include <linux/delay.h>
#include <linux/time.h>
#include <linux/etherdevice.h>
#include <linux/genetlink.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/percpu.h>
#include <linux/rcupdate.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/version.h>
#include <linux/ethtool.h>
#include <linux/wait.h>
#include <asm/div64.h>
#include <linux/highmem.h>
#include <linux/netfilter_bridge.h>
#include <linux/netfilter_ipv4.h>
#include <linux/inetdevice.h>
#include <linux/list.h>
#include <linux/openvswitch.h>
#include <linux/rculist.h>
#include <linux/dmi.h>
#include <linux/genetlink.h>
#include <net/genetlink.h>
#include <net/genetlink.h>
#include <net/net_namespace.h>
#include <net/netns/generic.h>

#include "datapath.h"
#include "flow.h"
#include "gso.h"
#include "mpls.h"
#include "vlan.h"
#include "vport-internal_dev.h"
#include "vport-netdev.h"

#define REHASH_FLOW_INTERVAL (10 * 60 * HZ)

int ovs_net_id __read_mostly;

static void ovs_notify(struct sk_buff *skb, struct genl_info *info,
		       struct genl_multicast_group *grp)
{
	genl_notify(skb, genl_info_net(info), info->snd_portid,
		    grp->id, info->nlhdr, GFP_KERNEL);
}

/**
 * DOC: Locking:
 *
 * All writes e.g. Writes to device state (add/remove datapath, port, set
 * operations on vports, etc.), Writes to other state (flow table
 * modifications, set miscellaneous datapath parameters, etc.) are protected
 * by ovs_lock.
 *
 * Reads are protected by RCU.
 *
 * There are a few special cases (mostly stats) that have their own
 * synchronization but they nest under all of above and don't interact with
 * each other.
 *
 * The RTNL lock nests inside ovs_mutex.
 */

static DEFINE_MUTEX(ovs_mutex);

void ovs_lock(void)
{
	mutex_lock(&ovs_mutex);
}

void ovs_unlock(void)
{
	mutex_unlock(&ovs_mutex);
}

#ifdef CONFIG_LOCKDEP
int lockdep_ovsl_is_held(void)
{
	if (debug_locks)
		return lockdep_is_held(&ovs_mutex);
	else
		return 1;
}
#endif

static struct vport *new_vport(const struct vport_parms *);
static int queue_gso_packets(struct net *, int dp_ifindex, struct sk_buff *,
			     const struct dp_upcall_info *);
static int queue_userspace_packet(struct net *, int dp_ifindex,
				  struct sk_buff *,
				  const struct dp_upcall_info *);

/* Must be called with rcu_read_lock or ovs_mutex. */
static struct datapath *get_dp(struct net *net, int dp_ifindex)
{
	struct datapath *dp = NULL;
	struct net_device *dev;

	rcu_read_lock();
	dev = dev_get_by_index_rcu(net, dp_ifindex);
	if (dev) {
		struct vport *vport = ovs_internal_dev_get_vport(dev);
		if (vport)
			dp = vport->dp;
	}
	rcu_read_unlock();

	return dp;
}

/* Must be called with rcu_read_lock or ovs_mutex. */
const char *ovs_dp_name(const struct datapath *dp)
{
	struct vport *vport = ovs_vport_ovsl_rcu(dp, OVSP_LOCAL);
	return vport->ops->get_name(vport);
}

static int get_dpifindex(struct datapath *dp)
{
	struct vport *local;
	int ifindex;

	rcu_read_lock();

	local = ovs_vport_rcu(dp, OVSP_LOCAL);
	if (local)
		ifindex = netdev_vport_priv(local)->dev->ifindex;
	else
		ifindex = 0;

	rcu_read_unlock();

	return ifindex;
}

static void destroy_dp_rcu(struct rcu_head *rcu)
{
	struct datapath *dp = container_of(rcu, struct datapath, rcu);

	ovs_flow_tbl_destroy((__force struct flow_table *)dp->table, false);
	free_percpu(dp->stats_percpu);
	release_net(ovs_dp_get_net(dp));
	kfree(dp->ports);
	kfree(dp);
}

static struct hlist_head *vport_hash_bucket(const struct datapath *dp,
					    u16 port_no)
{
	return &dp->ports[port_no & (DP_VPORT_HASH_BUCKETS - 1)];
}

struct vport *ovs_lookup_vport(const struct datapath *dp, u16 port_no)
{
	struct vport *vport;
	struct hlist_head *head;

	head = vport_hash_bucket(dp, port_no);
	hlist_for_each_entry_rcu(vport, head, dp_hash_node) {
		if (vport->port_no == port_no)
			return vport;
	}
	return NULL;
}

/* Called with ovs_mutex. */
static struct vport *new_vport(const struct vport_parms *parms)
{
	struct vport *vport;

	vport = ovs_vport_add(parms);
	if (!IS_ERR(vport)) {
		struct datapath *dp = parms->dp;
		struct hlist_head *head = vport_hash_bucket(dp, vport->port_no);

		hlist_add_head_rcu(&vport->dp_hash_node, head);
	}
	return vport;
}

void ovs_dp_detach_port(struct vport *p)
{
	ASSERT_OVSL();

	/* First drop references to device. */
	hlist_del_rcu(&p->dp_hash_node);

	/* Then destroy it. */
	ovs_vport_del(p);
}

/* Must be called with rcu_read_lock. */
void ovs_dp_process_received_packet(struct vport *p, struct sk_buff *skb)
{
	struct datapath *dp = p->dp;
	struct sw_flow *flow;
	struct dp_stats_percpu *stats;
	struct sw_flow_key key;
	u64 *stats_counter;
	int error;

	stats = this_cpu_ptr(dp->stats_percpu);

	/* Extract flow from 'skb' into 'key'. */
	error = ovs_flow_extract(skb, p->port_no, &key);
	if (unlikely(error)) {
		kfree_skb(skb);
		return;
	}

	/* Look up flow. */
	flow = ovs_flow_lookup(rcu_dereference(dp->table), &key);
	if (unlikely(!flow)) {
		struct dp_upcall_info upcall;

		upcall.cmd = OVS_PACKET_CMD_MISS;
		upcall.key = &key;
		upcall.userdata = NULL;
		upcall.portid = p->upcall_portid;
		ovs_dp_upcall(dp, skb, &upcall);
		consume_skb(skb);
		stats_counter = &stats->n_missed;
		goto out;
	}

	OVS_CB(skb)->flow = flow;
	OVS_CB(skb)->pkt_key = &key;

	stats_counter = &stats->n_hit;
	ovs_flow_used(OVS_CB(skb)->flow, skb);
	ovs_execute_actions(dp, skb);

out:
	/* Update datapath statistics. */
	u64_stats_update_begin(&stats->sync);
	(*stats_counter)++;
	u64_stats_update_end(&stats->sync);
}

static struct genl_family dp_packet_genl_family = {
	.id = GENL_ID_GENERATE,
	.hdrsize = sizeof(struct ovs_header),
	.name = OVS_PACKET_FAMILY,
	.version = OVS_PACKET_VERSION,
	.maxattr = OVS_PACKET_ATTR_MAX,
	.netnsok = true,
	 SET_PARALLEL_OPS
};

int ovs_dp_upcall(struct datapath *dp, struct sk_buff *skb,
		  const struct dp_upcall_info *upcall_info)
{
	struct dp_stats_percpu *stats;
	int dp_ifindex;
	int err;

	if (upcall_info->portid == 0) {
		err = -ENOTCONN;
		goto err;
	}

	dp_ifindex = get_dpifindex(dp);
	if (!dp_ifindex) {
		err = -ENODEV;
		goto err;
	}

	if (!skb_is_gso(skb))
		err = queue_userspace_packet(ovs_dp_get_net(dp), dp_ifindex, skb, upcall_info);
	else
		err = queue_gso_packets(ovs_dp_get_net(dp), dp_ifindex, skb, upcall_info);
	if (err)
		goto err;

	return 0;

err:
	stats = this_cpu_ptr(dp->stats_percpu);

	u64_stats_update_begin(&stats->sync);
	stats->n_lost++;
	u64_stats_update_end(&stats->sync);

	return err;
}

static int queue_gso_packets(struct net *net, int dp_ifindex,
			     struct sk_buff *skb,
			     const struct dp_upcall_info *upcall_info)
{
	unsigned short gso_type = skb_shinfo(skb)->gso_type;
	struct dp_upcall_info later_info;
	struct sw_flow_key later_key;
	struct sk_buff *segs, *nskb;
	int err;

	segs = __skb_gso_segment(skb, NETIF_F_SG | NETIF_F_HW_CSUM, false);
	if (IS_ERR(segs))
		return PTR_ERR(segs);

	/* Queue all of the segments. */
	skb = segs;
	do {
		err = queue_userspace_packet(net, dp_ifindex, skb, upcall_info);
		if (err)
			break;

		if (skb == segs && gso_type & SKB_GSO_UDP) {
			/* The initial flow key extracted by ovs_flow_extract()
			 * in this case is for a first fragment, so we need to
			 * properly mark later fragments.
			 */
			later_key = *upcall_info->key;
			later_key.ip.frag = OVS_FRAG_TYPE_LATER;

			later_info = *upcall_info;
			later_info.key = &later_key;
			upcall_info = &later_info;
		}
	} while ((skb = skb->next));

	/* Free all of the segments. */
	skb = segs;
	do {
		nskb = skb->next;
		if (err)
			kfree_skb(skb);
		else
			consume_skb(skb);
	} while ((skb = nskb));
	return err;
}

static size_t key_attr_size(void)
{
	return    nla_total_size(4)   /* OVS_KEY_ATTR_PRIORITY */
		+ nla_total_size(0)   /* OVS_KEY_ATTR_TUNNEL */
		  + nla_total_size(8)   /* OVS_TUNNEL_KEY_ATTR_ID */
		  + nla_total_size(4)   /* OVS_TUNNEL_KEY_ATTR_IPV4_SRC */
		  + nla_total_size(4)   /* OVS_TUNNEL_KEY_ATTR_IPV4_DST */
		  + nla_total_size(1)   /* OVS_TUNNEL_KEY_ATTR_TOS */
		  + nla_total_size(1)   /* OVS_TUNNEL_KEY_ATTR_TTL */
		  + nla_total_size(0)   /* OVS_TUNNEL_KEY_ATTR_DONT_FRAGMENT */
		  + nla_total_size(0)   /* OVS_TUNNEL_KEY_ATTR_CSUM */
		+ nla_total_size(4)   /* OVS_KEY_ATTR_IN_PORT */
		+ nla_total_size(4)   /* OVS_KEY_ATTR_SKB_MARK */
		+ nla_total_size(12)  /* OVS_KEY_ATTR_ETHERNET */
		+ nla_total_size(2)   /* OVS_KEY_ATTR_ETHERTYPE */
		+ nla_total_size(4)   /* OVS_KEY_ATTR_8021Q */
		+ nla_total_size(0)   /* OVS_KEY_ATTR_ENCAP */
		+ nla_total_size(2)   /* OVS_KEY_ATTR_ETHERTYPE */
		+ nla_total_size(40)  /* OVS_KEY_ATTR_IPV6 */
		+ nla_total_size(2)   /* OVS_KEY_ATTR_ICMPV6 */
		+ nla_total_size(28); /* OVS_KEY_ATTR_ND */
}

static size_t upcall_msg_size(const struct sk_buff *skb,
			      const struct nlattr *userdata)
{
	size_t size = NLMSG_ALIGN(sizeof(struct ovs_header))
		+ nla_total_size(skb->len) /* OVS_PACKET_ATTR_PACKET */
		+ nla_total_size(key_attr_size()); /* OVS_PACKET_ATTR_KEY */

	/* OVS_PACKET_ATTR_USERDATA */
	if (userdata)
		size += NLA_ALIGN(userdata->nla_len);

	return size;
}

static int queue_userspace_packet(struct net *net, int dp_ifindex,
				  struct sk_buff *skb,
				  const struct dp_upcall_info *upcall_info)
{
	struct ovs_header *upcall;
	struct sk_buff *nskb = NULL;
	struct sk_buff *user_skb; /* to be queued to userspace */
	struct nlattr *nla;
	int err;

	if (vlan_tx_tag_present(skb)) {
		nskb = skb_clone(skb, GFP_ATOMIC);
		if (!nskb)
			return -ENOMEM;

		nskb = __vlan_put_tag(nskb, nskb->vlan_proto, vlan_tx_tag_get(nskb));
		if (!nskb)
			return -ENOMEM;

		vlan_set_tci(nskb, 0);

		skb = nskb;
	}

	if (nla_attr_size(skb->len) > USHRT_MAX) {
		err = -EFBIG;
		goto out;
	}

	user_skb = genlmsg_new(upcall_msg_size(skb, upcall_info->userdata), GFP_ATOMIC);
	if (!user_skb) {
		err = -ENOMEM;
		goto out;
	}

	upcall = genlmsg_put(user_skb, 0, 0, &dp_packet_genl_family,
			     0, upcall_info->cmd);
	upcall->dp_ifindex = dp_ifindex;

	nla = nla_nest_start(user_skb, OVS_PACKET_ATTR_KEY);
	ovs_flow_to_nlattrs(upcall_info->key, upcall_info->key, user_skb);
	nla_nest_end(user_skb, nla);

	if (upcall_info->userdata)
		__nla_put(user_skb, OVS_PACKET_ATTR_USERDATA,
			  nla_len(upcall_info->userdata),
			  nla_data(upcall_info->userdata));

	nla = __nla_reserve(user_skb, OVS_PACKET_ATTR_PACKET, skb->len);

	skb_copy_and_csum_dev(skb, nla_data(nla));

	genlmsg_end(user_skb, upcall);
	err = genlmsg_unicast(net, user_skb, upcall_info->portid);

out:
	kfree_skb(nskb);
	return err;
}

/* Called with ovs_mutex. */
static int flush_flows(struct datapath *dp)
{
	struct flow_table *old_table;
	struct flow_table *new_table;

	old_table = ovsl_dereference(dp->table);
	new_table = ovs_flow_tbl_alloc(TBL_MIN_BUCKETS);
	if (!new_table)
		return -ENOMEM;

	rcu_assign_pointer(dp->table, new_table);

	ovs_flow_tbl_destroy(old_table, true);
	return 0;
}

static struct nlattr *reserve_sfa_size(struct sw_flow_actions **sfa, int attr_len)
{

	struct sw_flow_actions *acts;
	int new_acts_size;
	int req_size = NLA_ALIGN(attr_len);
	int next_offset = offsetof(struct sw_flow_actions, actions) +
					(*sfa)->actions_len;

	if (req_size <= (ksize(*sfa) - next_offset))
		goto out;

	new_acts_size = ksize(*sfa) * 2;

	if (new_acts_size > MAX_ACTIONS_BUFSIZE) {
		if ((MAX_ACTIONS_BUFSIZE - next_offset) < req_size)
			return ERR_PTR(-EMSGSIZE);
		new_acts_size = MAX_ACTIONS_BUFSIZE;
	}

	acts = ovs_flow_actions_alloc(new_acts_size);
	if (IS_ERR(acts))
		return (void *)acts;

	memcpy(acts->actions, (*sfa)->actions, (*sfa)->actions_len);
	acts->actions_len = (*sfa)->actions_len;
	kfree(*sfa);
	*sfa = acts;

out:
	(*sfa)->actions_len += req_size;
	return  (struct nlattr *) ((unsigned char *)(*sfa) + next_offset);
}

static int add_action(struct sw_flow_actions **sfa, int attrtype, void *data, int len)
{
	struct nlattr *a;

	a = reserve_sfa_size(sfa, nla_attr_size(len));
	if (IS_ERR(a))
		return PTR_ERR(a);

	a->nla_type = attrtype;
	a->nla_len = nla_attr_size(len);

	if (data)
		memcpy(nla_data(a), data, len);
	memset((unsigned char *) a + a->nla_len, 0, nla_padlen(len));

	return 0;
}

static inline int add_nested_action_start(struct sw_flow_actions **sfa, int attrtype)
{
	int used = (*sfa)->actions_len;
	int err;

	err = add_action(sfa, attrtype, NULL, 0);
	if (err)
		return err;

	return used;
}

static inline void add_nested_action_end(struct sw_flow_actions *sfa, int st_offset)
{
	struct nlattr *a = (struct nlattr *) ((unsigned char *)sfa->actions + st_offset);

	a->nla_len = sfa->actions_len - st_offset;
}

#define MAX_ETH_TYPES 16 /* Arbitrary Limit */

/* struct eth_types - possible eth types
 * @types: provides storage for the possible eth types.
 * @start: is the index of the first entry of types which is possible.
 * @end: is the index of the last entry of types which is possible.
 * @cursor: is the index of the entry which should be updated if an action
 * changes the eth type.
 *
 * Due to the sample action there may be multiple possible eth types.
 * In order to correctly validate actions all possible types are tracked
 * and verified. This is done using struct eth_types.
 *
 * Initially start, end and cursor should be 0, and the first element of
 * types should be set to the eth type of the flow.
 *
 * When an action changes the eth type then the values of start and end are
 * updated to the value of cursor. The new type is stored at types[cursor].
 *
 * When entering a sample action the start and cursor values are saved. The
 * value of cursor is set to the value of end plus one.
 *
 * When leaving a sample action the start and cursor values are restored to
 * their saved values.
 *
 * An example follows.
 *
 * actions: pop_mpls(A),sample(pop_mpls(B)),sample(pop_mpls(C)),pop_mpls(D)
 *
 * 0. Initial state:
 *	types = { original_eth_type }
 * 	start = end = cursor = 0;
 *
 * 1. pop_mpls(A)
 *    a. Check types from start (0) to end (0) inclusive
 *       i.e. Check against original_eth_type
 *    b. Set start = end = cursor
 *    c. Set types[cursor] = A
 *    New state:
 *	types = { A }
 *	start = end = cursor = 0;
 *
 * 2. Enter first sample()
 *    a. Save start and cursor
 *    b. Set cursor = end + 1
 *    New state:
 *	types = { A }
 *	start = end = 0;
 *	cursor = 1;
 *
 * 3. pop_mpls(B)
 *    a. Check types from start (0) to end (0)
 *       i.e: Check against A
 *    b. Set start = end = cursor
 *    c. Set types[cursor] = B
 *    New state:
 *	types = { A, B }
 *	start = end = cursor = 1;
 *
 * 4. Leave first sample()
 *    a. Restore start and cursor to the values when entering 2.
 *    New state:
 *	types = { A, B }
 *	start = cursor = 0;
 *	end = 1;
 *
 * 5. Enter second sample()
 *    a. Save start and cursor
 *    b. Set cursor = end + 1
 *    New state:
 *	types = { A, B }
 *	start = 0;
 *	end = 1;
 *	cursor = 2;
 *
 * 6. pop_mpls(C)
 *    a. Check types from start (0) to end (1) inclusive
 *       i.e: Check against A and B
 *    b. Set start = end = cursor
 *    c. Set types[cursor] = C
 *    New state:
 *	types = { A, B, C }
 *	start = end = cursor = 2;
 *
 * 7. Leave second sample()
 *    a. Restore start and cursor to the values when entering 5.
 *    New state:
 *	types = { A, B, C }
 *	start = cursor = 0;
 *	end = 2;
 *
 * 8. pop_mpls(D)
 *    a. Check types from start (0) to end (2) inclusive
 *       i.e: Check against A, B and C
 *    b. Set start = end = cursor
 *    c. Set types[cursor] = D
 *    New state:
 *	types = { D } // Trailing entries of type are no longer used end = 0
 *	start = end = cursor = 0;
 */
struct eth_types {
	int start, end, cursor;
	__be16 types[MAX_ETH_TYPES];
};

static void eth_types_set(struct eth_types *types, __be16 type)
{
	types->start = types->end = types->cursor;
	types->types[types->cursor] = type;
}

static int validate_and_copy_actions__(const struct nlattr *attr,
				const struct sw_flow_key *key, int depth,
				struct sw_flow_actions **sfa,
				struct eth_types *eth_types);

static int validate_and_copy_sample(const struct nlattr *attr,
			   const struct sw_flow_key *key, int depth,
			   struct sw_flow_actions **sfa,
			   struct eth_types *eth_types)
{
	const struct nlattr *attrs[OVS_SAMPLE_ATTR_MAX + 1];
	const struct nlattr *probability, *actions;
	const struct nlattr *a;
	int rem, start, err, st_acts;
	int saved_eth_types_start, saved_eth_types_cursor;

	memset(attrs, 0, sizeof(attrs));
	nla_for_each_nested(a, attr, rem) {
		int type = nla_type(a);
		if (!type || type > OVS_SAMPLE_ATTR_MAX || attrs[type])
			return -EINVAL;
		attrs[type] = a;
	}
	if (rem)
		return -EINVAL;

	probability = attrs[OVS_SAMPLE_ATTR_PROBABILITY];
	if (!probability || nla_len(probability) != sizeof(u32))
		return -EINVAL;

	actions = attrs[OVS_SAMPLE_ATTR_ACTIONS];
	if (!actions || (nla_len(actions) && nla_len(actions) < NLA_HDRLEN))
		return -EINVAL;

	/* validation done, copy sample action. */
	start = add_nested_action_start(sfa, OVS_ACTION_ATTR_SAMPLE);
	if (start < 0)
		return start;
	err = add_action(sfa, OVS_SAMPLE_ATTR_PROBABILITY, nla_data(probability), sizeof(u32));
	if (err)
		return err;
	st_acts = add_nested_action_start(sfa, OVS_SAMPLE_ATTR_ACTIONS);
	if (st_acts < 0)
		return st_acts;

	/* Save and update eth_types cursor and start.  Please see the
	 * comment for struct eth_types for a discussion of this.
	 */
	saved_eth_types_start = eth_types->start;
	saved_eth_types_cursor = eth_types->cursor;
	eth_types->cursor = eth_types->end + 1;
	if (eth_types->cursor == MAX_ETH_TYPES)
		return -EINVAL;

	err = validate_and_copy_actions__(actions, key, depth + 1, sfa,
					  eth_types);
	if (err)
		return err;

	/* Restore eth_types cursor and start.  Please see the
	 * comment for struct eth_types for a discussion of this.
	 */
	eth_types->cursor = saved_eth_types_cursor;
	eth_types->start = saved_eth_types_start;

	add_nested_action_end(*sfa, st_acts);
	add_nested_action_end(*sfa, start);

	return 0;
}

static int validate_tp_port__(const struct sw_flow_key *flow_key,
			      __be16 eth_type)
{
	if (eth_type == htons(ETH_P_IP)) {
		if (flow_key->ipv4.tp.src || flow_key->ipv4.tp.dst)
			return 0;
	} else 	if (eth_type == htons(ETH_P_IPV6)) {
		if (flow_key->ipv6.tp.src || flow_key->ipv6.tp.dst)
			return 0;
	}

	return -EINVAL;
}

static int validate_tp_port(const struct sw_flow_key *flow_key,
			    const struct eth_types *eth_types)
{
	int i;

	for (i = eth_types->start; i < eth_types->end; i++) {
		int ret = validate_tp_port__(flow_key, eth_types->types[i]);

		if (ret)
			return ret;
	}

	return 0;
}

static int validate_and_copy_set_tun(const struct nlattr *attr,
				     struct sw_flow_actions **sfa)
{
	struct sw_flow_match match;
	struct sw_flow_key key;
	int err, start;

	ovs_match_init(&match, &key, NULL);
	err = ovs_ipv4_tun_from_nlattr(nla_data(attr), &match, false);
	if (err)
		return err;

	start = add_nested_action_start(sfa, OVS_ACTION_ATTR_SET);
	if (start < 0)
		return start;

	err = add_action(sfa, OVS_KEY_ATTR_IPV4_TUNNEL, &match.key->tun_key,
			sizeof(match.key->tun_key));
	add_nested_action_end(*sfa, start);

	return err;
}

static int validate_set(const struct nlattr *a,
			const struct sw_flow_key *flow_key,
			struct sw_flow_actions **sfa,
			bool *set_tun, struct eth_types *eth_types)
{
	const struct nlattr *ovs_key = nla_data(a);
	int key_type = nla_type(ovs_key);

	/* There can be only one key in a action */
	if (nla_total_size(nla_len(ovs_key)) != nla_len(a))
		return -EINVAL;

	if (key_type > OVS_KEY_ATTR_MAX ||
	    (ovs_key_lens[key_type] != nla_len(ovs_key) &&
	     ovs_key_lens[key_type] != -1))
		return -EINVAL;

	switch (key_type) {
	const struct ovs_key_ipv4 *ipv4_key;
	const struct ovs_key_ipv6 *ipv6_key;
	int err;

	case OVS_KEY_ATTR_PRIORITY:
	case OVS_KEY_ATTR_SKB_MARK:
	case OVS_KEY_ATTR_ETHERNET:
		break;

	case OVS_KEY_ATTR_TUNNEL:
		*set_tun = true;
		err = validate_and_copy_set_tun(a, sfa);
		if (err)
			return err;
		break;

	case OVS_KEY_ATTR_IPV4: {
		int i;

		for (i = eth_types->start; i <= eth_types->end; i++)
			if (eth_types->types[i] != htons(ETH_P_IP))
				return -EINVAL;

		if (!flow_key->ip.proto)
			return -EINVAL;

		ipv4_key = nla_data(ovs_key);
		if (ipv4_key->ipv4_proto != flow_key->ip.proto)
			return -EINVAL;

		if (ipv4_key->ipv4_frag != flow_key->ip.frag)
			return -EINVAL;

		break;
	}

	case OVS_KEY_ATTR_IPV6: {
		int i;

		for (i = eth_types->start; i <= eth_types->end; i++)
			if (eth_types->types[i] != htons(ETH_P_IPV6))
				return -EINVAL;

		if (!flow_key->ip.proto)
			return -EINVAL;

		ipv6_key = nla_data(ovs_key);
		if (ipv6_key->ipv6_proto != flow_key->ip.proto)
			return -EINVAL;

		if (ipv6_key->ipv6_frag != flow_key->ip.frag)
			return -EINVAL;

		if (ntohl(ipv6_key->ipv6_label) & 0xFFF00000)
			return -EINVAL;

		break;
	}

	case OVS_KEY_ATTR_TCP:
		if (flow_key->ip.proto != IPPROTO_TCP)
			return -EINVAL;

		return validate_tp_port(flow_key, eth_types);

	case OVS_KEY_ATTR_UDP:
		if (flow_key->ip.proto != IPPROTO_UDP)
			return -EINVAL;

		return validate_tp_port(flow_key, eth_types);

	case OVS_KEY_ATTR_MPLS: {
		int i;

		for (i = eth_types->start; i < eth_types->end; i++)
			if (!eth_p_mpls(eth_types->types[i]))
				return -EINVAL;
		break;
	}

	case OVS_KEY_ATTR_SCTP:
		if (flow_key->ip.proto != IPPROTO_SCTP)
			return -EINVAL;

		return validate_tp_port(flow_key, eth_types);

	default:
		return -EINVAL;
	}

	return 0;
}

static int validate_userspace(const struct nlattr *attr)
{
	static const struct nla_policy userspace_policy[OVS_USERSPACE_ATTR_MAX + 1] =	{
		[OVS_USERSPACE_ATTR_PID] = {.type = NLA_U32 },
		[OVS_USERSPACE_ATTR_USERDATA] = {.type = NLA_UNSPEC },
	};
	struct nlattr *a[OVS_USERSPACE_ATTR_MAX + 1];
	int error;

	error = nla_parse_nested(a, OVS_USERSPACE_ATTR_MAX,
				 attr, userspace_policy);
	if (error)
		return error;

	if (!a[OVS_USERSPACE_ATTR_PID] ||
	    !nla_get_u32(a[OVS_USERSPACE_ATTR_PID]))
		return -EINVAL;

	return 0;
}

static int copy_action(const struct nlattr *from,
		      struct sw_flow_actions **sfa)
{
	int totlen = NLA_ALIGN(from->nla_len);
	struct nlattr *to;

	to = reserve_sfa_size(sfa, from->nla_len);
	if (IS_ERR(to))
		return PTR_ERR(to);

	memcpy(to, from, totlen);
	return 0;
}

static int validate_and_copy_actions__(const struct nlattr *attr,
				const struct sw_flow_key *key, int depth,
				struct sw_flow_actions **sfa,
				struct eth_types *eth_types)
{
	const struct nlattr *a;
	int rem, err;

	if (depth >= SAMPLE_ACTION_DEPTH)
		return -EOVERFLOW;

	nla_for_each_nested(a, attr, rem) {
		/* Expected argument lengths, (u32)-1 for variable length. */
		static const u32 action_lens[OVS_ACTION_ATTR_MAX + 1] = {
			[OVS_ACTION_ATTR_OUTPUT] = sizeof(u32),
			[OVS_ACTION_ATTR_USERSPACE] = (u32)-1,
			[OVS_ACTION_ATTR_PUSH_MPLS] = sizeof(struct ovs_action_push_mpls),
			[OVS_ACTION_ATTR_POP_MPLS] = sizeof(__be16),
			[OVS_ACTION_ATTR_PUSH_VLAN] = sizeof(struct ovs_action_push_vlan),
			[OVS_ACTION_ATTR_POP_VLAN] = 0,
			[OVS_ACTION_ATTR_SET] = (u32)-1,
			[OVS_ACTION_ATTR_SAMPLE] = (u32)-1
		};
		const struct ovs_action_push_vlan *vlan;
		int type = nla_type(a);
		bool skip_copy;

		if (type > OVS_ACTION_ATTR_MAX ||
		    (action_lens[type] != nla_len(a) &&
		     action_lens[type] != (u32)-1))
			return -EINVAL;

		skip_copy = false;
		switch (type) {
		case OVS_ACTION_ATTR_UNSPEC:
			return -EINVAL;

		case OVS_ACTION_ATTR_USERSPACE:
			err = validate_userspace(a);
			if (err)
				return err;
			break;

		case OVS_ACTION_ATTR_OUTPUT:
			if (nla_get_u32(a) >= DP_MAX_PORTS)
				return -EINVAL;
			break;

		case OVS_ACTION_ATTR_PUSH_MPLS: {
			const struct ovs_action_push_mpls *mpls = nla_data(a);
			if (!eth_p_mpls(mpls->mpls_ethertype))
				return -EINVAL;
			eth_types_set(eth_types, mpls->mpls_ethertype);
			break;
		}

		case OVS_ACTION_ATTR_POP_MPLS: {
			int i;

			for (i = eth_types->start; i <= eth_types->end; i++)
				if (!eth_p_mpls(eth_types->types[i]))
					return -EINVAL;

			/* Disallow subsequent L2.5+ set and mpls_pop actions
			 * as there is no check here to ensure that the new
			 * eth_type is valid and thus set actions could
			 * write off the end of the packet or otherwise
			 * corrupt it.
			 *
			 * Support for these actions is planned using packet
			 * recirculation.
			 */
			eth_types_set(eth_types, htons(0));
			break;
		}

		case OVS_ACTION_ATTR_POP_VLAN:
			break;

		case OVS_ACTION_ATTR_PUSH_VLAN:
			vlan = nla_data(a);
			if (vlan->vlan_tpid != htons(ETH_P_8021Q))
				return -EINVAL;
			if (!(vlan->vlan_tci & htons(VLAN_TAG_PRESENT)))
				return -EINVAL;
			break;

		case OVS_ACTION_ATTR_SET:
			err = validate_set(a, key, sfa, &skip_copy, eth_types);
			if (err)
				return err;
			break;

		case OVS_ACTION_ATTR_SAMPLE:
			err = validate_and_copy_sample(a, key, depth, sfa,
						       eth_types);
			if (err)
				return err;
			skip_copy = true;
			break;

		default:
			return -EINVAL;
		}
		if (!skip_copy) {
			err = copy_action(a, sfa);
			if (err)
				return err;
		}
	}

	if (rem > 0)
		return -EINVAL;

	return 0;
}

static int validate_and_copy_actions(const struct nlattr *attr,
				const struct sw_flow_key *key,
				struct sw_flow_actions **sfa)
{
	struct eth_types eth_type = {
		.start = 0,
		.end = 0,
		.cursor = 0,
		.types = { key->eth.type, },
	};

	return validate_and_copy_actions__(attr, key, 0, sfa, &eth_type);
}

static void clear_stats(struct sw_flow *flow)
{
	flow->used = 0;
	flow->tcp_flags = 0;
	flow->packet_count = 0;
	flow->byte_count = 0;
}

static int ovs_packet_cmd_execute(struct sk_buff *skb, struct genl_info *info)
{
	struct ovs_header *ovs_header = info->userhdr;
	struct nlattr **a = info->attrs;
	struct sw_flow_actions *acts;
	struct sk_buff *packet;
	struct sw_flow *flow;
	struct datapath *dp;
	struct ethhdr *eth;
	int len;
	int err;

	err = -EINVAL;
	if (!a[OVS_PACKET_ATTR_PACKET] || !a[OVS_PACKET_ATTR_KEY] ||
	    !a[OVS_PACKET_ATTR_ACTIONS])
		goto err;

	len = nla_len(a[OVS_PACKET_ATTR_PACKET]);
	packet = __dev_alloc_skb(NET_IP_ALIGN + len, GFP_KERNEL);
	err = -ENOMEM;
	if (!packet)
		goto err;
	skb_reserve(packet, NET_IP_ALIGN);

	nla_memcpy(__skb_put(packet, len), a[OVS_PACKET_ATTR_PACKET], len);

	skb_reset_mac_header(packet);
	eth = eth_hdr(packet);

	/* Normally, setting the skb 'protocol' field would be handled by a
	 * call to eth_type_trans(), but it assumes there's a sending
	 * device, which we may not have. */
	if (ntohs(eth->h_proto) >= ETH_P_802_3_MIN)
		packet->protocol = eth->h_proto;
	else
		packet->protocol = htons(ETH_P_802_2);

	/* Build an sw_flow for sending this packet. */
	flow = ovs_flow_alloc();
	err = PTR_ERR(flow);
	if (IS_ERR(flow))
		goto err_kfree_skb;

	err = ovs_flow_extract(packet, -1, &flow->key);
	if (err)
		goto err_flow_free;

	err = ovs_flow_metadata_from_nlattrs(flow, a[OVS_PACKET_ATTR_KEY]);
	if (err)
		goto err_flow_free;
	acts = ovs_flow_actions_alloc(nla_len(a[OVS_PACKET_ATTR_ACTIONS]));
	err = PTR_ERR(acts);
	if (IS_ERR(acts))
		goto err_flow_free;

	err = validate_and_copy_actions(a[OVS_PACKET_ATTR_ACTIONS], &flow->key, &acts);
	rcu_assign_pointer(flow->sf_acts, acts);
	if (err)
		goto err_flow_free;

	OVS_CB(packet)->flow = flow;
	OVS_CB(packet)->pkt_key = &flow->key;
	packet->priority = flow->key.phy.priority;
	packet->mark = flow->key.phy.skb_mark;

	rcu_read_lock();
	dp = get_dp(sock_net(skb->sk), ovs_header->dp_ifindex);
	err = -ENODEV;
	if (!dp)
		goto err_unlock;

	local_bh_disable();
	err = ovs_execute_actions(dp, packet);
	local_bh_enable();
	rcu_read_unlock();

	ovs_flow_free(flow, false);
	return err;

err_unlock:
	rcu_read_unlock();
err_flow_free:
	ovs_flow_free(flow, false);
err_kfree_skb:
	kfree_skb(packet);
err:
	return err;
}

static const struct nla_policy packet_policy[OVS_PACKET_ATTR_MAX + 1] = {
	[OVS_PACKET_ATTR_PACKET] = { .len = ETH_HLEN },
	[OVS_PACKET_ATTR_KEY] = { .type = NLA_NESTED },
	[OVS_PACKET_ATTR_ACTIONS] = { .type = NLA_NESTED },
};

static struct genl_ops dp_packet_genl_ops[] = {
	{ .cmd = OVS_PACKET_CMD_EXECUTE,
	  .flags = GENL_ADMIN_PERM, /* Requires CAP_NET_ADMIN privilege. */
	  .policy = packet_policy,
	  .doit = ovs_packet_cmd_execute
	}
};

static void get_dp_stats(struct datapath *dp, struct ovs_dp_stats *stats)
{
	struct flow_table *table;
	int i;

	table = rcu_dereference_check(dp->table, lockdep_ovsl_is_held());
	stats->n_flows = ovs_flow_tbl_count(table);

	stats->n_hit = stats->n_missed = stats->n_lost = 0;
	for_each_possible_cpu(i) {
		const struct dp_stats_percpu *percpu_stats;
		struct dp_stats_percpu local_stats;
		unsigned int start;

		percpu_stats = per_cpu_ptr(dp->stats_percpu, i);

		do {
			start = u64_stats_fetch_begin_bh(&percpu_stats->sync);
			local_stats = *percpu_stats;
		} while (u64_stats_fetch_retry_bh(&percpu_stats->sync, start));

		stats->n_hit += local_stats.n_hit;
		stats->n_missed += local_stats.n_missed;
		stats->n_lost += local_stats.n_lost;
	}
}

static const struct nla_policy flow_policy[OVS_FLOW_ATTR_MAX + 1] = {
	[OVS_FLOW_ATTR_KEY] = { .type = NLA_NESTED },
	[OVS_FLOW_ATTR_ACTIONS] = { .type = NLA_NESTED },
	[OVS_FLOW_ATTR_CLEAR] = { .type = NLA_FLAG },
};

static struct genl_family dp_flow_genl_family = {
	.id = GENL_ID_GENERATE,
	.hdrsize = sizeof(struct ovs_header),
	.name = OVS_FLOW_FAMILY,
	.version = OVS_FLOW_VERSION,
	.maxattr = OVS_FLOW_ATTR_MAX,
	.netnsok = true,
	 SET_PARALLEL_OPS
};

static struct genl_multicast_group ovs_dp_flow_multicast_group = {
	.name = OVS_FLOW_MCGROUP
};

static int actions_to_attr(const struct nlattr *attr, int len, struct sk_buff *skb);
static int sample_action_to_attr(const struct nlattr *attr, struct sk_buff *skb)
{
	const struct nlattr *a;
	struct nlattr *start;
	int err = 0, rem;

	start = nla_nest_start(skb, OVS_ACTION_ATTR_SAMPLE);
	if (!start)
		return -EMSGSIZE;

	nla_for_each_nested(a, attr, rem) {
		int type = nla_type(a);
		struct nlattr *st_sample;

		switch (type) {
		case OVS_SAMPLE_ATTR_PROBABILITY:
			if (nla_put(skb, OVS_SAMPLE_ATTR_PROBABILITY, sizeof(u32), nla_data(a)))
				return -EMSGSIZE;
			break;
		case OVS_SAMPLE_ATTR_ACTIONS:
			st_sample = nla_nest_start(skb, OVS_SAMPLE_ATTR_ACTIONS);
			if (!st_sample)
				return -EMSGSIZE;
			err = actions_to_attr(nla_data(a), nla_len(a), skb);
			if (err)
				return err;
			nla_nest_end(skb, st_sample);
			break;
		}
	}

	nla_nest_end(skb, start);
	return err;
}

static int set_action_to_attr(const struct nlattr *a, struct sk_buff *skb)
{
	const struct nlattr *ovs_key = nla_data(a);
	int key_type = nla_type(ovs_key);
	struct nlattr *start;
	int err;

	switch (key_type) {
	case OVS_KEY_ATTR_IPV4_TUNNEL:
		start = nla_nest_start(skb, OVS_ACTION_ATTR_SET);
		if (!start)
			return -EMSGSIZE;

		err = ovs_ipv4_tun_to_nlattr(skb, nla_data(ovs_key),
					     nla_data(ovs_key));
		if (err)
			return err;
		nla_nest_end(skb, start);
		break;
	default:
		if (nla_put(skb, OVS_ACTION_ATTR_SET, nla_len(a), ovs_key))
			return -EMSGSIZE;
		break;
	}

	return 0;
}

static int actions_to_attr(const struct nlattr *attr, int len, struct sk_buff *skb)
{
	const struct nlattr *a;
	int rem, err;

	nla_for_each_attr(a, attr, len, rem) {
		int type = nla_type(a);

		switch (type) {
		case OVS_ACTION_ATTR_SET:
			err = set_action_to_attr(a, skb);
			if (err)
				return err;
			break;

		case OVS_ACTION_ATTR_SAMPLE:
			err = sample_action_to_attr(a, skb);
			if (err)
				return err;
			break;
		default:
			if (nla_put(skb, type, nla_len(a), nla_data(a)))
				return -EMSGSIZE;
			break;
		}
	}

	return 0;
}

static size_t ovs_flow_cmd_msg_size(const struct sw_flow_actions *acts)
{
	return NLMSG_ALIGN(sizeof(struct ovs_header))
		+ nla_total_size(key_attr_size()) /* OVS_FLOW_ATTR_KEY */
		+ nla_total_size(key_attr_size()) /* OVS_FLOW_ATTR_MASK */
		+ nla_total_size(sizeof(struct ovs_flow_stats)) /* OVS_FLOW_ATTR_STATS */
		+ nla_total_size(1) /* OVS_FLOW_ATTR_TCP_FLAGS */
		+ nla_total_size(8) /* OVS_FLOW_ATTR_USED */
		+ nla_total_size(acts->actions_len); /* OVS_FLOW_ATTR_ACTIONS */
}

/* Called with ovs_mutex. */
static int ovs_flow_cmd_fill_info(struct sw_flow *flow, struct datapath *dp,
				  struct sk_buff *skb, u32 portid,
				  u32 seq, u32 flags, u8 cmd)
{
	const int skb_orig_len = skb->len;
	struct nlattr *start;
	struct ovs_flow_stats stats;
	struct ovs_header *ovs_header;
	struct nlattr *nla;
	unsigned long used;
	u8 tcp_flags;
	int err;

	ovs_header = genlmsg_put(skb, portid, seq, &dp_flow_genl_family, flags, cmd);
	if (!ovs_header)
		return -EMSGSIZE;

	ovs_header->dp_ifindex = get_dpifindex(dp);

	/* Fill flow key. */
	nla = nla_nest_start(skb, OVS_FLOW_ATTR_KEY);
	if (!nla)
		goto nla_put_failure;

	err = ovs_flow_to_nlattrs(&flow->unmasked_key,
			&flow->unmasked_key, skb);
	if (err)
		goto error;
	nla_nest_end(skb, nla);

	nla = nla_nest_start(skb, OVS_FLOW_ATTR_MASK);
	if (!nla)
		goto nla_put_failure;

	err = ovs_flow_to_nlattrs(&flow->key, &flow->mask->key, skb);
	if (err)
		goto error;

	nla_nest_end(skb, nla);

	spin_lock_bh(&flow->lock);
	used = flow->used;
	stats.n_packets = flow->packet_count;
	stats.n_bytes = flow->byte_count;
	tcp_flags = flow->tcp_flags;
	spin_unlock_bh(&flow->lock);

	if (used &&
	    nla_put_u64(skb, OVS_FLOW_ATTR_USED, ovs_flow_used_time(used)))
		goto nla_put_failure;

	if (stats.n_packets &&
	    nla_put(skb, OVS_FLOW_ATTR_STATS,
		    sizeof(struct ovs_flow_stats), &stats))
		goto nla_put_failure;

	if (tcp_flags &&
	    nla_put_u8(skb, OVS_FLOW_ATTR_TCP_FLAGS, tcp_flags))
		goto nla_put_failure;

	/* If OVS_FLOW_ATTR_ACTIONS doesn't fit, skip dumping the actions if
	 * this is the first flow to be dumped into 'skb'.  This is unusual for
	 * Netlink but individual action lists can be longer than
	 * NLMSG_GOODSIZE and thus entirely undumpable if we didn't do this.
	 * The userspace caller can always fetch the actions separately if it
	 * really wants them.  (Most userspace callers in fact don't care.)
	 *
	 * This can only fail for dump operations because the skb is always
	 * properly sized for single flows.
	 */
	start = nla_nest_start(skb, OVS_FLOW_ATTR_ACTIONS);
	if (start) {
		const struct sw_flow_actions *sf_acts;

		sf_acts = rcu_dereference_check(flow->sf_acts,
						lockdep_ovsl_is_held());

		err = actions_to_attr(sf_acts->actions, sf_acts->actions_len, skb);
		if (!err)
			nla_nest_end(skb, start);
		else {
			if (skb_orig_len)
				goto error;

			nla_nest_cancel(skb, start);
		}
	} else if (skb_orig_len)
		goto nla_put_failure;

	return genlmsg_end(skb, ovs_header);

nla_put_failure:
	err = -EMSGSIZE;
error:
	genlmsg_cancel(skb, ovs_header);
	return err;
}

static struct sk_buff *ovs_flow_cmd_alloc_info(struct sw_flow *flow)
{
	const struct sw_flow_actions *sf_acts;

	sf_acts = ovsl_dereference(flow->sf_acts);

	return genlmsg_new(ovs_flow_cmd_msg_size(sf_acts), GFP_KERNEL);
}

static struct sk_buff *ovs_flow_cmd_build_info(struct sw_flow *flow,
					       struct datapath *dp,
					       u32 portid, u32 seq, u8 cmd)
{
	struct sk_buff *skb;
	int retval;

	skb = ovs_flow_cmd_alloc_info(flow);
	if (!skb)
		return ERR_PTR(-ENOMEM);

	retval = ovs_flow_cmd_fill_info(flow, dp, skb, portid, seq, 0, cmd);
	BUG_ON(retval < 0);
	return skb;
}

static int ovs_flow_cmd_new_or_set(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr **a = info->attrs;
	struct ovs_header *ovs_header = info->userhdr;
	struct sw_flow_key key, masked_key;
	struct sw_flow *flow = NULL;
	struct sw_flow_mask mask;
	struct sk_buff *reply;
	struct datapath *dp;
	struct flow_table *table;
	struct sw_flow_actions *acts = NULL;
	struct sw_flow_match match;
	int error;

	/* Extract key. */
	error = -EINVAL;
	if (!a[OVS_FLOW_ATTR_KEY])
		goto error;

	ovs_match_init(&match, &key, &mask);
	error = ovs_match_from_nlattrs(&match,
			a[OVS_FLOW_ATTR_KEY], a[OVS_FLOW_ATTR_MASK]);
	if (error)
		goto error;

	/* Validate actions. */
	if (a[OVS_FLOW_ATTR_ACTIONS]) {
		acts = ovs_flow_actions_alloc(nla_len(a[OVS_FLOW_ATTR_ACTIONS]));
		error = PTR_ERR(acts);
		if (IS_ERR(acts))
			goto error;

		ovs_flow_key_mask(&masked_key, &key, &mask);
		error = validate_and_copy_actions(a[OVS_FLOW_ATTR_ACTIONS],
						  &masked_key, &acts);
		if (error) {
			OVS_NLERR("Flow actions may not be safe on all matching packets.\n");
			goto err_kfree;
		}
	} else if (info->genlhdr->cmd == OVS_FLOW_CMD_NEW) {
		error = -EINVAL;
		goto error;
	}

	ovs_lock();
	dp = get_dp(sock_net(skb->sk), ovs_header->dp_ifindex);
	error = -ENODEV;
	if (!dp)
		goto err_unlock_ovs;

	table = ovsl_dereference(dp->table);

	/* Check if this is a duplicate flow */
	flow = ovs_flow_lookup(table, &key);
	if (!flow) {
		struct flow_table *new_table = NULL;
		struct sw_flow_mask *mask_p;

		/* Bail out if we're not allowed to create a new flow. */
		error = -ENOENT;
		if (info->genlhdr->cmd == OVS_FLOW_CMD_SET)
			goto err_unlock_ovs;

		/* Expand table, if necessary, to make room. */
		if (ovs_flow_tbl_need_to_expand(table))
			new_table = ovs_flow_tbl_expand(table);
		else if (time_after(jiffies, dp->last_rehash + REHASH_FLOW_INTERVAL))
			new_table = ovs_flow_tbl_rehash(table);

		if (new_table && !IS_ERR(new_table)) {
			rcu_assign_pointer(dp->table, new_table);
			ovs_flow_tbl_destroy(table, true);
			table = ovsl_dereference(dp->table);
			dp->last_rehash = jiffies;
		}

		/* Allocate flow. */
		flow = ovs_flow_alloc();
		if (IS_ERR(flow)) {
			error = PTR_ERR(flow);
			goto err_unlock_ovs;
		}
		clear_stats(flow);

		flow->key = masked_key;
		flow->unmasked_key = key;

		/* Make sure mask is unique in the system */
		mask_p = ovs_sw_flow_mask_find(table, &mask);
		if (!mask_p) {
			/* Allocate a new mask if none exsits. */
			mask_p = ovs_sw_flow_mask_alloc();
			if (!mask_p)
				goto err_flow_free;
			mask_p->key = mask.key;
			mask_p->range = mask.range;
			ovs_sw_flow_mask_insert(table, mask_p);
		}

		ovs_sw_flow_mask_add_ref(mask_p);
		flow->mask = mask_p;
		rcu_assign_pointer(flow->sf_acts, acts);

		/* Put flow in bucket. */
		ovs_flow_insert(table, flow);

		reply = ovs_flow_cmd_build_info(flow, dp, info->snd_portid,
						info->snd_seq, OVS_FLOW_CMD_NEW);
	} else {
		/* We found a matching flow. */
		struct sw_flow_actions *old_acts;

		/* Bail out if we're not allowed to modify an existing flow.
		 * We accept NLM_F_CREATE in place of the intended NLM_F_EXCL
		 * because Generic Netlink treats the latter as a dump
		 * request.  We also accept NLM_F_EXCL in case that bug ever
		 * gets fixed.
		 */
		error = -EEXIST;
		if (info->genlhdr->cmd == OVS_FLOW_CMD_NEW &&
		    info->nlhdr->nlmsg_flags & (NLM_F_CREATE | NLM_F_EXCL))
			goto err_unlock_ovs;

		/* The unmasked key has to be the same for flow updates. */
		error = -EINVAL;
		if (!ovs_flow_cmp_unmasked_key(flow, &key, match.range.end)) {
			OVS_NLERR("Flow modification message rejected, unmasked key does not match.\n");
			goto err_unlock_ovs;
		}

		/* Update actions. */
		old_acts = ovsl_dereference(flow->sf_acts);
		rcu_assign_pointer(flow->sf_acts, acts);
		ovs_flow_deferred_free_acts(old_acts);

		reply = ovs_flow_cmd_build_info(flow, dp, info->snd_portid,
					       info->snd_seq, OVS_FLOW_CMD_NEW);

		/* Clear stats. */
		if (a[OVS_FLOW_ATTR_CLEAR]) {
			spin_lock_bh(&flow->lock);
			clear_stats(flow);
			spin_unlock_bh(&flow->lock);
		}
	}
	ovs_unlock();

	if (!IS_ERR(reply))
		ovs_notify(reply, info, &ovs_dp_flow_multicast_group);
	else
		netlink_set_err(sock_net(skb->sk)->genl_sock, 0,
				ovs_dp_flow_multicast_group.id,	PTR_ERR(reply));
	return 0;

err_flow_free:
	ovs_flow_free(flow, false);
err_unlock_ovs:
	ovs_unlock();
err_kfree:
	kfree(acts);
error:
	return error;
}

static int ovs_flow_cmd_get(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr **a = info->attrs;
	struct ovs_header *ovs_header = info->userhdr;
	struct sw_flow_key key;
	struct sk_buff *reply;
	struct sw_flow *flow;
	struct datapath *dp;
	struct flow_table *table;
	struct sw_flow_match match;
	int err;

	if (!a[OVS_FLOW_ATTR_KEY]) {
		OVS_NLERR("Flow get message rejected, Key attribute missing.\n");
		return -EINVAL;
	}

	ovs_match_init(&match, &key, NULL);
	err = ovs_match_from_nlattrs(&match, a[OVS_FLOW_ATTR_KEY], NULL);
	if (err)
		return err;

	ovs_lock();
	dp = get_dp(sock_net(skb->sk), ovs_header->dp_ifindex);
	if (!dp) {
		err = -ENODEV;
		goto unlock;
	}

	table = ovsl_dereference(dp->table);
	flow = ovs_flow_lookup_unmasked_key(table, &match);
	if (!flow) {
		err = -ENOENT;
		goto unlock;
	}

	reply = ovs_flow_cmd_build_info(flow, dp, info->snd_portid,
					info->snd_seq, OVS_FLOW_CMD_NEW);
	if (IS_ERR(reply)) {
		err = PTR_ERR(reply);
		goto unlock;
	}

	ovs_unlock();
	return genlmsg_reply(reply, info);
unlock:
	ovs_unlock();
	return err;
}

static int ovs_flow_cmd_del(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr **a = info->attrs;
	struct ovs_header *ovs_header = info->userhdr;
	struct sw_flow_key key;
	struct sk_buff *reply;
	struct sw_flow *flow;
	struct datapath *dp;
	struct flow_table *table;
	struct sw_flow_match match;
	int err;

	ovs_lock();
	dp = get_dp(sock_net(skb->sk), ovs_header->dp_ifindex);
	if (!dp) {
		err = -ENODEV;
		goto unlock;
	}

	if (!a[OVS_FLOW_ATTR_KEY]) {
		err = flush_flows(dp);
		goto unlock;
	}

	ovs_match_init(&match, &key, NULL);
	err = ovs_match_from_nlattrs(&match, a[OVS_FLOW_ATTR_KEY], NULL);
	if (err)
		goto unlock;

	table = ovsl_dereference(dp->table);
	flow = ovs_flow_lookup_unmasked_key(table, &match);
	if (!flow) {
		err = -ENOENT;
		goto unlock;
	}

	reply = ovs_flow_cmd_alloc_info(flow);
	if (!reply) {
		err = -ENOMEM;
		goto unlock;
	}

	ovs_flow_remove(table, flow);

	err = ovs_flow_cmd_fill_info(flow, dp, reply, info->snd_portid,
				     info->snd_seq, 0, OVS_FLOW_CMD_DEL);
	BUG_ON(err < 0);

	ovs_flow_free(flow, true);
	ovs_unlock();

	ovs_notify(reply, info, &ovs_dp_flow_multicast_group);
	return 0;
unlock:
	ovs_unlock();
	return err;
}

static int ovs_flow_cmd_dump(struct sk_buff *skb, struct netlink_callback *cb)
{
	struct ovs_header *ovs_header = genlmsg_data(nlmsg_data(cb->nlh));
	struct datapath *dp;
	struct flow_table *table;

	rcu_read_lock();
	dp = get_dp(sock_net(skb->sk), ovs_header->dp_ifindex);
	if (!dp) {
		rcu_read_unlock();
		return -ENODEV;
	}

	table = rcu_dereference(dp->table);
	for (;;) {
		struct sw_flow *flow;
		u32 bucket, obj;

		bucket = cb->args[0];
		obj = cb->args[1];
		flow = ovs_flow_dump_next(table, &bucket, &obj);
		if (!flow)
			break;

		if (ovs_flow_cmd_fill_info(flow, dp, skb,
					   NETLINK_CB(cb->skb).portid,
					   cb->nlh->nlmsg_seq, NLM_F_MULTI,
					   OVS_FLOW_CMD_NEW) < 0)
			break;

		cb->args[0] = bucket;
		cb->args[1] = obj;
	}
	rcu_read_unlock();
	return skb->len;
}

static struct genl_ops dp_flow_genl_ops[] = {
	{ .cmd = OVS_FLOW_CMD_NEW,
	  .flags = GENL_ADMIN_PERM, /* Requires CAP_NET_ADMIN privilege. */
	  .policy = flow_policy,
	  .doit = ovs_flow_cmd_new_or_set
	},
	{ .cmd = OVS_FLOW_CMD_DEL,
	  .flags = GENL_ADMIN_PERM, /* Requires CAP_NET_ADMIN privilege. */
	  .policy = flow_policy,
	  .doit = ovs_flow_cmd_del
	},
	{ .cmd = OVS_FLOW_CMD_GET,
	  .flags = 0,		    /* OK for unprivileged users. */
	  .policy = flow_policy,
	  .doit = ovs_flow_cmd_get,
	  .dumpit = ovs_flow_cmd_dump
	},
	{ .cmd = OVS_FLOW_CMD_SET,
	  .flags = GENL_ADMIN_PERM, /* Requires CAP_NET_ADMIN privilege. */
	  .policy = flow_policy,
	  .doit = ovs_flow_cmd_new_or_set,
	},
};

static const struct nla_policy datapath_policy[OVS_DP_ATTR_MAX + 1] = {
	[OVS_DP_ATTR_NAME] = { .type = NLA_NUL_STRING, .len = IFNAMSIZ - 1 },
	[OVS_DP_ATTR_UPCALL_PID] = { .type = NLA_U32 },
};

static struct genl_family dp_datapath_genl_family = {
	.id = GENL_ID_GENERATE,
	.hdrsize = sizeof(struct ovs_header),
	.name = OVS_DATAPATH_FAMILY,
	.version = OVS_DATAPATH_VERSION,
	.maxattr = OVS_DP_ATTR_MAX,
	.netnsok = true,
	 SET_PARALLEL_OPS
};

static struct genl_multicast_group ovs_dp_datapath_multicast_group = {
	.name = OVS_DATAPATH_MCGROUP
};

static size_t ovs_dp_cmd_msg_size(void)
{
	size_t msgsize = NLMSG_ALIGN(sizeof(struct ovs_header));

	msgsize += nla_total_size(IFNAMSIZ);
	msgsize += nla_total_size(sizeof(struct ovs_dp_stats));

	return msgsize;
}

static int ovs_dp_cmd_fill_info(struct datapath *dp, struct sk_buff *skb,
				u32 portid, u32 seq, u32 flags, u8 cmd)
{
	struct ovs_header *ovs_header;
	struct ovs_dp_stats dp_stats;
	int err;

	ovs_header = genlmsg_put(skb, portid, seq, &dp_datapath_genl_family,
				   flags, cmd);
	if (!ovs_header)
		goto error;

	ovs_header->dp_ifindex = get_dpifindex(dp);

	rcu_read_lock();
	err = nla_put_string(skb, OVS_DP_ATTR_NAME, ovs_dp_name(dp));
	rcu_read_unlock();
	if (err)
		goto nla_put_failure;

	get_dp_stats(dp, &dp_stats);
	if (nla_put(skb, OVS_DP_ATTR_STATS, sizeof(struct ovs_dp_stats), &dp_stats))
		goto nla_put_failure;

	return genlmsg_end(skb, ovs_header);

nla_put_failure:
	genlmsg_cancel(skb, ovs_header);
error:
	return -EMSGSIZE;
}

static struct sk_buff *ovs_dp_cmd_build_info(struct datapath *dp, u32 portid,
					     u32 seq, u8 cmd)
{
	struct sk_buff *skb;
	int retval;

	skb = genlmsg_new(ovs_dp_cmd_msg_size(), GFP_KERNEL);
	if (!skb)
		return ERR_PTR(-ENOMEM);

	retval = ovs_dp_cmd_fill_info(dp, skb, portid, seq, 0, cmd);
	if (retval < 0) {
		kfree_skb(skb);
		return ERR_PTR(retval);
	}
	return skb;
}

/* Called with ovs_mutex. */
static struct datapath *lookup_datapath(struct net *net,
					struct ovs_header *ovs_header,
					struct nlattr *a[OVS_DP_ATTR_MAX + 1])
{
	struct datapath *dp;

	if (!a[OVS_DP_ATTR_NAME])
		dp = get_dp(net, ovs_header->dp_ifindex);
	else {
		struct vport *vport;

		rcu_read_lock();
		vport = ovs_vport_locate(net, nla_data(a[OVS_DP_ATTR_NAME]));
		dp = vport && vport->port_no == OVSP_LOCAL ? vport->dp : NULL;
		rcu_read_unlock();
	}
	return dp ? dp : ERR_PTR(-ENODEV);
}

static int ovs_dp_cmd_new(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr **a = info->attrs;
	struct vport_parms parms;
	struct sk_buff *reply;
	struct datapath *dp;
	struct vport *vport;
	struct ovs_net *ovs_net;
	int err, i;

	err = -EINVAL;
	if (!a[OVS_DP_ATTR_NAME] || !a[OVS_DP_ATTR_UPCALL_PID])
		goto err;

	ovs_lock();

	err = -ENOMEM;
	dp = kzalloc(sizeof(*dp), GFP_KERNEL);
	if (dp == NULL)
		goto err_unlock_ovs;

	ovs_dp_set_net(dp, hold_net(sock_net(skb->sk)));

	/* Allocate table. */
	err = -ENOMEM;
	rcu_assign_pointer(dp->table, ovs_flow_tbl_alloc(TBL_MIN_BUCKETS));
	if (!dp->table)
		goto err_free_dp;

	dp->stats_percpu = alloc_percpu(struct dp_stats_percpu);
	if (!dp->stats_percpu) {
		err = -ENOMEM;
		goto err_destroy_table;
	}

	dp->ports = kmalloc(DP_VPORT_HASH_BUCKETS * sizeof(struct hlist_head),
			    GFP_KERNEL);
	if (!dp->ports) {
		err = -ENOMEM;
		goto err_destroy_percpu;
	}

	for (i = 0; i < DP_VPORT_HASH_BUCKETS; i++)
		INIT_HLIST_HEAD(&dp->ports[i]);

	/* Set up our datapath device. */
	parms.name = nla_data(a[OVS_DP_ATTR_NAME]);
	parms.type = OVS_VPORT_TYPE_INTERNAL;
	parms.options = NULL;
	parms.dp = dp;
	parms.port_no = OVSP_LOCAL;
	parms.upcall_portid = nla_get_u32(a[OVS_DP_ATTR_UPCALL_PID]);

	vport = new_vport(&parms);
	if (IS_ERR(vport)) {
		err = PTR_ERR(vport);
		if (err == -EBUSY)
			err = -EEXIST;

		goto err_destroy_ports_array;
	}

	reply = ovs_dp_cmd_build_info(dp, info->snd_portid,
				      info->snd_seq, OVS_DP_CMD_NEW);
	err = PTR_ERR(reply);
	if (IS_ERR(reply))
		goto err_destroy_local_port;

	ovs_net = net_generic(ovs_dp_get_net(dp), ovs_net_id);
	list_add_tail_rcu(&dp->list_node, &ovs_net->dps);

	ovs_unlock();

	ovs_notify(reply, info, &ovs_dp_datapath_multicast_group);
	return 0;

err_destroy_local_port:
	ovs_dp_detach_port(ovs_vport_ovsl(dp, OVSP_LOCAL));
err_destroy_ports_array:
	kfree(dp->ports);
err_destroy_percpu:
	free_percpu(dp->stats_percpu);
err_destroy_table:
	ovs_flow_tbl_destroy(ovsl_dereference(dp->table), false);
err_free_dp:
	release_net(ovs_dp_get_net(dp));
	kfree(dp);
err_unlock_ovs:
	ovs_unlock();
err:
	return err;
}

/* Called with ovs_mutex. */
static void __dp_destroy(struct datapath *dp)
{
	int i;

	for (i = 0; i < DP_VPORT_HASH_BUCKETS; i++) {
		struct vport *vport;
		struct hlist_node *n;

		hlist_for_each_entry_safe(vport, n, &dp->ports[i], dp_hash_node)
			if (vport->port_no != OVSP_LOCAL)
				ovs_dp_detach_port(vport);
	}

	list_del_rcu(&dp->list_node);

	/* OVSP_LOCAL is datapath internal port. We need to make sure that
 	 * all port in datapath are destroyed first before freeing datapath.
 	 */
	ovs_dp_detach_port(ovs_vport_ovsl(dp, OVSP_LOCAL));

	call_rcu(&dp->rcu, destroy_dp_rcu);
}

static int ovs_dp_cmd_del(struct sk_buff *skb, struct genl_info *info)
{
	struct sk_buff *reply;
	struct datapath *dp;
	int err;

	ovs_lock();
	dp = lookup_datapath(sock_net(skb->sk), info->userhdr, info->attrs);
	err = PTR_ERR(dp);
	if (IS_ERR(dp))
		goto unlock;

	reply = ovs_dp_cmd_build_info(dp, info->snd_portid,
				      info->snd_seq, OVS_DP_CMD_DEL);
	err = PTR_ERR(reply);
	if (IS_ERR(reply))
		goto unlock;

	__dp_destroy(dp);
	ovs_unlock();

	ovs_notify(reply, info, &ovs_dp_datapath_multicast_group);

	return 0;
unlock:
	ovs_unlock();
	return err;
}

static int ovs_dp_cmd_set(struct sk_buff *skb, struct genl_info *info)
{
	struct sk_buff *reply;
	struct datapath *dp;
	int err;

	ovs_lock();
	dp = lookup_datapath(sock_net(skb->sk), info->userhdr, info->attrs);
	err = PTR_ERR(dp);
	if (IS_ERR(dp))
		goto unlock;

	reply = ovs_dp_cmd_build_info(dp, info->snd_portid,
				      info->snd_seq, OVS_DP_CMD_NEW);
	if (IS_ERR(reply)) {
		err = PTR_ERR(reply);
		netlink_set_err(sock_net(skb->sk)->genl_sock, 0,
				ovs_dp_datapath_multicast_group.id, err);
		err = 0;
		goto unlock;
	}

	ovs_unlock();
	ovs_notify(reply, info, &ovs_dp_datapath_multicast_group);

	return 0;
unlock:
	ovs_unlock();
	return err;
}

static int ovs_dp_cmd_get(struct sk_buff *skb, struct genl_info *info)
{
	struct sk_buff *reply;
	struct datapath *dp;
	int err;

	ovs_lock();
	dp = lookup_datapath(sock_net(skb->sk), info->userhdr, info->attrs);
	if (IS_ERR(dp)) {
		err = PTR_ERR(dp);
		goto unlock;
	}

	reply = ovs_dp_cmd_build_info(dp, info->snd_portid,
				      info->snd_seq, OVS_DP_CMD_NEW);
	if (IS_ERR(reply)) {
		err = PTR_ERR(reply);
		goto unlock;
	}

	ovs_unlock();
	return genlmsg_reply(reply, info);

unlock:
	ovs_unlock();
	return err;
}

static int ovs_dp_cmd_dump(struct sk_buff *skb, struct netlink_callback *cb)
{
	struct ovs_net *ovs_net = net_generic(sock_net(skb->sk), ovs_net_id);
	struct datapath *dp;
	int skip = cb->args[0];
	int i = 0;

	rcu_read_lock();
	list_for_each_entry_rcu(dp, &ovs_net->dps, list_node) {
		if (i >= skip &&
		    ovs_dp_cmd_fill_info(dp, skb, NETLINK_CB(cb->skb).portid,
					 cb->nlh->nlmsg_seq, NLM_F_MULTI,
					 OVS_DP_CMD_NEW) < 0)
			break;
		i++;
	}
	rcu_read_unlock();

	cb->args[0] = i;

	return skb->len;
}

static struct genl_ops dp_datapath_genl_ops[] = {
	{ .cmd = OVS_DP_CMD_NEW,
	  .flags = GENL_ADMIN_PERM, /* Requires CAP_NET_ADMIN privilege. */
	  .policy = datapath_policy,
	  .doit = ovs_dp_cmd_new
	},
	{ .cmd = OVS_DP_CMD_DEL,
	  .flags = GENL_ADMIN_PERM, /* Requires CAP_NET_ADMIN privilege. */
	  .policy = datapath_policy,
	  .doit = ovs_dp_cmd_del
	},
	{ .cmd = OVS_DP_CMD_GET,
	  .flags = 0,		    /* OK for unprivileged users. */
	  .policy = datapath_policy,
	  .doit = ovs_dp_cmd_get,
	  .dumpit = ovs_dp_cmd_dump
	},
	{ .cmd = OVS_DP_CMD_SET,
	  .flags = GENL_ADMIN_PERM, /* Requires CAP_NET_ADMIN privilege. */
	  .policy = datapath_policy,
	  .doit = ovs_dp_cmd_set,
	},
};

static const struct nla_policy vport_policy[OVS_VPORT_ATTR_MAX + 1] = {
	[OVS_VPORT_ATTR_NAME] = { .type = NLA_NUL_STRING, .len = IFNAMSIZ - 1 },
	[OVS_VPORT_ATTR_STATS] = { .len = sizeof(struct ovs_vport_stats) },
	[OVS_VPORT_ATTR_PORT_NO] = { .type = NLA_U32 },
	[OVS_VPORT_ATTR_TYPE] = { .type = NLA_U32 },
	[OVS_VPORT_ATTR_UPCALL_PID] = { .type = NLA_U32 },
	[OVS_VPORT_ATTR_OPTIONS] = { .type = NLA_NESTED },
};

static struct genl_family dp_vport_genl_family = {
	.id = GENL_ID_GENERATE,
	.hdrsize = sizeof(struct ovs_header),
	.name = OVS_VPORT_FAMILY,
	.version = OVS_VPORT_VERSION,
	.maxattr = OVS_VPORT_ATTR_MAX,
	.netnsok = true,
	 SET_PARALLEL_OPS
};

struct genl_multicast_group ovs_dp_vport_multicast_group = {
	.name = OVS_VPORT_MCGROUP
};

/* Called with ovs_mutex or RCU read lock. */
static int ovs_vport_cmd_fill_info(struct vport *vport, struct sk_buff *skb,
				   u32 portid, u32 seq, u32 flags, u8 cmd)
{
	struct ovs_header *ovs_header;
	struct ovs_vport_stats vport_stats;
	int err;

	ovs_header = genlmsg_put(skb, portid, seq, &dp_vport_genl_family,
				 flags, cmd);
	if (!ovs_header)
		return -EMSGSIZE;

	ovs_header->dp_ifindex = get_dpifindex(vport->dp);

	if (nla_put_u32(skb, OVS_VPORT_ATTR_PORT_NO, vport->port_no) ||
	    nla_put_u32(skb, OVS_VPORT_ATTR_TYPE, vport->ops->type) ||
	    nla_put_string(skb, OVS_VPORT_ATTR_NAME, vport->ops->get_name(vport)) ||
	    nla_put_u32(skb, OVS_VPORT_ATTR_UPCALL_PID, vport->upcall_portid))
		goto nla_put_failure;

	ovs_vport_get_stats(vport, &vport_stats);
	if (nla_put(skb, OVS_VPORT_ATTR_STATS, sizeof(struct ovs_vport_stats),
		    &vport_stats))
		goto nla_put_failure;

	err = ovs_vport_get_options(vport, skb);
	if (err == -EMSGSIZE)
		goto error;

	return genlmsg_end(skb, ovs_header);

nla_put_failure:
	err = -EMSGSIZE;
error:
	genlmsg_cancel(skb, ovs_header);
	return err;
}

/* Called with ovs_mutex or RCU read lock. */
struct sk_buff *ovs_vport_cmd_build_info(struct vport *vport, u32 portid,
					 u32 seq, u8 cmd)
{
	struct sk_buff *skb;
	int retval;

	skb = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_ATOMIC);
	if (!skb)
		return ERR_PTR(-ENOMEM);

	retval = ovs_vport_cmd_fill_info(vport, skb, portid, seq, 0, cmd);
	BUG_ON(retval < 0);

	return skb;
}

/* Called with ovs_mutex or RCU read lock. */
static struct vport *lookup_vport(struct net *net,
				  struct ovs_header *ovs_header,
				  struct nlattr *a[OVS_VPORT_ATTR_MAX + 1])
{
	struct datapath *dp;
	struct vport *vport;

	if (a[OVS_VPORT_ATTR_NAME]) {
		vport = ovs_vport_locate(net, nla_data(a[OVS_VPORT_ATTR_NAME]));
		if (!vport)
			return ERR_PTR(-ENODEV);
		if (ovs_header->dp_ifindex &&
		    ovs_header->dp_ifindex != get_dpifindex(vport->dp))
			return ERR_PTR(-ENODEV);
		return vport;
	} else if (a[OVS_VPORT_ATTR_PORT_NO]) {
		u32 port_no = nla_get_u32(a[OVS_VPORT_ATTR_PORT_NO]);

		if (port_no >= DP_MAX_PORTS)
			return ERR_PTR(-EFBIG);

		dp = get_dp(net, ovs_header->dp_ifindex);
		if (!dp)
			return ERR_PTR(-ENODEV);

		vport = ovs_vport_ovsl_rcu(dp, port_no);
		if (!vport)
			return ERR_PTR(-ENODEV);
		return vport;
	} else
		return ERR_PTR(-EINVAL);
}

static int ovs_vport_cmd_new(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr **a = info->attrs;
	struct ovs_header *ovs_header = info->userhdr;
	struct vport_parms parms;
	struct sk_buff *reply;
	struct vport *vport;
	struct datapath *dp;
	u32 port_no;
	int err;

	err = -EINVAL;
	if (!a[OVS_VPORT_ATTR_NAME] || !a[OVS_VPORT_ATTR_TYPE] ||
	    !a[OVS_VPORT_ATTR_UPCALL_PID])
		goto exit;

	ovs_lock();
	dp = get_dp(sock_net(skb->sk), ovs_header->dp_ifindex);
	err = -ENODEV;
	if (!dp)
		goto exit_unlock;

	if (a[OVS_VPORT_ATTR_PORT_NO]) {
		port_no = nla_get_u32(a[OVS_VPORT_ATTR_PORT_NO]);

		err = -EFBIG;
		if (port_no >= DP_MAX_PORTS)
			goto exit_unlock;

		vport = ovs_vport_ovsl(dp, port_no);
		err = -EBUSY;
		if (vport)
			goto exit_unlock;
	} else {
		for (port_no = 1; ; port_no++) {
			if (port_no >= DP_MAX_PORTS) {
				err = -EFBIG;
				goto exit_unlock;
			}
			vport = ovs_vport_ovsl(dp, port_no);
			if (!vport)
				break;
		}
	}

	parms.name = nla_data(a[OVS_VPORT_ATTR_NAME]);
	parms.type = nla_get_u32(a[OVS_VPORT_ATTR_TYPE]);
	parms.options = a[OVS_VPORT_ATTR_OPTIONS];
	parms.dp = dp;
	parms.port_no = port_no;
	parms.upcall_portid = nla_get_u32(a[OVS_VPORT_ATTR_UPCALL_PID]);

	vport = new_vport(&parms);
	err = PTR_ERR(vport);
	if (IS_ERR(vport))
		goto exit_unlock;

	err = 0;
	if (a[OVS_VPORT_ATTR_STATS])
		ovs_vport_set_stats(vport, nla_data(a[OVS_VPORT_ATTR_STATS]));

	reply = ovs_vport_cmd_build_info(vport, info->snd_portid, info->snd_seq,
					 OVS_VPORT_CMD_NEW);
	if (IS_ERR(reply)) {
		err = PTR_ERR(reply);
		ovs_dp_detach_port(vport);
		goto exit_unlock;
	}

	ovs_notify(reply, info, &ovs_dp_vport_multicast_group);

exit_unlock:
	ovs_unlock();
exit:
	return err;
}

static int ovs_vport_cmd_set(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr **a = info->attrs;
	struct sk_buff *reply;
	struct vport *vport;
	int err;

	ovs_lock();
	vport = lookup_vport(sock_net(skb->sk), info->userhdr, a);
	err = PTR_ERR(vport);
	if (IS_ERR(vport))
		goto exit_unlock;

	if (a[OVS_VPORT_ATTR_TYPE] &&
	    nla_get_u32(a[OVS_VPORT_ATTR_TYPE]) != vport->ops->type) {
		err = -EINVAL;
		goto exit_unlock;
	}

	reply = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!reply) {
		err = -ENOMEM;
		goto exit_unlock;
	}

	if (a[OVS_VPORT_ATTR_OPTIONS]) {
		err = ovs_vport_set_options(vport, a[OVS_VPORT_ATTR_OPTIONS]);
		if (err)
			goto exit_free;
	}

	if (a[OVS_VPORT_ATTR_STATS])
		ovs_vport_set_stats(vport, nla_data(a[OVS_VPORT_ATTR_STATS]));

	if (a[OVS_VPORT_ATTR_UPCALL_PID])
		vport->upcall_portid = nla_get_u32(a[OVS_VPORT_ATTR_UPCALL_PID]);

	err = ovs_vport_cmd_fill_info(vport, reply, info->snd_portid,
				      info->snd_seq, 0, OVS_VPORT_CMD_NEW);
	BUG_ON(err < 0);

	ovs_unlock();
	ovs_notify(reply, info, &ovs_dp_vport_multicast_group);
	return 0;

exit_free:
	kfree_skb(reply);
exit_unlock:
	ovs_unlock();
	return err;
}

static int ovs_vport_cmd_del(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr **a = info->attrs;
	struct sk_buff *reply;
	struct vport *vport;
	int err;

	ovs_lock();
	vport = lookup_vport(sock_net(skb->sk), info->userhdr, a);
	err = PTR_ERR(vport);
	if (IS_ERR(vport))
		goto exit_unlock;

	if (vport->port_no == OVSP_LOCAL) {
		err = -EINVAL;
		goto exit_unlock;
	}

	reply = ovs_vport_cmd_build_info(vport, info->snd_portid,
					 info->snd_seq, OVS_VPORT_CMD_DEL);
	err = PTR_ERR(reply);
	if (IS_ERR(reply))
		goto exit_unlock;

	err = 0;
	ovs_dp_detach_port(vport);

	ovs_notify(reply, info, &ovs_dp_vport_multicast_group);

exit_unlock:
	ovs_unlock();
	return err;
}

static int ovs_vport_cmd_get(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr **a = info->attrs;
	struct ovs_header *ovs_header = info->userhdr;
	struct sk_buff *reply;
	struct vport *vport;
	int err;

	rcu_read_lock();
	vport = lookup_vport(sock_net(skb->sk), ovs_header, a);
	err = PTR_ERR(vport);
	if (IS_ERR(vport))
		goto exit_unlock;

	reply = ovs_vport_cmd_build_info(vport, info->snd_portid,
					 info->snd_seq, OVS_VPORT_CMD_NEW);
	err = PTR_ERR(reply);
	if (IS_ERR(reply))
		goto exit_unlock;

	rcu_read_unlock();

	return genlmsg_reply(reply, info);

exit_unlock:
	rcu_read_unlock();
	return err;
}

static int ovs_vport_cmd_dump(struct sk_buff *skb, struct netlink_callback *cb)
{
	struct ovs_header *ovs_header = genlmsg_data(nlmsg_data(cb->nlh));
	struct datapath *dp;
	int bucket = cb->args[0], skip = cb->args[1];
	int i, j = 0;

	dp = get_dp(sock_net(skb->sk), ovs_header->dp_ifindex);
	if (!dp)
		return -ENODEV;

	rcu_read_lock();
	for (i = bucket; i < DP_VPORT_HASH_BUCKETS; i++) {
		struct vport *vport;

		j = 0;
		hlist_for_each_entry_rcu(vport, &dp->ports[i], dp_hash_node) {
			if (j >= skip &&
			    ovs_vport_cmd_fill_info(vport, skb,
						    NETLINK_CB(cb->skb).portid,
						    cb->nlh->nlmsg_seq,
						    NLM_F_MULTI,
						    OVS_VPORT_CMD_NEW) < 0)
				goto out;

			j++;
		}
		skip = 0;
	}
out:
	rcu_read_unlock();

	cb->args[0] = i;
	cb->args[1] = j;

	return skb->len;
}

static struct genl_ops dp_vport_genl_ops[] = {
	{ .cmd = OVS_VPORT_CMD_NEW,
	  .flags = GENL_ADMIN_PERM, /* Requires CAP_NET_ADMIN privilege. */
	  .policy = vport_policy,
	  .doit = ovs_vport_cmd_new
	},
	{ .cmd = OVS_VPORT_CMD_DEL,
	  .flags = GENL_ADMIN_PERM, /* Requires CAP_NET_ADMIN privilege. */
	  .policy = vport_policy,
	  .doit = ovs_vport_cmd_del
	},
	{ .cmd = OVS_VPORT_CMD_GET,
	  .flags = 0,		    /* OK for unprivileged users. */
	  .policy = vport_policy,
	  .doit = ovs_vport_cmd_get,
	  .dumpit = ovs_vport_cmd_dump
	},
	{ .cmd = OVS_VPORT_CMD_SET,
	  .flags = GENL_ADMIN_PERM, /* Requires CAP_NET_ADMIN privilege. */
	  .policy = vport_policy,
	  .doit = ovs_vport_cmd_set,
	},
};

struct genl_family_and_ops {
	struct genl_family *family;
	struct genl_ops *ops;
	int n_ops;
	struct genl_multicast_group *group;
};

static const struct genl_family_and_ops dp_genl_families[] = {
	{ &dp_datapath_genl_family,
	  dp_datapath_genl_ops, ARRAY_SIZE(dp_datapath_genl_ops),
	  &ovs_dp_datapath_multicast_group },
	{ &dp_vport_genl_family,
	  dp_vport_genl_ops, ARRAY_SIZE(dp_vport_genl_ops),
	  &ovs_dp_vport_multicast_group },
	{ &dp_flow_genl_family,
	  dp_flow_genl_ops, ARRAY_SIZE(dp_flow_genl_ops),
	  &ovs_dp_flow_multicast_group },
	{ &dp_packet_genl_family,
	  dp_packet_genl_ops, ARRAY_SIZE(dp_packet_genl_ops),
	  NULL },
};

static void dp_unregister_genl(int n_families)
{
	int i;

	for (i = 0; i < n_families; i++)
		genl_unregister_family(dp_genl_families[i].family);
}

static int dp_register_genl(void)
{
	int n_registered;
	int err;
	int i;

	n_registered = 0;
	for (i = 0; i < ARRAY_SIZE(dp_genl_families); i++) {
		const struct genl_family_and_ops *f = &dp_genl_families[i];

		err = genl_register_family_with_ops(f->family, f->ops,
						    f->n_ops);
		if (err)
			goto error;
		n_registered++;

		if (f->group) {
			err = genl_register_mc_group(f->family, f->group);
			if (err)
				goto error;
		}
	}

	return 0;

error:
	dp_unregister_genl(n_registered);
	return err;
}

static int __net_init ovs_init_net(struct net *net)
{
	struct ovs_net *ovs_net = net_generic(net, ovs_net_id);

	INIT_LIST_HEAD(&ovs_net->dps);
	INIT_WORK(&ovs_net->dp_notify_work, ovs_dp_notify_wq);
	return 0;
}

static void __net_exit ovs_exit_net(struct net *net)
{
	struct datapath *dp, *dp_next;
	struct ovs_net *ovs_net = net_generic(net, ovs_net_id);

	ovs_lock();
	list_for_each_entry_safe(dp, dp_next, &ovs_net->dps, list_node)
		__dp_destroy(dp);
	ovs_unlock();

	cancel_work_sync(&ovs_net->dp_notify_work);
}

static struct pernet_operations ovs_net_ops = {
	.init = ovs_init_net,
	.exit = ovs_exit_net,
	.id   = &ovs_net_id,
	.size = sizeof(struct ovs_net),
};

DEFINE_COMPAT_PNET_REG_FUNC(device);

static int __init dp_init(void)
{
	int err;

	BUILD_BUG_ON(sizeof(struct ovs_skb_cb) > FIELD_SIZEOF(struct sk_buff, cb));

	pr_info("Open vSwitch switching datapath %s, built "__DATE__" "__TIME__"\n",
		VERSION);

	err = ovs_flow_init();
	if (err)
		goto error;

	err = ovs_vport_init();
	if (err)
		goto error_flow_exit;

	err = register_pernet_device(&ovs_net_ops);
	if (err)
		goto error_vport_exit;

	err = register_netdevice_notifier(&ovs_dp_device_notifier);
	if (err)
		goto error_netns_exit;

	err = dp_register_genl();
	if (err < 0)
		goto error_unreg_notifier;

	return 0;

error_unreg_notifier:
	unregister_netdevice_notifier(&ovs_dp_device_notifier);
error_netns_exit:
	unregister_pernet_device(&ovs_net_ops);
error_vport_exit:
	ovs_vport_exit();
error_flow_exit:
	ovs_flow_exit();
error:
	return err;
}

static void dp_cleanup(void)
{
	dp_unregister_genl(ARRAY_SIZE(dp_genl_families));
	unregister_netdevice_notifier(&ovs_dp_device_notifier);
	unregister_pernet_device(&ovs_net_ops);
	rcu_barrier();
	ovs_vport_exit();
	ovs_flow_exit();
}

module_init(dp_init);
module_exit(dp_cleanup);

MODULE_DESCRIPTION("Open vSwitch switching datapath");
MODULE_LICENSE("GPL");
MODULE_VERSION(VERSION);
