#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/ktime.h>
#include <linux/i2c.h>

/* Data structure to share between entry and return handlers */
struct my_data {
    ktime_t entry_stamp;
};

/* 1. ENTRY HANDLER: Called when i2c_master_send is entered */
static int entry_handler(struct kretprobe_instance* ri, struct pt_regs* regs)
{
    struct my_data* data;
    struct i2c_client* client = (struct i2c_client*)regs->regs[0]; // x0 on ARM64

    /* Filter for your specific HT16K33 address (0x70) */
    if (client->addr != 0x70)
        return 1; // Return 1 to skip the return probe for other devices

    data = (struct my_data*)ri->data;
    data->entry_stamp = ktime_get();
    return 0; // Return 0 to ensure the return handler is called
}

/* 2. RETURN HANDLER: Called when i2c_master_send returns */
static int ret_handler(struct kretprobe_instance* ri, struct pt_regs* regs)
{
    struct my_data* data = (struct my_data*)ri->data;
    s64 delta_ns;
    int retval = regs_return_value(regs); // Get the function's return value

    delta_ns = ktime_to_ns(ktime_sub(ktime_get(), data->entry_stamp));

    pr_info("[DEBUG] I2C Send to 0x70 completed in %lld ns (Ret: %d)\n",
        delta_ns, retval);
    return 0;
}

static struct kretprobe my_kretprobe = {
    .handler = ret_handler,
    .entry_handler = entry_handler,
    .data_size = sizeof(struct my_data), // Size of private data to allocate
    .maxactive = 20,                     // Max simultaneous probes
    .kp.symbol_name = "i2c_master_send",
};

static int __init debug_init(void)
{
    int ret = register_kretprobe(&my_kretprobe);
    if (ret < 0) {
        pr_err("[DEBUG] register_kretprobe failed, returned %d\n", ret);
        return ret;
    }
    pr_info("[DEBUG] Kretprobe attached to i2c_master_send\n");
    return 0;
}

static void __exit debug_exit(void)
{
    unregister_kretprobe(&my_kretprobe);
    pr_info("[DEBUG] Kretprobe removed\n");
}

module_init(debug_init);
module_exit(debug_exit);
MODULE_LICENSE("GPL");
