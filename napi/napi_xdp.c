/*
 * napi_xdp.c - Custom Network Driver with XDP Support
 *
 * EDUCATIONAL PURPOSE:
 * This kernel module demonstrates how to build a network device driver that:
 * 1. Implements NAPI (New API) for efficient packet processing
 * 2. Supports XDP (eXpress Data Path) for high-performance packet filtering
 * 3. Handles XDP redirect operations manually (ndo_xdp_xmit)
 * 4. Uses ring buffers to simulate hardware DMA descriptors
 * 5. Implements a timer-based interrupt simulation
 *
 * ARCHITECTURE:
 * This driver creates a virtual "l3loop0" device that acts as a software router.
 * It receives packets via XDP redirect (ndo_xdp_xmit), runs an XDP program on them
 * to make routing decisions, and manually redirects them to target interfaces.
 *
 * KEY CONCEPTS DEMONSTRATED:
 * - NAPI polling for efficient packet processing
 * - XDP program attachment and execution
 * - Ring buffer management (RX/TX rings)
 * - Memory barriers and synchronization
 * - XDP frame handling and conversion
 * - Manual XDP redirect implementation
 *
 * PACKET FLOW:
 * 1. Packet arrives via ndo_xdp_xmit() from another device
 * 2. Packet data copied to page-based buffer in RX ring
 * 3. Timer triggers NAPI poll
 * 4. NAPI poll processes RX ring, runs XDP program
 * 5. XDP program returns routing decision (PASS or REDIRECT)
 * 6. If REDIRECT: manually call target device's ndo_xdp_xmit()
 * 7. If PASS: convert to skb and deliver to network stack
 */

#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/bpf.h>
#include <linux/filter.h>
#include <net/xdp.h>
#include <linux/ip.h>
#include <linux/if_arp.h>

 /* Ring buffer configuration */
#define NUM_DESC    64                  /* Number of descriptors in each ring */
#define L3_OWN_CPU  1                   /* Descriptor owned by CPU (ready to process) */
#define XDP_PACKET_HEADROOM 256         /* Headroom before packet data for XDP */

/*
 * Packet Descriptor Structure
 *
 * Represents a single entry in the RX or TX ring buffer.
 * Simulates a hardware DMA descriptor that would be used in a real NIC.
 *
 * Fields:
 * - status: Ownership flag (0=free, L3_OWN_CPU=ready for processing)
 * - skb: Socket buffer (for normal network stack packets)
 * - xdpf: XDP frame (for XDP-redirected packets, currently unused)
 * - page: Page containing packet data (used for XDP processing)
 * - data_len: Length of packet data in bytes
 * - data_offset: Offset from page start to packet data (for headroom)
 */
struct l3_packet {
	u32 status;
	struct sk_buff* skb;
	struct xdp_frame* xdpf;
	struct page* page;
	u32 data_len;
	u32 data_offset;
};

/*
 * Device Private Data Structure
 *
 * Contains all per-device state for the l3loop driver.
 * This is allocated when the device is created and freed when destroyed.
 *
 * Fields:
 * - napi: NAPI structure for efficient polling
 * - netdev: Pointer to the network device structure
 * - lock: Spinlock for protecting ring buffer access
 * - xdp_prog: Attached XDP program (if any)
 * - xdp_rxq: XDP RX queue info (required for XDP)
 * - rx_ring/tx_ring: Ring buffers for packet descriptors
 * - cur_rx/dirty_rx: RX ring producer/consumer indices
 * - cur_tx/dirty_tx: TX ring producer/consumer indices
 * - irq_timer: Timer to simulate hardware interrupts
 */
struct l3_napi_adapter {
	struct napi_struct napi;
	struct net_device* netdev;
	spinlock_t lock;
	struct bpf_prog* xdp_prog;
	struct xdp_rxq_info xdp_rxq;

