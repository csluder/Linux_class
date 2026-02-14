// SPDX-License-Identifier: GPL-2.0
/**
 * @file l3harris_unified.c
 * @author AI Thought Partner & [Your Name]
 * @version 6.3 (Kernel 6.12 Headers Fixed)
 * 
 * PRESENTATION HIGHLIGHTS:
 * 1. Netlink Multicast: Push notifications from kernel to user-space.
 * 2. Hybrid IRQ/Timer: Trigger on edge (IRQ), monitor state via SoftIRQ (Timer).
 * 3. Atomic Allocation: Using GFP_ATOMIC for safe Netlink delivery in timers.
 * 4. Versatile Removal: Pre-processor bridges for 6.11+ void return types.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/machine.h> /* CRITICAL: Required for enum gpio_lookup_flags */
#include <linux/interrupt.h>
#include <linux/timer.h>
#include <linux/property.h>
#include <linux/irq.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <net/genetlink.h> 
#include <net/netlink.h>

/* Handle transition to void return for .remove in 6.11+ */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
    #define USE_REMOVE_NEW
#endif

#define NETLINK_L3HARRIS 31   
#define L3H_MCAST_GROUP  1    

struct l3harris_ctx {
	struct gpio_desc* red;
	struct gpio_desc* blue;
	struct gpio_desc* pir_desc;
	struct timer_list flash_timer;
	struct sock *nl_sk;        
	struct device* dev;
	int irq;
	bool state;
};

/* ---------- Netlink logic ---------- */

static void l3harris_broadcast_event(struct l3harris_ctx *ctx, const char *msg)
{
	struct sk_buff *skb;
	struct nlmsghdr *nlh;
	int msg_size = strlen(msg) + 1;

	skb = nlmsg_new(msg_size, GFP_ATOMIC);
	if (!skb) return;

	nlh = nlmsg_put(skb, 0, 0, NLMSG_DONE, msg_size, 0);
	NETLINK_CB(skb).dst_group = L3H_MCAST_GROUP; 
	strncpy(nlmsg_data(nlh), msg, msg_size);

	nlmsg_multicast(ctx->nl_sk, skb, 0, L3H_MCAST_GROUP, GFP_ATOMIC);
}

/* ---------- IRQ & Timer logic ---------- */

static void flash_timer_handler(struct timer_list* t)
{
	struct l3harris_ctx* ctx = from_timer(ctx, t, flash_timer);

	if (!gpiod_get_raw_value(ctx->pir_desc)) {
		l3harris_broadcast_event(ctx, "STATE:CLEAR");
		gpiod_set_value(ctx->red, 0);
		gpiod_set_value(ctx->blue, 0);
		return; 
	}

	ctx->state = !ctx->state;
	gpiod_set_value(ctx->red, ctx->state);
	gpiod_set_value(ctx->blue, !ctx->state);

	mod_timer(&ctx->flash_timer, jiffies + msecs_to_jiffies(500));
}

static irqreturn_t pir_irq_handler(int irq, void* dev_id)
{
	struct l3harris_ctx* ctx = dev_id;

	if (gpiod_get_raw_value(ctx->pir_desc)) {
		if (!timer_pending(&ctx->flash_timer)) {
			l3harris_broadcast_event(ctx, "STATE:MOTION");
			mod_timer(&ctx->flash_timer, jiffies + msecs_to_jiffies(10));
		}
	}
	return IRQ_HANDLED;
}

/* ---------- Life Cycle ---------- */

static int l3harris_probe(struct platform_device* pdev)
{
	struct device* dev = &pdev->dev;
	struct fwnode_handle *child, *grandchild;
	struct l3harris_ctx* ctx;
	struct irq_data* idata;
	struct gpio_chip* chip;
	struct netlink_kernel_cfg cfg = { .groups = 1 }; 
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx) return -ENOMEM;
	ctx->dev = dev;

	ctx->nl_sk = netlink_kernel_create(&init_net, NETLINK_L3HARRIS, &cfg);
	if (!ctx->nl_sk) return -ENOMEM;

	device_for_each_child_node(dev, child) {
		if (fwnode_name_eq(child, "leds")) {
			fwnode_for_each_child_node(child, grandchild) {
				if (fwnode_name_eq(grandchild, "led_red"))
					ctx->red = devm_fwnode_gpiod_get_index(dev, grandchild, NULL, 0, GPIOD_OUT_LOW, "red");
				else if (fwnode_name_eq(grandchild, "led_blue"))
					ctx->blue = devm_fwnode_gpiod_get_index(dev, grandchild, NULL, 0, GPIOD_OUT_LOW, "blue");
			}
		}
		if (fwnode_name_eq(child, "sr501")) {
			ctx->irq = fwnode_irq_get(child, 0);
		}
	}

	if (IS_ERR_OR_NULL(ctx->red) || IS_ERR_OR_NULL(ctx->blue) || ctx->irq <= 0) {
		ret = -ENODEV;
		goto err_nl;
	}

	idata = irq_get_irq_data(ctx->irq);
	if (!idata || !(chip = irq_data_get_irq_chip_data(idata))) {
		ret = -ENODEV;
		goto err_nl;
	}

	/* 
	 * FIX: Cast the flag to 'enum gpio_lookup_flags' explicitly.
	 * This satisfies the strict type checking in Kernel 6.12.
	 */
	ctx->pir_desc = gpiochip_request_own_desc(chip, idata->hwirq, "pir-sensor",
		(enum gpio_lookup_flags)0, GPIOD_IN);
    
	if (IS_ERR(ctx->pir_desc)) {
		ret = PTR_ERR(ctx->pir_desc);
		goto err_nl;
	}

	timer_setup(&ctx->flash_timer, flash_timer_handler, 0);
	
	ret = devm_request_threaded_irq(dev, ctx->irq, NULL, pir_irq_handler,
		IRQF_TRIGGER_RISING | IRQF_ONESHOT, "l3harris-pir", ctx);

	if (ret) goto err_gpio;

	platform_set_drvdata(pdev, ctx);
	return 0;

err_gpio:
	gpiochip_free_own_desc(ctx->pir_desc);
err_nl:
	netlink_kernel_release(ctx->nl_sk);
	return ret;
}

#ifdef USE_REMOVE_NEW
static void l3harris_remove(struct platform_device* pdev)
#else
static int l3harris_remove(struct platform_device* pdev)
#endif
{
	struct l3harris_ctx* ctx = platform_get_drvdata(pdev);
	if (ctx) {
		del_timer_sync(&ctx->flash_timer);
		if (ctx->nl_sk) netlink_kernel_release(ctx->nl_sk);
		if (ctx->pir_desc) gpiochip_free_own_desc(ctx->pir_desc); 
	}
#ifndef USE_REMOVE_NEW
	return 0;
#endif
}

static const struct of_device_id l3harris_of_match[] = {
	{.compatible = "l3harris,demo-bus" },
	{ }
};
MODULE_DEVICE_TABLE(of, l3harris_of_match);

static struct platform_driver l3harris_driver = {
	.probe = l3harris_probe,
#ifdef USE_REMOVE_NEW
	.remove_new = l3harris_remove, 
#else
	.remove = l3harris_remove,
#endif
	.driver = {
		.name = "l3harris_pir_led",
		.of_match_table = l3harris_of_match
	},
};
module_platform_driver(l3harris_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("AI Thought Partner & [Your Name]");

