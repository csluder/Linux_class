// SPDX-License-Identifier: GPL-2.0
/*
 * i2cfw_eeprom.c — I2C framework driver for AT24C256 I2C EEPROM
 * Optimized for Raspberry Pi 4/5 (Kernels 5.x to 6.12+)
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/sysfs.h>
#include <linux/device.h>
#include <linux/version.h>
#include <linux/property.h> // For firmware-agnostic property API

/* Default fallback values if Device Tree properties are missing */
#define AT24_DEFAULT_SIZE 32768
#define AT24_DEFAULT_PAGE 64
#define AT24_WRITE_TIMEOUT_MS 5

struct at24_data {
    struct i2c_client *client;
    struct miscdevice miscdev;
    struct bin_attribute bin_attr;
    struct mutex lock;
    char *devname;
    u32 size;      /* Dynamically read from DT "size" */
    u32 pagesize;  /* Dynamically read from DT "pagesize" */
};

/* ---------- I2C Transfer Helpers ---------- */

static int at24_read_combined(struct i2c_client *client, u16 addr, u8 *buf, size_t len)
{
    u8 addrbuf[2] = { (u8)(addr >> 8), (u8)(addr & 0xFF) };
    struct i2c_msg msgs[2] = {
        { .addr = client->addr, .flags = 0,        .len = 2,   .buf = addrbuf },
        { .addr = client->addr, .flags = I2C_M_RD, .len = len, .buf = buf     },
    };
    int ret = i2c_transfer(client->adapter, msgs, 2);
    return (ret == 2) ? 0 : ((ret < 0) ? ret : -EIO);
}

static int at24_write_page_msg(struct i2c_client *client, u16 addr, const u8 *data, size_t len)
{
    struct i2c_msg msg;
    u8 *tx;
    int ret;

    tx = kmalloc(len + 2, GFP_KERNEL);
    if (!tx) return -ENOMEM;

    tx[0] = (u8)(addr >> 8);
    tx[1] = (u8)(addr & 0xFF);
    memcpy(&tx[2], data, len);

    msg.addr = client->addr; 
    msg.flags = 0; 
    msg.len = len + 2; 
    msg.buf = tx;

    ret = i2c_transfer(client->adapter, &msg, 1);
    kfree(tx);
    return (ret == 1) ? 0 : ((ret < 0) ? ret : -EIO);
}

static int at24_wait_ready(struct i2c_client *client)
{
    unsigned long deadline = jiffies + msecs_to_jiffies(AT24_WRITE_TIMEOUT_MS);
    u8 dummy = 0;
    struct i2c_msg msg = { .addr = client->addr, .flags = 0, .len = 1, .buf = &dummy };
    
    do {
        if (i2c_transfer(client->adapter, &msg, 1) == 1) return 0;
        usleep_range(1000, 1500);
    } while (time_before(jiffies, deadline));
    
    return -ETIMEDOUT;
}

/* ---------- File Operations (/dev) ---------- */

static ssize_t at24_read_file(struct file *f, char __user *ubuf, size_t count, loff_t *ppos)
{
    struct at24_data *ee = container_of(f->private_data, struct at24_data, miscdev);
    loff_t pos = *ppos;
    size_t todo;
    ssize_t done = 0;
    int ret = 0;

    if (pos >= ee->size) return 0;
    todo = min_t(size_t, count, (size_t)(ee->size - pos));
    
    mutex_lock(&ee->lock);
    while (todo > 0) {
        size_t chunk = min_t(size_t, todo, 128);
        u8 *kbuf = kmalloc(chunk, GFP_KERNEL);
        if (!kbuf) { ret = -ENOMEM; break; }

        ret = at24_read_combined(ee->client, (u16)pos, kbuf, chunk);
        if (!ret && copy_to_user(ubuf + done, kbuf, chunk)) ret = -EFAULT;
        
        kfree(kbuf);
        if (ret) break;
        pos += chunk; todo -= chunk; done += chunk;
    }
    mutex_unlock(&ee->lock);

    if (done > 0) { *ppos += done; return done; }
    return ret;
}

static ssize_t at24_write_file(struct file *f, const char __user *ubuf, size_t count, loff_t *ppos)
{
    struct at24_data *ee = container_of(f->private_data, struct at24_data, miscdev);
    loff_t pos = *ppos;
    size_t remaining;
    ssize_t written = 0;
    int ret = 0;

    if (pos >= ee->size) return -ENOSPC;
    remaining = min_t(size_t, count, (size_t)(ee->size - pos));

    mutex_lock(&ee->lock);
    while (remaining > 0) {
        size_t page_off = (size_t)(pos % ee->pagesize);
        size_t chunk = min_t(size_t, remaining, (size_t)(ee->pagesize - page_off));
        u8 *kbuf = kmalloc(chunk, GFP_KERNEL);
        
        if (!kbuf) { ret = -ENOMEM; break; }
        if (copy_from_user(kbuf, ubuf + written, chunk)) { kfree(kbuf); ret = -EFAULT; break; }

        ret = at24_write_page_msg(ee->client, (u16)pos, kbuf, chunk);
        kfree(kbuf);
        if (ret) break;

        at24_wait_ready(ee->client);
        pos += chunk; remaining -= chunk; written += chunk;
    }
    mutex_unlock(&ee->lock);

    if (written > 0) { *ppos += written; return written; }
    return ret;
}

