#include <linux/module.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#define DEVICE_NAME "modern_encryptor"
#define CLASS_NAME  "encrypt_class"
#define BUF_SIZE    1024

static dev_t dev_num;
static struct cdev my_cdev;
static struct class *my_class;
static struct device *my_device;
static char *kernel_buffer;
static int shift_key = 3; // Adjustable via sysfs

/**
* Caesar Cipher Logic
*/
static void encrypt_data(char *data, size_t len) {
    int i;
    for (i = 0; i < len; i++) {
        if (data[i] >= 'a' && data[i] <= 'z')
            data[i] = ((data[i] - 'a' + shift_key) % 26) + 'a';
        else if (data[i] >= 'A' && data[i] <= 'Z')
            data[i] = ((data[i] - 'A' + shift_key) % 26) + 'A';
    }
}

/**
* Sysfs "Show" Routine - Reads current shift_key
*/
static ssize_t key_show(struct device *dev, struct device_attribute *attr, char *buf) {
    return sprintf(buf, "%d\n", shift_key);
}

/**
* Sysfs "Store" Routine - Sets new shift_key
*/
static ssize_t key_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count) {
    int val;
    if (kstrtoint(buf, 10, &val) < 0) return -EINVAL;
    shift_key = val % 26; 
    return count;
}

// Macro to create the dev_attr_key structure
static DEVICE_ATTR_RW(key);

/**
* File Operations
*/
static ssize_t dev_read(struct file *f, char __user *buf, size_t len, loff_t *off) {
    size_t datalen = strlen(kernel_buffer);
    if (*off >= datalen) return 0;
    if (copy_to_user(buf, kernel_buffer, datalen)) return -EFAULT;
    *off += datalen;
    return datalen;
}

static ssize_t dev_write(struct file *f, const char __user *buf, size_t len, loff_t *off) {
    size_t to_copy = min(len, (size_t)BUF_SIZE - 1);
    memset(kernel_buffer, 0, BUF_SIZE);
    if (copy_from_user(kernel_buffer, buf, to_copy)) return -EFAULT;
    encrypt_data(kernel_buffer, to_copy);
    return to_copy;
}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .read = dev_read,
    .write = dev_write,
};

/**
* Module Init
*/
static int __init mod_init(void) {
    alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    
    cdev_init(&my_cdev, &fops);
    cdev_add(&my_cdev, dev_num, 1);
    
    my_class = class_create(CLASS_NAME);
    my_device = device_create(my_class, NULL, dev_num, NULL, DEVICE_NAME);
    
    // Create the sysfs file /sys/class/encrypt_class/modern_encryptor/key
    if (device_create_file(my_device, &dev_attr_key) < 0) {
        pr_err("Failed to create sysfs file\n");
    }

    kernel_buffer = kmalloc(BUF_SIZE, GFP_KERNEL);
    pr_info("Encryptor Loaded. Key: %d\n", shift_key);
    return 0;
}

/**
* Module Exit
*/
static void __exit mod_exit(void) {
    device_remove_file(my_device, &dev_attr_key);
    device_destroy(my_class, dev_num);
    class_destroy(my_class);
    cdev_del(&my_cdev);
    unregister_chrdev_region(dev_num, 1);
    kfree(kernel_buffer);
}

module_init(mod_init);
module_exit(mod_exit);
MODULE_LICENSE("GPL");
