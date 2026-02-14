#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h> /* Required for of_device_id */
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/version.h>
#include <linux/scatterlist.h>
#include <crypto/skcipher.h>     /* Symmetric Key Cipher API */

#define DRIVER_NAME "l3harris_secure"
#define MASTER_KEY_SIZE 16       /* AES-128 Block/Key Size */
#define LUKS_KEY_SIZE   512      /* Standard 4096-bit LUKS key */

/**
 * ANNOTATION: The Master Key Simulation
 * ------------------------------------
 * Hardcoded to simulate a key etched into hardware (OTP).
 * This acts as the "Root of Trust" for unwrapping the Session Key.
 */
static const u8 master_key[MASTER_KEY_SIZE] = {
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
    0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10
};

struct l3harris_dev {
    struct cdev cdev;
    struct class *cls;
    u8 *session_key;      /* Layer 1: Decrypted via Master Key */
    u8 *luks_password;    /* Layer 2: Final 512B LUKS password */
    struct bin_attribute key_attr;
    dev_t dev_num;
};

static struct l3harris_dev *ldev_ptr;

/**
 * ANNOTATION: Multi-block AES-CBC Decryption
 * -----------------------------------------
 * This function processes 32 blocks (512 bytes) using the Crypto API.
 * FIX: We must provide a valid 16-byte IV buffer. Passing NULL causes 
 * a Kernel Oops on Pi 5 hardware (aes_ce_blk).
 */
static int aes_decrypt_buffer(const u8 *key, size_t key_len, const u8 *input, u8 *output, size_t data_len) {
    struct crypto_skcipher *tfm;
    struct skcipher_request *req;
    struct scatterlist sg_in, sg_out;
    u8 iv[MASTER_KEY_SIZE]; /* Must be valid memory for Hardware engine */
    int ret;

    /* Initialize IV to zero to match OpenSSL defaults */
    memset(iv, 0, sizeof(iv));

    /* 1. Allocate a standard skcipher (AES in CBC mode) */
    tfm = crypto_alloc_skcipher("cbc(aes)", 0, 0);
    if (IS_ERR(tfm)) return PTR_ERR(tfm);

    /* 2. Allocate request object (Mandatory for modern skcipher API) */
    req = skcipher_request_alloc(tfm, GFP_KERNEL);
    if (!req) {
        ret = -ENOMEM;
        goto out_tfm;
    }

    ret = crypto_skcipher_setkey(tfm, key, key_len);
    if (ret) goto out_req;

    sg_init_one(&sg_in, input, data_len);
    sg_init_one(&sg_out, output, data_len);

    /**
     * LESSON: The IV pointer MUST be valid.
     * Even if zeroed, hardware drivers like 'aes_ce_blk' on Pi 5 will 
     * dereference this address to load the IV register.
     */
    skcipher_request_set_crypt(req, &sg_in, &sg_out, data_len, iv);

    /* 3. Execute Decryption */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
    ret = crypto_skcipher_decrypt(req);
#else
    ret = crypto_sync_skcipher_decrypt(req);
#endif
    
out_req:
    skcipher_request_free(req);
out_tfm:
    crypto_free_skcipher(tfm);
    return ret;
}

/**
 * ANNOTATION: Stage 1 Injection (Sysfs Binary)
 * -------------------------------------------
 * Userspace writes a 16-byte encrypted blob here.
 * Result: Session Key is stored in ldev_ptr->session_key.
 */
static ssize_t session_blob_write(struct file *filp, struct kobject *kobj,
                                 struct bin_attribute *bin_attr,
                                 char *buf, loff_t pos, size_t count) {
    if (count != MASTER_KEY_SIZE) return -EINVAL;

    if (aes_decrypt_buffer(master_key, MASTER_KEY_SIZE, (u8 *)buf, ldev_ptr->session_key, MASTER_KEY_SIZE))
        return -EIO;

    pr_info("%s: Stage 1: Session Key decrypted.\n", DRIVER_NAME);
    return count;
}

/**
 * ANNOTATION: Stage 2 Injection (Char Write)
 * -----------------------------------------
 * Userspace writes a 512-byte blob to /dev/l3harris_secure.
 * Result: LUKS Password is stored in ldev_ptr->luks_password.
 */