	struct l3_packet rx_ring[NUM_DESC];
	struct l3_packet tx_ring[NUM_DESC];

	u32 cur_rx, dirty_rx;      /* RX ring: cur_rx=next to fill, dirty_rx=next to process */
	u32 cur_tx, dirty_tx;      /* TX ring: cur_tx=next to fill, dirty_tx=next to complete */

	struct timer_list irq_timer;
};

/*
 * ARP Packet Structure
 *
 * Defines the layout of an ARP packet for parsing.
 * Used to extract the target IP address for routing decisions.
 */
struct arp_packet {
	struct arphdr arp;
	unsigned char ar_sha[ETH_ALEN];  /* Sender hardware address */
	unsigned char ar_sip[4];         /* Sender IP address */
	unsigned char ar_tha[ETH_ALEN];  /* Target hardware address */
	unsigned char ar_tip[4];         /* Target IP address */
} __packed;

/*
 * Get Redirect Target Interface Index
 *
 * PURPOSE:
 * Parses packet headers to determine the destination IP address,
 * then looks up the corresponding network interface to redirect to.
 *
 * This implements the routing logic:
 * - 10.0.0.1 ? v-cbr (client interface)
 * - 10.0.0.2 ? v-lbr (listener interface)
 *
 * PARAMETERS:
 * @priv: Device private data
 * @data: Pointer to start of packet data
 * @data_end: Pointer to end of packet data
 *
 * RETURNS:
 * - Positive value: Interface index to redirect to
 * - Negative value: No route found
 *
 * PACKET TYPES HANDLED:
 * - ARP: Routes based on Target Protocol Address (TPA)
 * - IPv4: Routes based on destination IP address
 */
static int l3_get_redirect_ifindex(struct l3_napi_adapter* priv, void* data, void* data_end)
{
	struct ethhdr* eth = data;
	__be32 dest_ip = 0;
	struct net_device* target_dev = NULL;
	int ifindex = -1;

	/* Validate Ethernet header */
	if ((void*)(eth + 1) > data_end)
		return -1;

	/*
	 * ARP PACKET PROCESSING
	 * Extract target IP from ARP packet
	 */
	if (eth->h_proto == htons(ETH_P_ARP)) {
		struct arp_packet* arp = (void*)(eth + 1);
		if ((void*)(arp + 1) > data_end)
			return -1;

		/* Copy target IP address (4 bytes) */
		memcpy(&dest_ip, arp->ar_tip, 4);
	}
	/*
	 * IPv4 PACKET PROCESSING
	 * Extract destination IP from IP header
	 */
	else if (eth->h_proto == htons(ETH_P_IP)) {
		struct iphdr* iph = (void*)(eth + 1);
		if ((void*)(iph + 1) > data_end)
			return -1;

		dest_ip = iph->daddr;
	}
	else {
		/* Unknown protocol, no routing */
		return -1;
	}

	/*
	 * ROUTING TABLE LOOKUP
	 * Dynamically look up target interface by name
	 * This allows the driver to work even if interfaces are created
	 * after the module is loaded
	 */
	rcu_read_lock();

	/* Route to client (10.0.0.1) */
	if (dest_ip == htonl(0x0A000001)) {
		target_dev = dev_get_by_name_rcu(dev_net(priv->netdev), "v-cbr");
		if (target_dev) {
			ifindex = target_dev->ifindex;
		}
	}
	/* Route to listener (10.0.0.2) */
	else if (dest_ip == htonl(0x0A000002)) {
		target_dev = dev_get_by_name_rcu(dev_net(priv->netdev), "v-lbr");
		if (target_dev) {
			ifindex = target_dev->ifindex;
		}
	}

	rcu_read_unlock();

	return ifindex;
}