static const struct file_operations at24_fops = {
    .owner = THIS_MODULE,
    .read = at24_read_file,
    .write = at24_write_file,
    .llseek = generic_file_llseek,
    .open = nonseekable_open,
};

/* ---------- Sysfs Operations (/sys) ---------- */

static ssize_t at24_sysfs_read(struct file *filp, struct kobject *kobj,
                               struct bin_attribute *attr, char *buf,
                               loff_t off, size_t count)
{
    struct device *dev = kobj_to_dev(kobj);
    struct i2c_client *client = to_i2c_client(dev);
    struct at24_data *ee = i2c_get_clientdata(client);
    int ret;

    if (off >= ee->size) return 0;
    if (off + count > ee->size) count = ee->size - off;

    mutex_lock(&ee->lock);
    ret = at24_read_combined(client, (u16)off, (u8 *)buf, count);
    mutex_unlock(&ee->lock);

    return ret ? ret : count;
}

/* ---------- Probe & Remove ---------- */

/**
 * at24_probe - Handles device discovery
 * NOTE: Kernel 6.3+ removed the 2nd parameter (id) from probe.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int at24_probe(struct i2c_client *client)
#else
static int at24_probe(struct i2c_client *client, const struct i2c_device_id *id)
#endif
{
    struct device *dev = &client->dev;
    struct at24_data *ee;
    int ret;

    ee = devm_kzalloc(dev, sizeof(*ee), GFP_KERNEL);
    if (!ee) return -ENOMEM;

    /* Pull properties from Device Tree entry */
    if (device_property_read_u32(dev, "size", &ee->size))
        ee->size = AT24_DEFAULT_SIZE;
    if (device_property_read_u32(dev, "pagesize", &ee->pagesize))
        ee->pagesize = AT24_DEFAULT_PAGE;

    ee->client = client;
    mutex_init(&ee->lock);
    i2c_set_clientdata(client, ee);

    /* 1. Register Binary Sysfs node (/sys/bus/i2c/devices/X-XXXX/eeprom) */
    sysfs_bin_attr_init(&ee->bin_attr);
    ee->bin_attr.attr.name = "eeprom";
    ee->bin_attr.attr.mode = 0644;
    ee->bin_attr.read = at24_sysfs_read;
    ee->bin_attr.size = ee->size;
    ret = device_create_bin_file(dev, &ee->bin_attr);
    if (ret) return ret;

    /* 2. Register Misc Device (/dev/at24c256-X-XX) */
    ee->devname = devm_kasprintf(dev, GFP_KERNEL, "at24c256-%d-%02x", 
                                 client->adapter->nr, client->addr);
    ee->miscdev.minor = MISC_DYNAMIC_MINOR;
    ee->miscdev.name = ee->devname;
    ee->miscdev.fops = &at24_fops;
    ee->miscdev.parent = dev;

    ret = misc_register(&ee->miscdev);
    if (ret) {
        device_remove_bin_file(dev, &ee->bin_attr);
        return ret;
    }

    dev_info(dev, "EEPROM bound: %u bytes, %u pgsize -> /dev/%s\n", 
             ee->size, ee->pagesize, ee->devname);
    return 0;
}

/**
 * at24_remove - Cleanup logic
 * NOTE: Kernel 6.1+ changed return type to void.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
static void at24_remove(struct i2c_client *client)
#else
static int at24_remove(struct i2c_client *client)
#endif
{
    struct at24_data *ee = i2c_get_clientdata(client);
    misc_deregister(&ee->miscdev);
    device_remove_bin_file(&client->dev, &ee->bin_attr);
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 0)
    return 0;
#endif
}

/* ---------- Infrastructure ---------- */

static const struct of_device_id at24_of_match[] = {
    { .compatible = "atmel,24c256" },
    { .compatible = "at24" },
    { }
};
MODULE_DEVICE_TABLE(of, at24_of_match);

static struct i2c_driver at24_driver = {
    .driver = {
        .name = "at24c256",
        .of_match_table = at24_of_match,
    },
    .probe = at24_probe,
    .remove = at24_remove,
};

module_i2c_driver(at24_driver);

MODULE_AUTHOR("csluder");
MODULE_DESCRIPTION("RPi 4/5 I2C EEPROM Driver");
MODULE_LICENSE("GPL v2");

