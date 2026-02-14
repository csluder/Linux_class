#include <linux/module.h>
#include <linux/version.h>  /* Required for KERNEL_VERSION macros */
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/slab.h>
#include <linux/kobject.h>
#include <linux/device.h>
#include <linux/uaccess.h>

#define DEVICE_NAME "kobj_example"
#define BUF_SIZE 4096

/* 
 * LESSON 1: Embedding the kobject.
 * By putting the kobject inside our custom struct, we can use container_of()
 * to find our data from the kobject's release callback.
 */
struct kobj_example_dev {
    struct cdev cdev;
    char *buffer;
    size_t data_len;
    struct kobject kobj; 
};

static dev_t dev_num;
static struct class *kobject_example_class;
static struct kobj_example_dev *global_dev; 

/**
 * kobj_example_release - The "Destructor"
 * @kobj: Pointer to the kobject being freed
 * 
 * This is called automatically when the reference count reaches ZERO.
 * It is the ONLY place where kfree() should happen for this structure.
 */
static void kobj_example_release(struct kobject *kobj)
{
    struct kobj_example_dev *dev = container_of(kobj, struct kobj_example_dev, kobj);
    
    pr_info("%s: Final reference released. Cleaning up memory.\n", DEVICE_NAME);
    kfree(dev->buffer);
    kfree(dev);
}

/* The ktype defines the behavior of the kobject (in this case, its destructor) */
static struct kobj_type kobj_example_ktype = {
    .release = kobj_example_release,
};

// --- File Operations ---

static int kobj_example_open(struct inode *inode, struct file *file) {
    struct kobj_example_dev *dev = container_of(inode->i_cdev, struct kobj_example_dev, cdev);
    
    file->private_data = dev;
    
    /* 
     * LESSON 2: Increment reference count on Open.
     * This ensures the memory isn't freed while a user is using the file.
     */
    kobject_get(&dev->kobj);
    
    pr_info("%s: Device opened, kobj refcount incremented\n", DEVICE_NAME);
    return 0;
}

static int kobj_example_release_file(struct inode *inode, struct file *file) {
    struct kobj_example_dev *dev = file->private_data;
    
    /* 
     * LESSON 3: Decrement reference count on Close.
     * If the module was unloaded while this file was open, this put()
     * will finally trigger the release() function.
     */
    kobject_put(&dev->kobj);
    
    pr_info("%s: Device closed, kobj refcount decremented\n", DEVICE_NAME);
    return 0;
}

static ssize_t kobj_example_read(struct file *file, char __user *buf, size_t count, loff_t *ppos) {
    struct kobj_example_dev *dev = file->private_data;
    
    if (*ppos >= dev->data_len) return 0;
    if (count > dev->data_len - *ppos) count = dev->data_len - *ppos;

    if (copy_to_user(buf, dev->buffer + *ppos, count)) return -EFAULT;
    
    *ppos += count;
    return count;
}

static ssize_t kobj_example_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos) {
    struct kobj_example_dev *dev = file->private_data;

    if (count > BUF_SIZE) count = BUF_SIZE;

    if (copy_from_user(dev->buffer, buf, count)) return -EFAULT;
    
    dev->data_len = count;
    *ppos = count;
    return count;
}

static const struct file_operations kobj_example_fops = {
    .owner = THIS_MODULE,
    .open = kobj_example_open,
    .release = kobj_example_release_file,
    .read = kobj_example_read,
    .write = kobj_example_write,
    .llseek = default_llseek,
};

// --- Module Lifecycle ---

static int __init kobj_init_module(void) {
    int ret;

    ret = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    if (ret < 0) return ret;

    /* Handle class_create change in Kernel 6.4+ */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    kobject_example_class = class_create(DEVICE_NAME);
#else
    kobject_example_class = class_create(THIS_MODULE, DEVICE_NAME);
#endif

    if (IS_ERR(kobject_example_class)) {
        ret = PTR_ERR(kobject_example_class);
        goto err_unregister;
    }

    /* Allocate the custom struct */
    global_dev = kzalloc(sizeof(*global_dev), GFP_KERNEL);
    if (!global_dev) {
        ret = -ENOMEM;
        goto err_class;
    }

    global_dev->buffer = kmalloc(BUF_SIZE, GFP_KERNEL);
    if (!global_dev->buffer) {
        ret = -ENOMEM;
        goto err_free_dev;
    }

    /* 
     * LESSON 4: Initialize the kobject.
     * This sets the initial refcount to 1.
     * kernel_kobj puts this in /sys/kernel/kobj_example
     */
    ret = kobject_init_and_add(&global_dev->kobj, &kobj_example_ktype, kernel_kobj, DEVICE_NAME);
    if (ret) {
        /* 
         * Important: if kobject_init_and_add fails, you MUST use kobject_put
         * to let the release callback handle the kfree.
         */
        kobject_put(&global_dev->kobj);
        goto err_class; 
    }

    cdev_init(&global_dev->cdev, &kobj_example_fops);
    ret = cdev_add(&global_dev->cdev, dev_num, 1);
    if (ret) goto err_kobj;

    device_create(kobject_example_class, NULL, dev_num, NULL, DEVICE_NAME);

    pr_info("%s: Module loaded with kobject management\n", DEVICE_NAME);
    return 0;

err_kobj:
    kobject_del(&global_dev->kobj);
    kobject_put(&global_dev->kobj);
err_free_dev:
    /* Note: We only kfree here if the kobject hasn't been initialized yet */
    if (!global_dev->kobj.state_initialized) kfree(global_dev);
err_class:
    class_destroy(kobject_example_class);
err_unregister:
    unregister_chrdev_region(dev_num, 1);
    return ret;
}

static void __exit kobj_exit_module(void) {
    /* Stop new users from opening the device */
    device_destroy(kobject_example_class, dev_num);
    cdev_del(&global_dev->cdev);
    class_destroy(kobject_example_class);
    unregister_chrdev_region(dev_num, 1);

    /* 
     * LESSON 5: Release the module's reference.
     * The module "owns" one reference from kobject_init_and_add.
     * We drop it here. If no user has the file open, kobj_example_release 
     * runs immediately. If a user HAS the file open, the memory stays 
     * until they close it.
     */
    if (global_dev) {
        kobject_put(&global_dev->kobj);
    }
    
    pr_info("%s: Module unloaded\n", DEVICE_NAME);
}

module_init(kobj_init_module);
module_exit(kobj_exit_module);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Instructor");
MODULE_DESCRIPTION("Kobject Lifecycle Management Example");