/*
 * NAPI Poll Function
 *
 * PURPOSE:
 * This is the heart of the driver's packet processing.
 * Called by the kernel when NAPI is scheduled (after timer fires).
 * Processes packets from the RX ring and completes TX operations.
 *
 * NAPI BENEFITS:
 * - Batches packet processing for efficiency
 * - Reduces interrupt overhead
 * - Provides backpressure under load
 *
 * PARAMETERS:
 * @napi: NAPI structure
 * @budget: Maximum number of packets to process in this poll
 *
 * RETURNS:
 * Number of packets actually processed
 *
 * PROCESSING STEPS:
 * 1. Process RX ring (receive packets)
 *    - Check descriptor ownership
 *    - Run XDP program on packet
 *    - Handle XDP verdict (PASS or REDIRECT)
 * 2. Process TX ring (complete transmissions)
 *    - Free transmitted packets
 *    - Update statistics
 * 3. Re-enable interrupts if work is done
 */
static int l3_napi_poll(struct napi_struct* napi, int budget)
{
	struct l3_napi_adapter* priv = container_of(napi, struct l3_napi_adapter, napi);
	struct net_device* dev = priv->netdev;
	int work_done = 0;
	int xdp_redirects = 0;
	u32 entry;

	/*
	 * RX RING PROCESSING
	 * Process packets until budget exhausted or ring empty
	 */
	while (work_done < budget && priv->dirty_rx != priv->cur_rx) {
		entry = priv->dirty_rx % NUM_DESC;

		/* Check if descriptor is ready (owned by CPU) */
		if (READ_ONCE(priv->rx_ring[entry].status) != L3_OWN_CPU) {
			break;  /* No more packets ready */
		}

		/* Memory barrier: Ensure status read before data access */
		rmb();

		struct page* page = priv->rx_ring[entry].page;
		u32 data_len = priv->rx_ring[entry].data_len;
		u32 data_offset = priv->rx_ring[entry].data_offset;

		if (page) {
			/* Clear descriptor (mark as processed) */
			priv->rx_ring[entry].page = NULL;
			priv->rx_ring[entry].data_len = 0;
			priv->rx_ring[entry].data_offset = 0;
			priv->rx_ring[entry].status = 0;

			/*
			 * XDP PROGRAM EXECUTION
			 * Build xdp_buff and run attached XDP program
			 */
			struct xdp_buff xdp;
			struct bpf_prog* prog;
			u32 act;

			rcu_read_lock();
			prog = READ_ONCE(priv->xdp_prog);

			if (!prog) {
				/* No XDP program attached, pass to stack */
				rcu_read_unlock();
				__free_page(page);
				goto next_rx;
			}

			/* Build xdp_buff structure for XDP program */
			void* data = page_address(page) + data_offset;

			xdp.data_hard_start = page_address(page);  /* Start of buffer */
			xdp.data = data;                            /* Start of packet */
			xdp.data_end = data + data_len;            /* End of packet */
			xdp.data_meta = data;                       /* Metadata (unused) */
			xdp.rxq = &priv->xdp_rxq;                  /* RX queue info */
			xdp.frame_sz = PAGE_SIZE;                   /* Total buffer size */

			/* Run XDP program */
			act = bpf_prog_run_xdp(prog, &xdp);
			rcu_read_unlock();

			/*
			 * HANDLE XDP VERDICT
			 */
			if (act == XDP_REDIRECT) {
				/*
				 * XDP_REDIRECT: Forward packet to another interface
				 *
				 * We cannot use xdp_do_redirect() here because we're not
				 * in the original driver RX path. Instead, we manually:
				 * 1. Determine target interface from packet headers
				 * 2. Convert xdp_buff to xdp_frame
				 * 3. Call target device's ndo_xdp_xmit()
				 */
				int target_ifindex = l3_get_redirect_ifindex(priv, xdp.data, xdp.data_end);

				if (target_ifindex > 0) {
					struct net_device* target_dev;
					struct xdp_frame* xdpf;
					int err;

					/* Convert xdp_buff to xdp_frame for transmission */
					xdpf = xdp_convert_buff_to_frame(&xdp);
					if (!xdpf) {
						__free_page(page);
						goto next_rx;
					}

					/* Look up target device and call its ndo_xdp_xmit */
					rcu_read_lock();
					target_dev = dev_get_by_index_rcu(dev_net(dev), target_ifindex);

					if (!target_dev || !target_dev->netdev_ops->ndo_xdp_xmit) {
						rcu_read_unlock();
						xdp_return_frame(xdpf);
						goto next_rx;
					}

					/* Perform the redirect */
					err = target_dev->netdev_ops->ndo_xdp_xmit(target_dev, 1, &xdpf, 0);
					rcu_read_unlock();

					if (err <= 0) {
						/* Redirect failed, return frame to pool */
						xdp_return_frame(xdpf);
					}
					else {
						xdp_redirects++;
					}
				}
				else {
					/* No route found, drop packet */
					__free_page(page);
				}
			}
			else if (act == XDP_PASS) {
				/*
				 * XDP_PASS: Send packet to normal network stack
				 *
				 * Convert packet data to sk_buff and deliver via NAPI
				 */
				struct sk_buff* skb = netdev_alloc_skb(dev, data_len);
				if (skb) {
					void* pkt_data = page_address(page) + (xdp.data - xdp.data_hard_start);
					skb_copy_to_linear_data(skb, pkt_data, xdp.data_end - xdp.data);
					skb_put(skb, xdp.data_end - xdp.data);
					skb->protocol = eth_type_trans(skb, dev);

					/* Deliver to network stack via NAPI */
					napi_gro_receive(napi, skb);

					/* Update statistics */
					dev->stats.rx_packets++;
					dev->stats.rx_bytes += data_len;
				}
				__free_page(page);
			}
			else {
				/*
				 * XDP_DROP or other action: Drop packet
				 */
				__free_page(page);
			}
		}
		else {
			/* NULL page pointer - shouldn't happen */
			priv->rx_ring[entry].status = 0;
		}

	next_rx:
		priv->dirty_rx++;
		work_done++;
	}

	/*
	 * TX RING PROCESSING
	 * Complete transmitted packets and free resources
	 */
	while (priv->dirty_tx != priv->cur_tx) {
		entry = priv->dirty_tx % NUM_DESC;

		/* Check if TX completion is ready */
		if (READ_ONCE(priv->tx_ring[entry].status) != L3_OWN_CPU)
			break;

		rmb();

		if (priv->tx_ring[entry].skb) {
			struct sk_buff* skb = priv->tx_ring[entry].skb;

			/* Update statistics */
			dev->stats.tx_packets++;
			dev->stats.tx_bytes += skb->len;

			/* Free the sk_buff */
			dev_consume_skb_any(skb);
			priv->tx_ring[entry].skb = NULL;
		}

		priv->tx_ring[entry].status = 0;
		priv->dirty_tx++;
	}

	/* Wake TX queue if it was stopped */
	if (netif_queue_stopped(dev))
		netif_wake_queue(dev);

	/*
	 * NAPI COMPLETION
	 * If we processed fewer packets than budget, we're done.
	 * Tell NAPI to stop polling and re-enable interrupts.
	 */
	if (work_done < budget)
		napi_complete_done(napi, work_done);

	return work_done;
}

