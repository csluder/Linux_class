#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0)
#include <linux/gpio/consumer.h>
#endif

/* Configuration for Raspberry Pi 4 (Kernel 6.x) */
static int gpio_pin = 529; // GPIO 17 (512 base + 17)
static int irq_num;

/* Structure to maintain device state and child work items */
struct my_device_data {
    int irq_count;
    struct work_struct background_work;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0)
    struct gpio_desc *desc;
#endif
};

static struct my_device_data my_data;

/* 
 * TIER 3: WORKQUEUE FUNCTION (The "Slow" Bottom Half)
 * Context: System Worker Thread (Process Context)
 * Best for: Logging to disk, complex calculations, or network I/O.
 */
static void my_background_work_func(struct work_struct *work) {
    /* Retrieve the container structure using the pointer to the work member */
    struct my_device_data *data = container_of(work, struct my_device_data, background_work);
    
    pr_info("HYBRID_EX: [Tier 3] Workqueue logging event #%d\n", data->irq_count);
    msleep(500); // Simulate long-running task
}

/* 
 * TIER 2: THREADED HANDLER (The "Fast" Bottom Half)
 * Context: Dedicated Kernel Thread (Process Context)
 * Best for: Mutex-heavy logic or data copying that must happen before the next IRQ.
 */
static irqreturn_t my_threaded_handler(int irq, void *id) {
    struct my_device_data *data = (struct my_device_data *)id;
    
    data->irq_count++;
    pr_info("HYBRID_EX: [Tier 2] Threaded IRQ processing event #%d\n", data->irq_count);

    /* Delegate non-urgent, heavy tasks to Tier 3 */
    schedule_work(&data->background_work);

    return IRQ_HANDLED;
}

/* 
 * TIER 1: TOP HALF (The Hard IRQ)
 * Context: Atomic / Interrupt Context
 * Best for: Hardware acknowledgment only.
 */
static irqreturn_t my_top_half(int irq, void *id) {
    return IRQ_WAKE_THREAD; 
}

static int __init my_hybrid_init(void) {
    int ret;

    /* Initialize the work metadata */
    INIT_WORK(&my_data.background_work, my_background_work_func);

    /* GPIO Setup for Pi 4 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0)
    my_data.desc = gpio_to_desc(gpio_pin);
    if (!my_data.desc) return -ENODEV;
    gpiod_direction_input(my_data.desc);
    irq_num = gpiod_to_irq(my_data.desc);
#else
    if (!gpio_is_valid(gpio_pin)) return -ENODEV;
    gpio_request(gpio_pin, "hybrid_gpio");
    gpio_direction_input(gpio_pin);
    irq_num = gpio_to_irq(gpio_pin);
#endif

    /* Register Threaded IRQ */
    ret = request_threaded_irq(irq_num, my_top_half, my_threaded_handler, 
                               IRQF_TRIGGER_FALLING, "hybrid_device", &my_data);
    if (ret) {
        pr_err("HYBRID_EX: Failed to register IRQ %d\n", irq_num);
        return ret;
    }

    pr_info("HYBRID_EX: Module Loaded. Monitoring GPIO %d on IRQ %d\n", gpio_pin, irq_num);
    return 0;
}

static void __exit my_hybrid_exit(void) {
    free_irq(irq_num, &my_data);
    
    /* 
     * IMPORTANT: cancel_work_sync() must be called AFTER free_irq().
     * This ensures the ISR can't schedule NEW work while we are 
     * waiting for OLD work to finish.
     */
    cancel_work_sync(&my_data.background_work);
    
    pr_info("HYBRID_EX: Module Unloaded. Total IRQs: %d\n", my_data.irq_count);
}

module_init(my_hybrid_init);
module_exit(my_hybrid_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Three-Tier Hybrid IRQ Example for Classroom");

