#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/timer.h>

#define NUM_DESC    64
#define L3_OWN_CPU  1

struct l3_packet {
	u32 status;
	struct sk_buff *skb;
};

struct l3_napi_adapter {
	struct napi_struct napi;
	struct net_device *netdev;
	spinlock_t lock;

	/* Descriptor Rings */
	struct l3_packet rx_ring[NUM_DESC];
	struct l3_packet tx_ring[NUM_DESC];
	
	u32 cur_rx, dirty_rx;
	u32 cur_tx, dirty_tx;

	struct timer_list irq_timer;
};

/* --- Simulated Interrupt (The Hardware Trigger) --- */
static void l3_fake_irq_handler(struct timer_list *t)
{
	struct l3_napi_adapter *priv = from_timer(priv, t, irq_timer);

	/* Trigger NAPI to process the rings in SoftIRQ context */
	if (napi_schedule_prep(&priv->napi)) {
		__napi_schedule(&priv->napi);
	}
}

/* --- NAPI Poll: Standard Kernel 6.12 implementation --- */
static int l3_napi_poll(struct napi_struct *napi, int budget)
{
	struct l3_napi_adapter *priv = container_of(napi, struct l3_napi_adapter, napi);
	struct net_device *dev = priv->netdev;
	int work_done = 0;
	u32 entry;

	/* 1. Process RX (The looped-back packets) */
	while (work_done < budget) {
		entry = priv->cur_rx % NUM_DESC;
		if (priv->rx_ring[entry].status != L3_OWN_CPU)
			break;

		struct sk_buff *skb = priv->rx_ring[entry].skb;
		priv->rx_ring[entry].skb = NULL;
		priv->rx_ring[entry].status = 0;

		/* Push to stack */
		skb->protocol = eth_type_trans(skb, dev);
		napi_gro_receive(napi, skb);

		dev->stats.rx_packets++;
		dev->stats.rx_bytes += skb->len;
		
		priv->cur_rx++;
		work_done++;
	}

	/* 2. Process TX Completions (Cleaning the original buffers) */
	while (priv->dirty_tx != priv->cur_tx) {
		entry = priv->dirty_tx % NUM_DESC;
		if (priv->tx_ring[entry].status != L3_OWN_CPU)
			break;

		if (priv->tx_ring[entry].skb) {
			struct sk_buff *skb = priv->tx_ring[entry].skb;
			dev->stats.tx_packets++;
			dev->stats.tx_bytes += skb->len;
			
			dev_consume_skb_any(skb);
			priv->tx_ring[entry].skb = NULL;
		}
		
		priv->tx_ring[entry].status = 0;
		priv->dirty_tx++;
	}

	/* Wake queue if we have space again */
	if (netif_queue_stopped(dev))
		netif_wake_queue(dev);

	if (work_done < budget) {
		napi_complete_done(napi, work_done);
	}

	return work_done;
}

/* --- Transmit Level: Loopback logic --- */
static netdev_tx_t l3_napi_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct l3_napi_adapter *priv = netdev_priv(dev);
	u32 tx_entry, rx_entry;
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);

	tx_entry = priv->cur_tx % NUM_DESC;
	rx_entry = priv->cur_rx % NUM_DESC;

	/* If TX ring is full, stop the stack */
	if (priv->tx_ring[tx_entry].skb) {
		netif_stop_queue(dev);
		spin_unlock_irqrestore(&priv->lock, flags);
		return NETDEV_TX_BUSY;
	}

	/* Place original in TX ring for completion cleaning */
	priv->tx_ring[tx_entry].skb = skb;
	priv->tx_ring[tx_entry].status = L3_OWN_CPU;
	priv->cur_tx++;

	/* LOOPBACK: Clone for the RX path */
	if (!priv->rx_ring[rx_entry].skb) {
		struct sk_buff *rx_skb = skb_clone(skb, GFP_ATOMIC);
		if (rx_skb) {
			priv->rx_ring[rx_entry].skb = rx_skb;
			priv->rx_ring[rx_entry].status = L3_OWN_CPU;
		}
	}

	spin_unlock_irqrestore(&priv->lock, flags);

	/* Trigger "Hardware" IRQ after 1ms delay */
	mod_timer(&priv->irq_timer, jiffies + msecs_to_jiffies(1));

	return NETDEV_TX_OK;
}

static int l3_napi_open(struct net_device *dev)
{
	struct l3_napi_adapter *priv = netdev_priv(dev);
	napi_enable(&priv->napi);
	netif_start_queue(dev);
	return 0;
}

static int l3_napi_stop(struct net_device *dev)
{
	struct l3_napi_adapter *priv = netdev_priv(dev);
	netif_stop_queue(dev);
	napi_disable(&priv->napi);
	del_timer_sync(&priv->irq_timer);
	return 0;
}

static const struct net_device_ops l3_ops = {
	.ndo_open = l3_napi_open,
	.ndo_stop = l3_napi_stop,
	.ndo_start_xmit = l3_napi_start_xmit,
	.ndo_validate_addr = eth_validate_addr,
};

/* Callback to initialize the device structure before registration */
static void l3_setup(struct net_device *dev)
{
	struct l3_napi_adapter *priv = netdev_priv(dev);

	ether_setup(dev);
	dev->netdev_ops = &l3_ops;
	
	/* Setup private pointers */
	priv->netdev = dev;
	spin_lock_init(&priv->lock);
	
	/* Initialize Timer and NAPI *BEFORE* registration */
	timer_setup(&priv->irq_timer, l3_fake_irq_handler, 0);
	netif_napi_add_weight(dev, &priv->napi, l3_napi_poll, NAPI_POLL_WEIGHT);
}

static struct net_device *my_dev;

static int __init l3_init(void) {
	/* 1. Allocation & Setup (Setup calls netif_napi_add_weight) */
	my_dev = alloc_netdev(sizeof(struct l3_napi_adapter), "l3loop%d", NET_NAME_UNKNOWN, l3_setup);
	if (!my_dev) 
		return -ENOMEM;

	eth_hw_addr_random(my_dev);

	/* 2. Registration (Device becomes active here) */
	if (register_netdev(my_dev)) {
		free_netdev(my_dev);
		return -EIO;
	}

	pr_info("L3 NAPI Unified Simulator Loaded (Loopback Active)\n");
	return 0;
}

static void __exit l3_exit(void) {
	if (my_dev) {
		unregister_netdev(my_dev);
		free_netdev(my_dev);
	}
}

module_init(l3_init);
module_exit(l3_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Unified Loopback NAPI Driver for Presentation");

