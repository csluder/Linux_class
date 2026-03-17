#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/ktime.h>
#include <linux/i2c.h>

/* Define the structure for shared data between handlers */
struct my_probe_data {
    ktime_t entry_stamp;
};

/* 1. ENTRY HANDLER: Runs when entering the function */
static int entry_handler(struct kretprobe_instance* ri, struct pt_regs* regs)
{
    struct my_probe_data* data;
    struct i2c_client* client = (struct i2c_client*)regs->regs[0]; // x0 on ARM64

    /* Filter for our specific device */
    if (!client || client->addr != 0x70)
        return 1; // Return 1 to skip the return handler for other devices

    /* Access the automatically allocated data buffer */
    data = (struct my_probe_data*)ri->data;
    data->entry_stamp = ktime_get();
    return 0;
}

/* 2. RETURN HANDLER: Runs when the function returns */
static int ret_handler(struct kretprobe_instance* ri, struct pt_regs* regs)
{
    struct my_probe_data* data = (struct my_probe_data*)ri->data;
    s64 delta_ns;

    delta_ns = ktime_to_ns(ktime_sub(ktime_get(), data->entry_stamp));

    pr_info("[DEBUG] i2c_master_send took %lld ns\n", delta_ns);
    return 0;
}

static struct kretprobe my_kretprobe = {
    .handler = ret_handler,
    .entry_handler = entry_handler,
    /* This tells the kernel how much memory to allocate for ri->data */
    .data_size = sizeof(struct my_probe_data),
    .maxactive = 20,
    .kp.symbol_name = "i2c_transfer_buffer_flags",
};

static int __init debug_init(void)
{
    int ret = register_kretprobe(&my_kretprobe);
    if (ret < 0) {
        pr_err("[DEBUG] register_kretprobe failed: %d\n", ret);
        return ret;
    }
    return 0;
}

static void __exit debug_exit(void)
{
    unregister_kretprobe(&my_kretprobe);
}

module_init(debug_init);
module_exit(debug_exit);

/* REQUIRED: Must be exactly "GPL" for kprobes symbols */
MODULE_LICENSE("GPL");
