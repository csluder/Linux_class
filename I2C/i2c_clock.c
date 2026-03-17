#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/timer.h>
#include <linux/timekeeping.h>
#include <linux/workqueue.h>
#include <linux/mod_devicetable.h>

#define HT16K33_SYSTEM_SETUP  0x21
#define HT16K33_DISPLAY_SETUP 0x81
#define HT16K33_BRIGHTNESS    0xEF 

/* Module parameter for DST/Timezone bias (Hours from UTC) */
static int utc_offset = 0;
module_param(utc_offset, int, 0644);
MODULE_PARM_DESC(utc_offset, "UTC offset in hours (e.g. -5 for EST, -4 for EDT)");

static const u8 seg_map[] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
};

struct ht16k33_ctx {
    struct i2c_client* client;
    struct timer_list timer;
    struct work_struct work;
};

/* WORK HANDLER: Runs in process context, safe for I2C (sleeping) */
static void ht16k33_work_handler(struct work_struct* work) {
    struct ht16k33_ctx* ctx = container_of(work, struct ht16k33_ctx, work);
    struct timespec64 ts;
    struct tm tm;
    u8 buf[11] = { 0 };

    ktime_get_real_ts64(&ts);

    /* Apply the UTC offset (hours * 3600 seconds) before converting to TM */
    time64_to_tm(ts.tv_sec + (utc_offset * 3600), 0, &tm);

    buf[0] = 0x00;                           // Start RAM Address
    buf[1] = seg_map[tm.tm_hour / 10];       // Digit 0
    buf[3] = seg_map[tm.tm_hour % 10];       // Digit 1
    buf[5] = 0x02;                           // Center Colon
    buf[7] = seg_map[tm.tm_min / 10];        // Digit 2
    buf[9] = seg_map[tm.tm_min % 10];        // Digit 3

    i2c_master_send(ctx->client, buf, sizeof(buf));
}

/* TIMER CALLBACK: Runs in atomic context, schedules the work */
static void ht16k33_timer_callback(struct timer_list* t) {
    struct ht16k33_ctx* ctx = from_timer(ctx, t, timer);

    schedule_work(&ctx->work);
    mod_timer(&ctx->timer, jiffies + HZ);
}

/* Managed cleanup: stops timer and work, then clears display */
static void ht16k33_cleanup(void* data) {
    struct ht16k33_ctx* ctx = data;
    del_timer_sync(&ctx->timer);
    cancel_work_sync(&ctx->work);
    i2c_smbus_write_byte(ctx->client, 0x80);
}

static int ht16k33_probe(struct i2c_client* client) {
    struct ht16k33_ctx* ctx;
    int ret;

    ctx = devm_kzalloc(&client->dev, sizeof(*ctx), GFP_KERNEL);
    if (!ctx) return -ENOMEM;

    ctx->client = client;

    /* Initialize Hardware */
    i2c_smbus_write_byte(client, HT16K33_SYSTEM_SETUP);
    i2c_smbus_write_byte(client, HT16K33_DISPLAY_SETUP);
    i2c_smbus_write_byte(client, HT16K33_BRIGHTNESS);

    /* Initialize Sync Primitives */
    INIT_WORK(&ctx->work, ht16k33_work_handler);
    timer_setup(&ctx->timer, ht16k33_timer_callback, 0);

    /* Register Managed Cleanup */
    ret = devm_add_action_or_reset(&client->dev, ht16k33_cleanup, ctx);
    if (ret) return ret;

    /* Initial trigger */
    mod_timer(&ctx->timer, jiffies);

    dev_info(&client->dev, "HT16K33 Clock (Offset: %d) Probed\n", utc_offset);
    return 0;
}

static const struct of_device_id ht16k33_dt_ids[] = {
    {.compatible = "adafruit,ht16k33-timer" },
    { }
};
MODULE_DEVICE_TABLE(of, ht16k33_dt_ids);

static struct i2c_driver ht16k33_driver = {
    .driver = {.name = "ht16k33_timer", .of_match_table = ht16k33_dt_ids },
    .probe = ht16k33_probe,
};

module_i2c_driver(ht16k33_driver);

MODULE_AUTHOR("Charles Sluder");
MODULE_DESCRIPTION("I2C Driver with Workqueue & DST Bias");
MODULE_LICENSE("GPL");