/*
 * ndo_xdp_xmit - Receive XDP-redirected packets from other devices
 *
 * PURPOSE:
 * This function is called when another device redirects packets to us
 * using XDP redirect. It's the entry point for XDP-redirected traffic.
 *
 * OPERATION:
 * 1. Allocate page for each frame
 * 2. Copy frame data to page (with headroom for XDP)
 * 3. Place page in RX ring
 * 4. Schedule NAPI to process the packets
 *
 * PARAMETERS:
 * @dev: Our network device
 * @n: Number of frames being transmitted
 * @frames: Array of XDP frame pointers
 * @flags: Transmission flags
 *
 * RETURNS:
 * Number of frames successfully queued (or negative error code)
 *
 * NOTE:
 * We copy the frame data instead of taking ownership of the frames
 * because we need to return them to the sender's memory pool.
 */
static int l3_ndo_xdp_xmit(struct net_device* dev, int n, struct xdp_frame** frames, u32 flags)
{
	struct l3_napi_adapter* priv = netdev_priv(dev);
	int nxmit = 0;
	int i;

	/* Validate flags */
	if (unlikely(flags & ~XDP_XMIT_FLAGS_MASK))
		return -EINVAL;

	/* Check if device is running */
	if (unlikely(!netif_running(dev)))
		return -ENETDOWN;

	spin_lock_bh(&priv->lock);

	/* Process each frame */
	for (i = 0; i < n; i++) {
		struct xdp_frame* xdpf = frames[i];
		u32 entry;
		struct page* page;
		void* data;

		entry = priv->cur_rx % NUM_DESC;

		/* Check if RX ring has space */
		if (priv->rx_ring[entry].status == L3_OWN_CPU ||
			priv->rx_ring[entry].xdpf != NULL ||
			priv->rx_ring[entry].page != NULL) {
			break;  /* Ring full */
		}

		/*
		 * ALLOCATE PAGE AND COPY DATA
		 * We allocate a full page to ensure proper alignment
		 * and headroom for XDP program modifications
		 */
		page = alloc_page(GFP_ATOMIC);
		if (!page) {
			break;  /* Out of memory */
		}

		/* Copy frame data with headroom */
		data = page_address(page) + XDP_PACKET_HEADROOM;
		memcpy(data, xdpf->data, xdpf->len);

		/* Place in RX ring */
		priv->rx_ring[entry].page = page;
		priv->rx_ring[entry].data_len = xdpf->len;
		priv->rx_ring[entry].data_offset = XDP_PACKET_HEADROOM;

		/* Memory barrier: Ensure data written before status update */
		smp_wmb();

		/* Mark descriptor as ready for processing */
		WRITE_ONCE(priv->rx_ring[entry].status, L3_OWN_CPU);
		priv->cur_rx++;

		/* Return original frame to sender's pool */
		xdp_return_frame(xdpf);
		nxmit++;
	}

	spin_unlock_bh(&priv->lock);

	/* Return any frames we couldn't queue */
	for (; i < n; i++)
		xdp_return_frame(frames[i]);

	/* Schedule NAPI to process the received frames */
	if (nxmit > 0) {
		napi_schedule(&priv->napi);
	}

	return nxmit;
}

