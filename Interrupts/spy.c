#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/fwnode.h>
#include <linux/of.h>
#include <linux/workqueue.h>

static int my_dev_id; 
static atomic_t interrupt_count = ATOMIC_INIT(0);
static int irq_num = -1;

/* 1. Define the work structure and the worker function */
static struct work_struct stats_work;

static void stats_worker_func(struct work_struct *work) {
    /* This runs in process context - safe for heavy printing */
    pr_info("SerialSpy Workqueue: Current Interrupt Count = %d\n", 
            atomic_read(&interrupt_count));
}

/* 
 * INTERRUPT HANDLER (Top Half)
 */
static irqreturn_t serial_monitor_handler(int irq, void *dev_id) {
    atomic_inc(&interrupt_count);
    
    /* 2. Schedule the work to run as soon as the CPU is free */
    schedule_work(&stats_work);
    
    return IRQ_NONE; /* Always return IRQ_NONE to pass to ttyS0 */
}

static int __init serial_spy_init(void) {
    struct device_node *dn;
    int ret;

    /* Initialize the workqueue structure */
    INIT_WORK(&stats_work, stats_worker_func);

    /* Find the ttyS0 node (serial0 alias) */
    dn = of_find_node_by_path("/aliases/serial0");
    if (!dn) dn = of_find_node_by_path("/soc/serial@7e215040");

    if (!dn) return -ENODEV;

    irq_num = fwnode_irq_get(&dn->fwnode, 0);
    of_node_put(dn);

    if (irq_num <= 0) return -EINVAL;

    ret = request_irq(irq_num, 
                      serial_monitor_handler, 
                      IRQF_SHARED, 
                      "serial_spy_monitor", 
                      &my_dev_id);

    if (ret) {
        pr_err("SerialSpy: IRQ %d request failed: %d\n", irq_num, ret);
        return ret;
    }

    pr_info("SerialSpy: Monitoring ttyS0. Workqueue initialized.\n");
    return 0;
}

static void __exit serial_spy_exit(void) {
    if (irq_num > 0) {
        free_irq(irq_num, &my_dev_id);
    }
    
    /* 3. Ensure all pending work is finished before unloading */
    cancel_work_sync(&stats_work);
    
    pr_info("SerialSpy: Final count: %d\n", atomic_read(&interrupt_count));
}

module_init(serial_spy_init);
module_exit(serial_spy_exit);
MODULE_LICENSE("GPL");

