#include <linux/module.h>
#include <linux/version.h>  /* For LINUX_VERSION_CODE */
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/kref.h>
#include <linux/device.h>

#define DEVICE_NAME "kref_example"
#define BUF_SIZE 4096

/*
 * LESSON 1: The kref structure.
 * Unlike kobject, kref does not provide sysfs visibility. It is a 
 * simple atomic counter that ensures the structure survives as long 
 * as any kernel subsystem or userspace process is using it.
 */
struct kref_example_dev {
    struct cdev cdev;
    char *name;
    char *buffer;
    struct kref refcount; /* The reference counter */
};

static dev_t dev_num;
static struct class *kref_example_class;
static struct kref_example_dev *global_obj; 

/**
 * my_data_release - The Destructor
 * @kref: Pointer to the kref member inside our struct
 * 
 * Called automatically by kref_put() only when the counter reaches zero.
 */
static void my_data_release(struct kref *kref)
{
    /* Use container_of to go from &refcount back to the start of the struct */
    struct kref_example_dev *data = container_of(kref, struct kref_example_dev, refcount);

    pr_info("%s: kref: Final reference released. Freeing memory.\n", data->name);
    kfree(data->name);
    kfree(data->buffer);
    kfree(data);
}

// --- File Operations ---

static int kref_example_open(struct inode *inode, struct file *file) {
    struct kref_example_dev *dev;
    
    dev = container_of(inode->i_cdev, struct kref_example_dev, cdev);
    file->private_data = dev;

    /* 
     * LESSON 2: Increment the counter.
     * Every 'file' object created in the kernel now holds a reference
     * to our data structure.
     */
    kref_get(&dev->refcount);
    
    pr_info("%s: Device opened. Refcount: %u\n", 
            DEVICE_NAME, kref_read(&dev->refcount));

    return 0;
}

static int kref_example_release(struct inode *inode, struct file *file) {
    struct kref_example_dev *dev = file->private_data;

    pr_info("%s: Device closed. Decrementing kref.\n", DEVICE_NAME);
    
    /* 
     * LESSON 3: Decrement the counter.
     * We pass the 'my_data_release' function pointer. If this put 
     * results in 0, that function is called immediately.
     */
    kref_put(&dev->refcount, my_data_release);

    return 0;
}

/* Standard Read/Write Implementation */
static ssize_t kref_example_read(struct file *file, char __user *buf, size_t count, loff_t *ppos) {
    struct kref_example_dev *dev = file->private_data;
    if (*ppos >= BUF_SIZE) return 0;
    if (count > BUF_SIZE - *ppos) count = BUF_SIZE - *ppos;

    if (copy_to_user(buf, dev->buffer + *ppos, count)) return -EFAULT;
    *ppos += count;
    return count;
}

static ssize_t kref_example_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos) {
    struct kref_example_dev *dev = file->private_data;
    if (*ppos >= BUF_SIZE) return -ENOSPC;
    if (count > BUF_SIZE - *ppos) count = BUF_SIZE - *ppos;

    if (copy_from_user(dev->buffer + *ppos, buf, count)) return -EFAULT;
    *ppos += count;
    return count;
}

static const struct file_operations kref_example_fops = {
    .owner   = THIS_MODULE,
    .open    = kref_example_open,
    .release = kref_example_release,
    .read    = kref_example_read,
    .write   = kref_example_write,
    .llseek  = default_llseek,
};

// --- Module Init/Exit ---

static int __init kref_example_init(void) {
    int ret;

    ret = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    if (ret < 0) return ret;

    /* Handle class_create API change (Kernel 6.4+) */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    kref_example_class = class_create(DEVICE_NAME);
#else
    kref_example_class = class_create(THIS_MODULE, DEVICE_NAME);
#endif

    if (IS_ERR(kref_example_class)) {
        ret = PTR_ERR(kref_example_class);
        goto err_unregister;
    }

    global_obj = kzalloc(sizeof(*global_obj), GFP_KERNEL);
    if (!global_obj) {
        ret = -ENOMEM;
        goto err_class;
    }

    global_obj->buffer = kzalloc(BUF_SIZE, GFP_KERNEL);
    global_obj->name = kstrdup(DEVICE_NAME, GFP_KERNEL);

    /* 
     * LESSON 4: Initialize the kref.
     * This sets the counter to 1. The module itself "owns" this 
     * first reference.
     */
    kref_init(&global_obj->refcount);

    cdev_init(&global_obj->cdev, &kref_example_fops);
    ret = cdev_add(&global_obj->cdev, dev_num, 1);
    if (ret < 0) goto err_mem;

    device_create(kref_example_class, NULL, dev_num, NULL, DEVICE_NAME);
    pr_info("%s: Module initialized with kref\n", DEVICE_NAME);
    return 0;

err_mem:
    /* Manually free if kref was never truly shared */
    kfree(global_obj->buffer);
    kfree(global_obj->name);
    kfree(global_obj);
err_class:
    class_destroy(kref_example_class);
err_unregister:
    unregister_chrdev_region(dev_num, 1);
    return ret;
}

static void __exit kref_example_exit(void) {
    /* Stop new userspace opens by removing the /dev node */
    device_destroy(kref_example_class, dev_num);
    cdev_del(&global_obj->cdev);
    class_destroy(kref_example_class);
    unregister_chrdev_region(dev_num, 1);
    
    /* 
     * LESSON 5: Release the module's initial reference.
     * If a user still has the device open, the refcount is > 1, 
     * so 'my_data_release' will NOT run yet. The memory stays safe.
     */
    if (global_obj) {
        kref_put(&global_obj->refcount, my_data_release);
    }
    
    pr_info("%s: Module exit complete\n", DEVICE_NAME);
}

module_init(kref_example_init);
module_exit(kref_example_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Instructor");
MODULE_DESCRIPTION("Class example for kref reference counting");