/*
 * XDP Program Setup
 *
 * PURPOSE:
 * Attach or detach an XDP program to this device.
 * Called when userspace loads/unloads an XDP program.
 *
 * PARAMETERS:
 * @dev: Network device
 * @bpf: BPF program information
 *
 * OPERATION:
 * - Increments program reference count if attaching
 * - Atomically swaps old and new programs
 * - Decrements old program reference count if detaching
 */
static int l3_xdp_setup(struct net_device* dev, struct netdev_bpf* bpf)
{
	struct l3_napi_adapter* priv = netdev_priv(dev);
	struct bpf_prog* old_prog, * new_prog = bpf->prog;

	/* Increment reference count for new program */
	if (new_prog)
		bpf_prog_add(new_prog, 1);

	/* Atomically swap programs */
	old_prog = xchg(&priv->xdp_prog, new_prog);

	/* Decrement reference count for old program */
	if (old_prog)
		bpf_prog_put(old_prog);

	return 0;
}

/*
 * ndo_bpf - Handle BPF-related operations
 *
 * PURPOSE:
 * Entry point for BPF operations on this device.
 * Currently only supports XDP program setup.
 */
static int l3_ndo_bpf(struct net_device* dev, struct netdev_bpf* bpf)
{
	switch (bpf->command) {
	case XDP_SETUP_PROG:
		return l3_xdp_setup(dev, bpf);
	default:
		return -EINVAL;
	}
}

