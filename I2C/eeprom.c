// SPDX-License-Identifier: GPL-2.0
/*
 * at24c256_i2c AT24C256 I2C EEPROM character driver
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

#define AT24C256_SIZE_BYTES   32768
#define AT24C256_PAGE_SIZE    64
#define AT24C256_WRITE_MS_DEF 5
#define AT24_READ_CHUNK       256

struct at24_data {
    struct i2c_client* client;
    struct miscdevice miscdev;
    struct bin_attribute bin_attr;
    struct mutex lock;
    char* devname;
};

/* ---------- I2C Transfer Helpers ---------- */

static int at24_read_combined(struct i2c_client* client, u16 addr, u8* buf, size_t len)
{
    u8 addrbuf[2] = { (u8)(addr >> 8), (u8)(addr & 0xFF) };
    struct i2c_msg msgs[2] = {
        {.addr = client->addr, .flags = 0,        .len = 2, .buf = addrbuf },
        {.addr = client->addr, .flags = I2C_M_RD, .len = len, .buf = buf   },
    };
    int ret = i2c_transfer(client->adapter, msgs, 2);
    return (ret == 2) ? 0 : ((ret < 0) ? ret : -EIO);
}

static int at24_write_page_msg(struct i2c_client* client, u16 addr, const u8* data, size_t len)
{
    struct i2c_msg msg;
    u8* tx = kmalloc(len + 2, GFP_KERNEL);
    int ret;
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

static int at24_wait_write_done(struct i2c_client* client)
{
    unsigned long deadline = jiffies + msecs_to_jiffies(AT24C256_WRITE_MS_DEF);
    u8 addrbuf[2] = { 0, 0 };
    struct i2c_msg msg = { .addr = client->addr, .flags = 0, .len = 2, .buf = addrbuf };
    do {
        if (i2c_transfer(client->adapter, &msg, 1) == 1) return 0;
        usleep_range(1000, 2000);
    } while (time_before(jiffies, deadline));
    return -ETIMEDOUT;
}

/* ---------- File Operations (/dev) ---------- */

static ssize_t at24_read_file(struct file* f, char __user* ubuf, size_t count, loff_t* ppos)
{
    struct at24_data* ee = container_of(f->private_data, struct at24_data, miscdev);
    loff_t pos = *ppos;
    size_t todo = count;
    ssize_t done = 0;
    int ret = 0;

    if (pos >= AT24C256_SIZE_BYTES) return 0;
    if (pos + todo > AT24C256_SIZE_BYTES) todo = AT24C256_SIZE_BYTES - pos;
    
    mutex_lock(&ee->lock);
    while (todo) {
        size_t chunk = min_t(size_t, todo, AT24_READ_CHUNK);
        u8* kbuf = kmalloc(chunk, GFP_KERNEL);
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

static ssize_t at24_write_file(struct file* f, const char __user* ubuf, size_t count, loff_t* ppos)
{
    struct at24_data* ee = container_of(f->private_data, struct at24_data, miscdev);
    loff_t pos = *ppos;
    size_t remaining = count;
    ssize_t written = 0;
    int ret = 0;

    if (pos >= AT24C256_SIZE_BYTES) return -ENOSPC;
    if (pos + remaining > AT24C256_SIZE_BYTES) remaining = AT24C256_SIZE_BYTES - pos;

    mutex_lock(&ee->lock);
    while (remaining) {
        size_t page_off = pos % AT24C256_PAGE_SIZE;
        size_t chunk = min_t(size_t, remaining, AT24C256_PAGE_SIZE - page_off);
        u8* kbuf = kmalloc(chunk, GFP_KERNEL);
        if (!kbuf) { ret = -ENOMEM; break; }
        if (copy_from_user(kbuf, ubuf + written, chunk)) { kfree(kbuf); ret = -EFAULT; break; }
        ret = at24_write_page_msg(ee->client, (u16)pos, kbuf, chunk);
        kfree(kbuf);
        if (ret) break;
        at24_wait_write_done(ee->client);
        pos += chunk; remaining -= chunk; written += chunk;
    }
    mutex_unlock(&ee->lock);
    if (written > 0) { *ppos += written; return written; }
    return ret;
}

static loff_t at24_llseek_file(struct file *f, loff_t offset, int whence)
{
    return fixed_size_llseek(f, offset, whence, AT24C256_SIZE_BYTES);
}

static const struct file_operations at24_fops = {
    .owner = THIS_MODULE,
    .read = at24_read_file,
    .write = at24_write_file,
    .llseek = at24_llseek_file,
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

    mutex_lock(&ee->lock);
    ret = at24_read_combined(client, (u16)off, (u8 *)buf, count);
    mutex_unlock(&ee->lock);

    return ret ? ret : count;
}

static ssize_t at24_sysfs_write(struct file *filp, struct kobject *kobj,
                                struct bin_attribute *attr, char *buf,
                                loff_t off, size_t count)
{
    struct device *dev = kobj_to_dev(kobj);
    struct i2c_client *client = to_i2c_client(dev);
    struct at24_data *ee = i2c_get_clientdata(client);
    size_t written = 0;
    int ret = 0;

    mutex_lock(&ee->lock);
    while (written < count) {
        size_t curr_off = off + written;
        size_t page_limit = AT24C256_PAGE_SIZE - (curr_off % AT24C256_PAGE_SIZE);
        size_t chunk = min_t(size_t, count - written, page_limit);

        ret = at24_write_page_msg(client, (u16)curr_off, (const u8 *)(buf + written), chunk);
        if (ret) break;
        at24_wait_write_done(client);
        written += chunk;
    }
    mutex_unlock(&ee->lock);
    return written ? written : ret;
}

/* ---------- Probe & Remove ---------- */

static int at24_probe(struct i2c_client *client)
{
    struct at24_data *ee;
    int ret;

    ee = devm_kzalloc(&client->dev, sizeof(*ee), GFP_KERNEL);
    if (!ee) return -ENOMEM;

    ee->client = client;
    mutex_init(&ee->lock);
    i2c_set_clientdata(client, ee);

    /* 1. Register Binary Sysfs Interface */
    sysfs_bin_attr_init(&ee->bin_attr);
    ee->bin_attr.attr.name = "eeprom";
    ee->bin_attr.attr.mode = 0644;
    ee->bin_attr.read = at24_sysfs_read;
    ee->bin_attr.write = at24_sysfs_write;
    ee->bin_attr.size = AT24C256_SIZE_BYTES;
    ret = device_create_bin_file(&client->dev, &ee->bin_attr);
    if (ret) return ret;

    /* 2. Register Character Device Interface */
    ee->devname = devm_kasprintf(&client->dev, GFP_KERNEL, "at24c256-%d-%02x", 
                                 client->adapter->nr, client->addr);
    ee->miscdev.minor = MISC_DYNAMIC_MINOR;
    ee->miscdev.name = ee->devname;
    ee->miscdev.fops = &at24_fops;
    ee->miscdev.parent = &client->dev;

    ret = misc_register(&ee->miscdev);
    if (ret) {
        device_remove_bin_file(&client->dev, &ee->bin_attr);
        return ret;
    }

    dev_info(&client->dev, "EEPROM ready: /dev/%s and /sys/.../eeprom\n", ee->devname);
    return 0;
}

static void at24_remove(struct i2c_client *client)
{
    struct at24_data *ee = i2c_get_clientdata(client);
    if (ee) {
        misc_deregister(&ee->miscdev);
        device_remove_bin_file(&client->dev, &ee->bin_attr);
    }
}

/* ---------- Tables ---------- */

static const struct of_device_id at24_of_match[] = {
    { .compatible = "atmel,24c256" },
    { .compatible = "at24" },
    { }
};
MODULE_DEVICE_TABLE(of, at24_of_match);

static const struct i2c_device_id at24_ids[] = {
    { "24c256", 0 },
    { "at24", 0 },
    { }
};
MODULE_DEVICE_TABLE(i2c, at24_ids);

static struct i2c_driver at24_driver = {
    .driver = {
        .name = "at24c256",
        .of_match_table = at24_of_match,
    },
    .probe    = at24_probe,
    .remove   = at24_remove,
    .id_table = at24_ids,
};

module_i2c_driver(at24_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("csluder");

