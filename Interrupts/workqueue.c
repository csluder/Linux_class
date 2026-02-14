#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>

/* 
 * 1. Configuration 
 * IRQ 1 is the keyboard on x86. We must use IRQF_SHARED.
 */
#define IRQ_NO 1
static int my_dev_id = 0; // Unique token for the shared IRQ line

/* 2. The Work Structure: The "Bottom Half" descriptor */
static struct work_struct my_work;

/* 
 * 3. Work Function (The Bottom Half)
 * CONTEXT: Process Context (Kernel Thread).
 * BEHAVIOR: Unlike tasklets, this CAN sleep or perform blocking I/O.
 */
static void my_work_handler(struct work_struct *work) {
    /* We can perform heavy/slow logic here without locking the system */
    pr_info("WORKQUEUE_EX: Bottom Half running in process context.\n");
}

/* 
 * 4. Interrupt Service Routine (The Top Half)
 * CONTEXT: Interrupt Context (Atomic).
 * BEHAVIOR: Must be extremely fast. It only schedules the work.
 */
static irqreturn_t my_isr(int irq, void *data) {
    /* Fixes 'unused parameter' warning */
    (void)data;

    pr_info("WORKQUEUE_EX: ISR triggered. Scheduling work...\n");
    
    /* Puts the work into the system-wide default workqueue */
    schedule_work(&my_work); 
    
    return IRQ_HANDLED;
}

/* 
 * 5. Module Initialization
 * NOTE: Renamed from 'workqueue_init' to avoid symbol collision with the kernel.
 */
static int __init my_wq_module_init(void) {
    /* Initialize the work structure with the handler */
    INIT_WORK(&my_work, my_work_handler);

    /* Request the IRQ line */
    if (request_irq(IRQ_NO, my_isr, IRQF_SHARED, "my_wq_dev", &my_dev_id)) {
        pr_err("WORKQUEUE_EX: Failed to register IRQ %d\n", IRQ_NO);
        return -EIO;
    }

    pr_info("WORKQUEUE_EX: Module Loaded. Registered IRQ %d\n", IRQ_NO);
    return 0;
}

/* 6. Module Cleanup */
static void __exit my_wq_module_exit(void) {
    /* 1. Stop the ISR from triggering */
    free_irq(IRQ_NO, &my_dev_id);

    /* 
     * 2. Synchronize: Ensure any running work finishes before 
     * the code is unloaded from memory (prevents kernel panic).
     */
    cancel_work_sync(&my_work);

    pr_info("WORKQUEUE_EX: Module Unloaded.\n");
}

module_init(my_wq_module_init);
module_exit(my_wq_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Class Example");
MODULE_DESCRIPTION("Interrupt Bottom Half using Workqueues");