/*
 * Fake IRQ Handler (Timer Callback)
 *
 * PURPOSE:
 * Simulates hardware interrupts using a kernel timer.
 * In a real driver, this would be a hardware interrupt handler.
 *
 * OPERATION:
 * Schedules NAPI polling when timer fires.
 * NAPI will then process any pending packets.
 */
static void l3_fake_irq_handler(struct timer_list* t)
{
	struct l3_napi_adapter* priv = from_timer(priv, t, irq_timer);

	/* Schedule NAPI if not already scheduled */
	if (napi_schedule_prep(&priv->napi))
		__napi_schedule(&priv->napi);
}

/*
 * ndo_start_xmit - Transmit a packet
 *
 * PURPOSE:
 * Called by the network stack to transmit a packet.
 * In this loopback driver, we also clone the packet to RX ring
 * to demonstrate the loopback functionality.
 *
 * PARAMETERS:
 * @skb: Socket buffer containing packet to transmit
 * @dev: Network device
 *
 * RETURNS:
 * NETDEV_TX_OK on success, NETDEV_TX_BUSY if queue full
 *
 * OPERATION:
 * 1. Place skb in TX ring (for completion tracking)
 * 2. Copy packet data to RX ring (loopback)
 * 3. Schedule timer to trigger NAPI processing
 */
static netdev_tx_t l3_napi_start_xmit(struct sk_buff* skb, struct net_device* dev)
{
	struct l3_napi_adapter* priv = netdev_priv(dev);
	u32 tx_entry, rx_entry;
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);

	tx_entry = priv->cur_tx % NUM_DESC;
	rx_entry = priv->cur_rx % NUM_DESC;

	/* Check if TX ring has space */
	if (priv->tx_ring[tx_entry].skb) {
		netif_stop_queue(dev);
		spin_unlock_irqrestore(&priv->lock, flags);
		return NETDEV_TX_BUSY;
	}

	/* Place in TX ring for completion tracking */
	priv->tx_ring[tx_entry].skb = skb;
	wmb();
	priv->tx_ring[tx_entry].status = L3_OWN_CPU;
	priv->cur_tx++;

	/*
	 * LOOPBACK FUNCTIONALITY
	 * Copy packet to RX ring so it can be processed by XDP program
	 * This demonstrates how packets enter the RX path
	 */
	if (priv->rx_ring[rx_entry].status != L3_OWN_CPU &&
		!priv->rx_ring[rx_entry].xdpf &&
		!priv->rx_ring[rx_entry].page) {

		struct page* page = alloc_page(GFP_ATOMIC);
		if (page) {
			void* data = page_address(page) + XDP_PACKET_HEADROOM;
			u32 copy_len = min_t(u32, skb->len, PAGE_SIZE - XDP_PACKET_HEADROOM);

			/* Copy packet data to page */
			skb_copy_bits(skb, 0, data, copy_len);

			/* Place in RX ring */
			priv->rx_ring[rx_entry].page = page;
			priv->rx_ring[rx_entry].data_len = copy_len;
			priv->rx_ring[rx_entry].data_offset = XDP_PACKET_HEADROOM;
			wmb();
			priv->rx_ring[rx_entry].status = L3_OWN_CPU;
			priv->cur_rx++;
		}
	}

	spin_unlock_irqrestore(&priv->lock, flags);

	/* Schedule timer to trigger NAPI (simulates hardware IRQ) */
	mod_timer(&priv->irq_timer, jiffies + msecs_to_jiffies(1));

	return NETDEV_TX_OK;
}

/*
 * ndo_open - Open the network device
 *
 * PURPOSE:
 * Called when device is brought up (e.g., "ip link set l3loop0 up")
 *
 * OPERATION:
 * 1. Register XDP RX queue information
 * 2. Enable NAPI polling
 * 3. Start TX queue
 */
