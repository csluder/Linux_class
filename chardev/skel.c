#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/poll.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/device.h>
#include <linux/version.h>

#define DEVICE_NAME "skeleton_dev"
#define BUF_SIZE 4096

struct skeleton_dev {
    struct cdev cdev;
    char *buffer;
    struct mutex lock;
    wait_queue_head_t wait_queue;
};

/* Global pointers for cleanup in module_exit */
static struct skeleton_dev *skel_dev;
static dev_t dev_num;
static struct class *skel_class;

// --- File Operations ---

static int skel_open(struct inode *inode, struct file *file) {
    /* container_of links the cdev pointer in the inode to our parent struct */
    struct skeleton_dev *dev = container_of(inode->i_cdev, struct skeleton_dev, cdev);
    file->private_data = dev;
    pr_info("%s: Device opened\n", DEVICE_NAME);
    return 0;
}

static int skel_release(struct inode *inode, struct file *file) {
    pr_info("%s: Device closed\n", DEVICE_NAME);
    return 0;
}

static ssize_t skel_read(struct file *file, char __user *buf, size_t count, loff_t *ppos) {
    struct skeleton_dev *dev = file->private_data;
    ssize_t retval = 0;

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    if (*ppos >= BUF_SIZE) goto out;
    if (count > BUF_SIZE - *ppos) count = BUF_SIZE - *ppos;

    if (copy_to_user(buf, dev->buffer + *ppos, count)) {
        retval = -EFAULT;
        goto out;
    }

    *ppos += count;
    retval = count;

out:
    mutex_unlock(&dev->lock);
    return retval;
}

static ssize_t skel_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos) {
    struct skeleton_dev *dev = file->private_data;
    ssize_t retval = 0;

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    if (*ppos >= BUF_SIZE) {
        retval = -ENOSPC;
        goto out;
    }
    if (count > BUF_SIZE - *ppos) count = BUF_SIZE - *ppos;

    if (copy_from_user(dev->buffer + *ppos, buf, count)) {
        retval = -EFAULT;
        goto out;
    }

    *ppos += count;
    retval = count;
    wake_up_interruptible(&dev->wait_queue);

out:
    mutex_unlock(&dev->lock);
    return retval;
}

static loff_t skel_lseek(struct file *file, loff_t offset, int whence) {
    return fixed_size_llseek(file, offset, whence, BUF_SIZE);
}

static __poll_t skel_poll(struct file *file, poll_table *wait) {
    struct skeleton_dev *dev = file->private_data;
    __poll_t mask = 0;

    poll_wait(file, &dev->wait_queue, wait);
    mask |= EPOLLOUT | EPOLLWRNORM;
    mask |= EPOLLIN | EPOLLRDNORM;
    return mask;
}

static int skel_mmap(struct file *file, struct vm_area_struct *vma) {
    struct skeleton_dev *dev = file->private_data;
    unsigned long pfn = virt_to_phys(dev->buffer) >> PAGE_SHIFT;
    
    vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
    if (remap_pfn_range(vma, vma->vm_start, pfn, vma->vm_end - vma->vm_start, vma->vm_page_prot))
        return -EAGAIN;

    return 0;
}

static const struct file_operations skel_fops = {
    .owner   = THIS_MODULE,
    .open    = skel_open,
    .release = skel_release,
    .read    = skel_read,
    .write   = skel_write,
    .llseek  = skel_lseek,
    .poll    = skel_poll,
    .mmap    = skel_mmap,
};

// --- Module Init/Exit ---

static int __init skel_init(void) {
    struct device *dev_ptr;
    int ret;

    /* 1. Allocate device numbers */
    ret = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    if (ret) return ret;

    /* 2. Create class (signature changed in Kernel 6.4, owner removed) */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    skel_class = class_create(DEVICE_NAME);
#else
    skel_class = class_create(THIS_MODULE, DEVICE_NAME);
#endif

    if (IS_ERR(skel_class)) {
        ret = PTR_ERR(skel_class);
        goto fail_class;
    }

    /* 3. Create the device FIRST to anchor devres memory */
    dev_ptr = device_create(skel_class, NULL, dev_num, NULL, DEVICE_NAME);
    if (IS_ERR(dev_ptr)) {
        ret = PTR_ERR(dev_ptr);
        goto fail_dev;
    }

    /* 4. Use devm_kzalloc anchored to the created device pointer */
    skel_dev = devm_kzalloc(dev_ptr, sizeof(struct skeleton_dev), GFP_KERNEL);
    if (!skel_dev) {
        ret = -ENOMEM;
        goto fail_all;
    }

    skel_dev->buffer = devm_kzalloc(dev_ptr, BUF_SIZE, GFP_KERNEL);
    if (!skel_dev->buffer) {
        ret = -ENOMEM;
        goto fail_all;
    }

    /* 5. Init hardware abstraction */
    mutex_init(&skel_dev->lock);
    init_waitqueue_head(&skel_dev->wait_queue);
    cdev_init(&skel_dev->cdev, &skel_fops);

    ret = cdev_add(&skel_dev->cdev, dev_num, 1);
    if (ret < 0) goto fail_all;

    pr_info("%s: Initialized successfully\n", DEVICE_NAME);
    return 0;

fail_all:
    device_destroy(skel_class, dev_num);
fail_dev:
    class_destroy(skel_class);
fail_class:
    unregister_chrdev_region(dev_num, 1);
    return ret;
}

static void __exit skel_exit(void) {
    /*
     * When device_destroy is called, devres automatically frees 
     * skel_dev and skel_dev->buffer. It also removes 
     * the device node and sysfs entry from the class. The device 
     * structure is removed by kernel garbage collection 
     */
    device_destroy(skel_class, dev_num);
    cdev_del(&skel_dev->cdev);
    class_destroy(skel_class);
    /* Free the device numbers */
    unregister_chrdev_region(dev_num, 1);
    pr_info("%s: Module exited\n", DEVICE_NAME);
}

module_init(skel_init);
module_exit(skel_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Modern Kernel Student");
MODULE_DESCRIPTION("Modern Char Driver with Devres and Mutexes");
