#!/usr/bin/python3

from bcc import BPF

# 1. The BPF C program(Instrumentation)
bpf_text = """
#include <linux/i2c.h>

struct data_t {
    u64 start_ns;
};

BPF_HASH(start_map, u32, struct data_t);

int probe_i2c_start(struct pt_regs* ctx, struct i2c_client* client) {
    u32 tid = bpf_get_current_pid_tgid();
    struct data_t data = {};

    // Filter for address 0x70
    if (client->addr == 0x70) {
        data.start_ns = bpf_ktime_get_ns();
        start_map.update(&tid, &data);
    }
    return 0;
}

int probe_i2c_return(struct pt_regs* ctx) {
    u32 tid = bpf_get_current_pid_tgid();
    struct data_t* datap;

    datap = start_map.lookup(&tid);
    if (datap != 0) {
        u64 delta = bpf_ktime_get_ns() - datap->start_ns;
        bpf_trace_printk("I2C Write to 0x70 took %llu ns\\n", delta);
        start_map.delete(&tid);
    }
    return 0;
}
"""

# 2. The Python Control Logic(User - space)
b = BPF(text = bpf_text)
# Attach to the entry and return of the function
b.attach_kprobe(event = "i2c_transfer_buffer_flags", fn_name = "probe_i2c_start")
b.attach_kretprobe(event = "i2c_transfer_buffer_flags", fn_name = "probe_i2c_return")

print("Tracing I2C clock updates... Hit Ctrl-C to end.")

# 3. Print the output from the kernel's trace buffer
try :
    b.trace_print()
    except KeyboardInterrupt :
exit()