static int l3_napi_open(struct net_device* dev) {
	struct l3_napi_adapter* priv = netdev_priv(dev);
	int err;

	/* Register XDP RX queue info (required for XDP) */
	err = xdp_rxq_info_reg(&priv->xdp_rxq, dev, 0, 0);
	if (err)
		return err;

	/* Register memory model (page-based) */
	err = xdp_rxq_info_reg_mem_model(&priv->xdp_rxq, MEM_TYPE_PAGE_SHARED, NULL);
	if (err) {
		xdp_rxq_info_unreg(&priv->xdp_rxq);
		return err;
	}

	/* Enable NAPI polling */
	napi_enable(&priv->napi);

	/* Start transmit queue */
	netif_start_queue(dev);

	return 0;
}

/*
 * ndo_stop - Close the network device
 *
 * PURPOSE:
 * Called when device is brought down (e.g., "ip link set l3loop0 down")
 *
 * OPERATION:
 * 1. Stop TX queue
 * 2. Disable NAPI
 * 3. Cancel timer
 * 4. Clean up any pending packets
 * 5. Unregister XDP info
 */
static int l3_napi_stop(struct net_device* dev) {
	struct l3_napi_adapter* priv = netdev_priv(dev);
	int i;

	/* Stop transmit queue */
	netif_stop_queue(dev);

	/* Disable NAPI polling */
	napi_disable(&priv->napi);

	/* Cancel timer */
	del_timer_sync(&priv->irq_timer);

	/*
	 * CLEANUP: Free any pending packets in rings
	 * Important to prevent memory leaks
	 */
	for (i = 0; i < NUM_DESC; i++) {
		if (priv->rx_ring[i].page) {
			__free_page(priv->rx_ring[i].page);
			priv->rx_ring[i].page = NULL;
		}
		if (priv->rx_ring[i].xdpf) {
			xdp_return_frame(priv->rx_ring[i].xdpf);
			priv->rx_ring[i].xdpf = NULL;
		}
	}

	/* Unregister XDP info */
	xdp_rxq_info_unreg(&priv->xdp_rxq);

	return 0;
}

/*
 * Network Device Operations
 *
 * This structure defines the callbacks that the kernel will use
 * to interact with our network device.
 */
static const struct net_device_ops l3_ops = {
	.ndo_open = l3_napi_open,
	.ndo_stop = l3_napi_stop,
	.ndo_start_xmit = l3_napi_start_xmit,
	.ndo_validate_addr = eth_validate_addr,
	.ndo_bpf = l3_ndo_bpf,
	.ndo_xdp_xmit = l3_ndo_xdp_xmit,
};

/*
 * Device Setup Function
 *
 * PURPOSE:
 * Initialize device structure and private data.
 * Called once when device is allocated.
 *
 * OPERATION:
 * 1. Set up as Ethernet device
 * 2. Configure device features
 * 3. Advertise XDP capabilities
 * 4. Initialize private data structures
 * 5. Register NAPI
 */
static void l3_setup(struct net_device* dev) {
	struct l3_napi_adapter* priv = netdev_priv(dev);

	/* Initialize as Ethernet device */
	ether_setup(dev);

	/* Set device operations */
	dev->netdev_ops = &l3_ops;

	/* Enable hardware checksum offload (simulated) */
	dev->features |= NETIF_F_HW_CSUM;
	dev->hw_features |= NETIF_F_HW_CSUM;

	/* Set MTU limits */
	dev->min_mtu = ETH_MIN_MTU;
	dev->max_mtu = ETH_MAX_MTU;

	/*
	 * ADVERTISE XDP CAPABILITIES
	 * Tell the kernel what XDP features we support:
	 * - BASIC: XDP_PASS, XDP_DROP, XDP_TX
	 * - REDIRECT: XDP_REDIRECT action
	 * - NDO_XMIT: Can receive redirected packets via ndo_xdp_xmit
	 */
	dev->xdp_features = NETDEV_XDP_ACT_BASIC |
		NETDEV_XDP_ACT_REDIRECT |
		NETDEV_XDP_ACT_NDO_XMIT;

	/* Initialize private data */
	priv->netdev = dev;
	spin_lock_init(&priv->lock);

	/* Set up timer for simulated interrupts */
	timer_setup(&priv->irq_timer, l3_fake_irq_handler, 0);

	/* Register NAPI with default weight (64 packets per poll) */
	netif_napi_add_weight(dev, &priv->napi, l3_napi_poll, NAPI_POLL_WEIGHT);
}