static ssize_t luks_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos) {
    u8 *encrypted_luks;
    int ret;

    if (count != LUKS_KEY_SIZE) return -EINVAL;
    if (!ldev_ptr->session_key) return -EACCES; /* Require Stage 1 first */

    encrypted_luks = kmalloc(LUKS_KEY_SIZE, GFP_KERNEL);
    if (!encrypted_luks) return -ENOMEM;

    if (copy_from_user(encrypted_luks, buf, LUKS_KEY_SIZE)) {
        kfree(encrypted_luks);
        return -EFAULT;
    }

    /* Decrypt the LUKS key using the laddered Session Key */
    ret = aes_decrypt_buffer(ldev_ptr->session_key, MASTER_KEY_SIZE, 
                            encrypted_luks, ldev_ptr->luks_password, LUKS_KEY_SIZE);
    
    kfree(encrypted_luks);
    if (ret) return -EIO;

    pr_info("%s: Stage 2: LUKS Password ready.\n", DRIVER_NAME);
    return count;
}

static ssize_t luks_read(struct file *file, char __user *buf, size_t count, loff_t *ppos) {
    if (*ppos >= LUKS_KEY_SIZE) return 0;
    if (count > LUKS_KEY_SIZE - *ppos) count = LUKS_KEY_SIZE - *ppos;
    if (copy_to_user(buf, ldev_ptr->luks_password + *ppos, count))
        return -EFAULT;
    *ppos += count;
    return count;
}

static const struct file_operations luks_fops = {
    .owner = THIS_MODULE,
    .write = luks_write,
    .read  = luks_read,
    .llseek = default_llseek,
};

/* --- Platform Driver Lifecycle --- */

static int l3harris_probe(struct platform_device *pdev) {
    struct device *dev = &pdev->dev;
    struct device *node;

    ldev_ptr = devm_kzalloc(dev, sizeof(*ldev_ptr), GFP_KERNEL);
    ldev_ptr->session_key = devm_kzalloc(dev, MASTER_KEY_SIZE, GFP_KERNEL);
    ldev_ptr->luks_password = devm_kzalloc(dev, LUKS_KEY_SIZE, GFP_KERNEL);

    if (alloc_chrdev_region(&ldev_ptr->dev_num, 0, 1, DRIVER_NAME))
        return -EBUSY;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    ldev_ptr->cls = class_create(DRIVER_NAME);
#else
    ldev_ptr->cls = class_create(THIS_MODULE, DRIVER_NAME);
#endif

    node = device_create(ldev_ptr->cls, dev, ldev_ptr->dev_num, NULL, DRIVER_NAME);

    sysfs_bin_attr_init(&ldev_ptr->key_attr);
    ldev_ptr->key_attr.attr.name = "key_blob";
    ldev_ptr->key_attr.attr.mode = 0200; 
    ldev_ptr->key_attr.size = MASTER_KEY_SIZE;
    ldev_ptr->key_attr.write = session_blob_write;
    
    if (device_create_bin_file(node, &ldev_ptr->key_attr))
        goto err_clean;

    cdev_init(&ldev_ptr->cdev, &luks_fops);
    if (cdev_add(&ldev_ptr->cdev, ldev_ptr->dev_num, 1))
        goto err_bin;

    platform_set_drvdata(pdev, ldev_ptr);
    dev_info(dev, "L3Harris Secure Driver Probed Successfully\n");
    return 0;

err_bin:
    device_remove_bin_file(node, &ldev_ptr->key_attr);
err_clean:
    device_destroy(ldev_ptr->cls, ldev_ptr->dev_num);
    class_destroy(ldev_ptr->cls);
    unregister_chrdev_region(ldev_ptr->dev_num, 1);
    return -1;
}

/**
 * ANNOTATION: Modern Platform Remove
 * ---------------------------------
 * Handles return type change (void) in Kernel 6.11+
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
static void l3harris_remove(struct platform_device *pdev)
#else
static int l3harris_remove(struct platform_device *pdev)
#endif
{
    struct l3harris_dev *ldev = platform_get_drvdata(pdev);
    if (ldev) {
        memzero_explicit(ldev->session_key, MASTER_KEY_SIZE);
        memzero_explicit(ldev->luks_password, LUKS_KEY_SIZE);
        device_destroy(ldev->cls, ldev->dev_num);
        class_destroy(ldev->cls);
        unregister_chrdev_region(ldev->dev_num, 1);
    }
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 11, 0)
    return 0;
#endif
}

static const struct of_device_id l3harris_match[] = {
    { .compatible = "l3harris,platform-device" },
    { }
};
MODULE_DEVICE_TABLE(of, l3harris_match);

static struct platform_driver l3harris_driver = {
    .probe = l3harris_probe,
    .remove = l3harris_remove, 
    .driver = { 
        .name = "l3harris_secure", 
        .of_match_table = l3harris_match 
    },
};

module_platform_driver(l3harris_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Multi-Stage 512B LUKS Key Ladder");
