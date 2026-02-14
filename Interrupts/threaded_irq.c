#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/version.h>      /* Required for KERNEL_VERSION macros */

/* Include headers based on kernel version */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0)
#include <linux/gpio/consumer.h> /* Modern: Descriptor-based */
#endif
#include <linux/gpio.h>          /* Legacy: Integer-based */

/* 
 * Configuration:
 * Modern kernels (6.x) often start GPIO numbering at 512.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
    static int gpio_pin = 529;   /* RPi 4 Modern (512 base + 17) */
#else
    static int gpio_pin = 17;    /* Legacy standard */
#endif

static int irq_num;
static int my_dev_id = 0;

/* Modern kernels use gpio_desc, older kernels use integers */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0)
static struct gpio_desc *my_desc;
#endif

/* --- Handlers remain the same for both eras --- */
static irqreturn_t my_top_half(int irq, void *data) {
    return IRQ_WAKE_THREAD;
}

static irqreturn_t my_threaded_handler(int irq, void *data) {
    pr_info("THREADED_IRQ: Interrupt triggered on IRQ %d!\n", irq);
    msleep(100); 
    return IRQ_HANDLED;
}

/* --- Initialization with Version Logic --- */
static int __init my_threaded_irq_init(void) {
    int result;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0)
    /* 
     * THE MODERN WAY (Kernel 5.0+)
     * We use descriptors for better type safety and hardware abstraction.
     */
    pr_info("THREADED_IRQ: Initializing via MODERN API\n");
    my_desc = gpio_to_desc(gpio_pin);
    if (!my_desc) {
        pr_err("THREADED_IRQ: Failed to get descriptor for %d\n", gpio_pin);
        return -ENODEV;
    }
    gpiod_direction_input(my_desc);
    irq_num = gpiod_to_irq(my_desc);
#else
    /* 
     * THE OLD WAY (Legacy)
     * Direct integer manipulation. Simpler, but prone to numbering conflicts.
     */
    pr_info("THREADED_IRQ: Initializing via LEGACY API\n");
    if (!gpio_is_valid(gpio_pin)) return -ENODEV;
    gpio_request(gpio_pin, "my_legacy_irq");
    gpio_direction_input(gpio_pin);
    irq_num = gpio_to_irq(gpio_pin);
#endif

    if (irq_num < 0) {
        pr_err("THREADED_IRQ: IRQ mapping failed: %d\n", irq_num);
        return irq_num;
    }

    result = request_threaded_irq(irq_num, my_top_half, my_threaded_handler, 
                                 IRQF_TRIGGER_FALLING, "my_threaded_dev", &my_dev_id);
    
    if (result) {
        pr_err("THREADED_IRQ: Request failed: %d\n", result);
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 0, 0)
        gpio_free(gpio_pin);
#endif
        return result;
    }

    pr_info("THREADED_IRQ: Loaded. Monitoring GPIO %d on IRQ %d\n", gpio_pin, irq_num);
    return 0;
}

static void __exit my_threaded_irq_exit(void) {
    free_irq(irq_num, &my_dev_id);
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 0, 0)
    gpio_free(gpio_pin); /* Modern gpiod doesn't strictly require this on exit */
#endif
    pr_info("THREADED_IRQ: Unloaded.\n");
}

module_init(my_threaded_irq_init);
module_exit(my_threaded_irq_exit);

MODULE_LICENSE("GPL");