/* Global pointer to our device */
static struct net_device* my_dev;

/*
 * Module Initialization
 *
 * PURPOSE:
 * Called when module is loaded (insmod)
 *
 * OPERATION:
 * 1. Allocate network device with private data
 * 2. Assign random MAC address
 * 3. Register device with kernel
 */
static int __init l3_init(void) {
	/* Allocate network device with private data */
	my_dev = alloc_netdev(sizeof(struct l3_napi_adapter),
		"l3loop%d",           /* Name pattern */
		NET_NAME_UNKNOWN,     /* Name assignment type */
		l3_setup);            /* Setup function */
	if (!my_dev)
		return -ENOMEM;

	/* Assign random MAC address */
	eth_hw_addr_random(my_dev);

	/* Register with kernel */
	if (register_netdev(my_dev)) {
		free_netdev(my_dev);
		return -EIO;
	}

	return 0;
}

/*
 * Module Cleanup
 *
 * PURPOSE:
 * Called when module is unloaded (rmmod)
 *
 * OPERATION:
 * 1. Release XDP program reference
 * 2. Unregister device
 * 3. Free device memory
 */
static void __exit l3_exit(void) {
	if (my_dev) {
		struct l3_napi_adapter* priv = netdev_priv(my_dev);

		/* Release XDP program if attached */
		if (priv->xdp_prog)
			bpf_prog_put(priv->xdp_prog);

		/* Unregister and free device */
		unregister_netdev(my_dev);
		free_netdev(my_dev);
	}
}

/* Register module entry/exit points */
module_init(l3_init);
module_exit(l3_exit);

/* Module metadata */
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("L3 Loop Device with XDP Support - Educational Example");
MODULE_VERSION("1.0");

/*
 * LEARNING SUMMARY
 * ================
 *
 * This driver demonstrates:
 *
 * 1. NAPI POLLING:
 *    - Efficient packet processing using polling instead of interrupts
 *    - Budget-based processing to prevent CPU starvation
 *    - Proper NAPI enable/disable lifecycle
 *
 * 2. XDP INTEGRATION:
 *    - Attaching/detaching XDP programs
 *    - Running XDP programs on received packets
 *    - Handling XDP verdicts (PASS, DROP, REDIRECT)
 *    - Manual XDP redirect implementation
 *
 * 3. RING BUFFER MANAGEMENT:
 *    - Producer/consumer indices (cur/dirty)
 *    - Descriptor ownership flags
 *    - Memory barriers for synchronization
 *
 * 4. MEMORY MANAGEMENT:
 *    - Page-based packet buffers for XDP
 *    - Proper cleanup on device shutdown
 *    - Reference counting for XDP programs
 *
 * 5. DEVICE DRIVER BASICS:
 *    - Network device operations (ndo_*)
 *    - Device lifecycle (open/stop)
 *    - Statistics tracking
 *    - Feature advertisement
 *
 * KEY TAKEAWAYS:
 * - XDP provides line-rate packet processing
 * - NAPI reduces interrupt overhead
 * - Proper synchronization is critical
 * - Memory management must be careful to avoid leaks
 * - XDP redirect requires special handling in virtual devices
 */