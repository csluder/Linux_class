#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/version.h>
#include <linux/gpio.h>      /* Required for GPIO to IRQ mapping */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0)
#include <linux/gpio/consumer.h>
#endif

/*
 * 1. Configuration
 * GPIO 17 on RPi4 (Kernel 6.12) is typically base 512 + 17 = 529.
 */
static int gpio_pin = 529; 
static int irq_num;
static int irq_device_id = 101; 

/*
 * 2. Forward Declarations
 * Changed in 5.9: unsigned long -> struct tasklet_struct*
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
    void my_tasklet_handler(struct tasklet_struct *t);
#else
    void my_tasklet_handler(unsigned long data);
#endif

/*
 * 3. Tasklet Definition (Bottom Half)
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
void my_tasklet_handler(struct tasklet_struct *t) {
    pr_info("TASKLET_BH: Running via Modern API\n");
}
#else
void my_tasklet_handler(unsigned long data) {
    pr_info("TASKLET_BH: Running via Legacy API (data: %ld)\n", data);
}
#endif

/*
 * 4. Declare the Tasklet
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
    DECLARE_TASKLET(my_tasklet, my_tasklet_handler);
#else
    DECLARE_TASKLET(my_tasklet, my_tasklet_handler, 0);
#endif

/*
 * 5. ISR (Top Half)
 */
static irqreturn_t my_interrupt_handler(int irq, void *dev_id) {
    tasklet_schedule(&my_tasklet);
    return IRQ_HANDLED; 
}

/*
 * 6. Initialization
 */
static int __init my_driver_init(void) {
    int result;

    /* Map GPIO to IRQ for ARM architecture */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0)
    struct gpio_desc *desc = gpio_to_desc(gpio_pin);
    if (!desc) return -ENODEV;
    gpiod_direction_input(desc);
    irq_num = gpiod_to_irq(desc);
#else
    if (!gpio_is_valid(gpio_pin)) return -ENODEV;
    gpio_request(gpio_pin, "my_tasklet_gpio");
    gpio_direction_input(gpio_pin);
    irq_num = gpio_to_irq(gpio_pin);
#endif

    /* Use IRQF_SHARED to allow testing alongside other modules */
    result = request_irq(irq_num,
                         my_interrupt_handler,
                         IRQF_SHARED | IRQF_TRIGGER_FALLING,
                         "my_educational_tasklet",
                         &irq_device_id);

    if (result) {
        pr_err("TASKLET_EX: Failed to register IRQ %d\n", irq_num);
        return result;
    }

    pr_info("TASKLET_EX: Loaded. Monitoring GPIO %d on IRQ %d\n", gpio_pin, irq_num);
    return 0;
}

/*
 * 7. Cleanup
 */
static void __exit my_driver_exit(void) {
    tasklet_kill(&my_tasklet);
    free_irq(irq_num, &irq_device_id);
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 0, 0)
    gpio_free(gpio_pin);
#endif
    pr_info("TASKLET_EX: Unloaded safely.\n");
}

module_init(my_driver_init);
module_exit(my_driver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Class Example");
MODULE_DESCRIPTION("RPi4 Compatible Tasklet Example");

