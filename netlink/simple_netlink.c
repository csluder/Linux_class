// SPDX-License-Identifier: GPL-2.0
/**
 * @file netlink_test.c
 * @author AI Thought Partner & [Your Name]
 * @brief Unified Sysfs-to-Netlink Broadcast Fix for Kernel 6.12+
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <net/netlink.h>
#include <net/genetlink.h>

#define NETLINK_USER 31    /* Custom protocol family */
#define MY_GROUP     1     /* Multicast group ID */

static struct sock* nl_sk = NULL;
static struct kobject* test_kobj;

/**
 * broadcast_event() - Sends a Netlink multicast message.
 * Fix: Explicitly sets dst_group and uses the raw group ID for nlmsg_multicast.
 */
static void broadcast_event(const char* msg) {
    struct sk_buff* skb;
    struct nlmsghdr* nlh;
    int msg_size = strlen(msg) + 1;
    int res;

    /* Create the socket buffer. Use GFP_KERNEL as sysfs stores can sleep. */
    skb = nlmsg_new(msg_size, GFP_KERNEL);
    if (!skb) return;

    /* Initialize Netlink header */
    nlh = nlmsg_put(skb, 0, 0, NLMSG_DONE, msg_size, 0);

    /* MANDATORY: Set the destination group in the Control Block (CB) */
    NETLINK_CB(skb).dst_group = MY_GROUP;

    /* Copy the payload */
    strncpy(nlmsg_data(nlh), msg, msg_size);

    /**
     * nlmsg_multicast() params:
     * 1. The socket
     * 2. The buffer (skb)
     * 3. portid (0 to skip no one)
     * 4. group (The raw group ID, e.g., 1)
     * 5. allocation flags
     */
    res = nlmsg_multicast(nl_sk, skb, 0, MY_GROUP, GFP_KERNEL);

    if (res < 0)
        pr_err("netlink_test: Send failed with error %d\n", res);
    else
        pr_info("netlink_test: Broadcasted '%s' to group %d\n", msg, MY_GROUP);
}

/* Handler for: echo "msg" > /sys/kernel/netlink_test/trigger */
static ssize_t trigger_store(struct kobject* kobj, struct kobj_attribute* attr,
    const char* buf, size_t count) {
    char kbuf[128];

    /* Sanitize and format the message */
    snprintf(kbuf, sizeof(kbuf), "DATA: %.*s", (int)min(count, sizeof(kbuf) - 10), buf);

    broadcast_event(kbuf);
    return count;
}

static struct kobj_attribute trigger_attribute = __ATTR_WO(trigger);

static int __init netlink_test_init(void) {
    /* Set groups = 1 to enable multicasting for this protocol */
    struct netlink_kernel_cfg cfg = { .groups = 1 };

    nl_sk = netlink_kernel_create(&init_net, NETLINK_USER, &cfg);
    if (!nl_sk) return -ENOMEM;

    test_kobj = kobject_create_and_add("netlink_test", kernel_kobj);
    if (!test_kobj) {
        netlink_kernel_release(nl_sk);
        return -ENOMEM;
    }

    if (sysfs_create_file(test_kobj, &trigger_attribute.attr)) {
        kobject_put(test_kobj);
        netlink_kernel_release(nl_sk);
        return -ENOMEM;
    }

    pr_info("netlink_test: Ready on /sys/kernel/netlink_test/trigger\n");
    return 0;
}

static void __exit netlink_test_exit(void) {
    if (test_kobj) kobject_put(test_kobj);
    if (nl_sk) netlink_kernel_release(nl_sk);
    pr_info("netlink_test: Unloaded\n");
}

module_init(netlink_test_init);
module_exit(netlink_test_exit);
MODULE_LICENSE("GPL");
