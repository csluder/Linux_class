#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/ktime.h>
#include <linux/percpu.h>

#define TARGET_OFFSET 0xf4 // Ensure this matches your objdump offset

/* Per-CPU variable to store start time in nanoseconds */
static DEFINE_PER_CPU(ktime_t, start_time);

static struct kprobe kp = {
    .symbol_name = "ht16k33_work_handler",
    .offset = TARGET_OFFSET,
};

static int handler_pre(struct kprobe* p, struct pt_regs* regs) {
  
    pr_info("[DEBUG] xxOffset 0x%x REG[0] = 0x%llx REG[2] = %llx\n",
        TARGET_OFFSET, reg[0], reg[2]);
    /* Get the most precise monotonic time available */
    __this_cpu_write(start_time, ktime_get());
    return 0;
}

static void handler_post(struct kprobe* p, struct pt_regs* regs, unsigned long flags) {
    ktime_t end = ktime_get();
    ktime_t start = __this_cpu_read(start_time);

    /* Calculate delta in nanoseconds */
    s64 delta_ns = ktime_to_ns(ktime_sub(end, start));

    pr_info("[DEBUG] Offset 0x%x execution time: %lld ns\n",
        TARGET_OFFSET, delta_ns);
}

static int __init debug_init(void) {
    kp.pre_handler = handler_pre;
    kp.post_handler = handler_post;

    if (register_kprobe(&kp) < 0) {
        pr_err("[DEBUG] register_kprobe failed\n");
        return -1;
    }

    pr_info("[DEBUG] Nano-timing probe attached to 0x%x\n", TARGET_OFFSET);
    return 0;
}

static void __exit debug_exit(void) {
    unregister_kprobe(&kp);
    pr_info("[DEBUG] Timing probe removed\n");
}

module_init(debug_init);
module_exit(debug_exit);
MODULE_LICENSE("GPL");
