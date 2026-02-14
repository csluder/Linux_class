#include <linux/device.h>
#include <linux/slab.h>
#include <linux/printk.h>
#include <linux/module.h>

/* Define a custom container structure */
struct legacy_device {
    struct device dev;
    int custom_data;
} *legacy;

#ifdef EXAMPLE
/*  Block Device Operations: Open */
static int my_blk_open(struct block_device *bdev, fmode_t mode) {
    struct my_block_dev *my_dev = bdev->bd_disk->private_data;
    
    /* Increment refcount so memory isn't freed while disk is open */
    get_device(&my_dev->dev);
    pr_info("my_block_dev: Disk opened\n");
    return 0;
}

/*  Block Device Operations: Release (Close) */
static void my_blk_release(struct gendisk *gd, fmode_t mode) {
    struct my_block_dev *my_dev = gd->private_data;
    
    /* Decrement refcount */
    put_device(&my_dev->dev);
    pr_info("my_block_dev: Disk closed\n");
}

static const struct block_device_operations my_blk_fops = {
    .owner   = THIS_MODULE,
    .open    = my_blk_open,
    .release = my_blk_release,
}
#endif

/* Define the release function */
/* This is called by the kernel core when dev's refcount reaches 0 */
static void legacy_release(struct device *dev)
{
    /* Use container_of to find the start of our custom structure */
    struct legacy_device *legacy_dev = container_of(dev, struct legacy_device, dev);
    pr_info("Called the device release routine");
    
    /* Safely free the memory allocated for the entire structure */
    kfree(legacy_dev);
}

static int __init legacy_init(void) {
    struct legacy_device *legacy_dev;
    int ret = 0;

    /* Allocate memory for the device */
    legacy_dev = kzalloc(sizeof(*legacy_dev), GFP_KERNEL);
    if (!legacy_dev)
        return -ENOMEM;

    legacy = legacy_dev;

    /* Assign the release function */
    legacy_dev->dev.release = legacy_release;
    
    /* Set other required fields */
    legacy_dev->dev.init_name = "my_custom_dev";

#ifdef  USING_LDM
    /* Register with the Linux Device Model */
    ret = device_register(&legacy_dev->dev);
    if (ret) {
        /* If registration fails, put_device() MUST be called to 
           trigger the 'release' callback and kfree(legacy_dev) */
        put_device(&legacy_dev->dev);
        return ret;
    }
#else

    /* INITIALIZE ONLY */
    /* This sets refcount to 1 and prepares the kobject. */
    /* It DOES NOT register the device with sysfs or buses. */
    device_initialize(&legacy_dev->dev);
    pr_info("legacy_dev: Initialized. Refcount is %u \n", 
                kref_read(&legacy->dev.kobj.kref));
#endif

    return ret;
}

static void __exit legacy_exit(void) {
    /*  Unregister the major number on module unload */
    pr_info("Legacy device unregistered\n");
    put_device(&legacy->dev);
    pr_info("legacy_dev: Unregistered. Refcount is %u \n", 
                kref_read(&legacy->dev.kobj.kref));
}

module_init(legacy_init);
module_exit(legacy_exit);

MODULE_LICENSE("GPL");
