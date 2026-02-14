#include <linux/module.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/delay.h>

/* --- Hardware Definitions --- */
#define SFP_REG_ADDR(off) (sfp_base + (off))
#define INGR_ERR          0x100
static void __iomem *sfp_base;
static int fuse_armed = 0;

/* Wrapper for single-register binary files */
struct sfp_bin_attribute {
    struct bin_attribute bin_attr;
    unsigned int reg_offset;
};

#define to_sfp_bin(_attr) container_of(_attr, struct sfp_bin_attribute, bin_attr)

/* --- 1. Standard Attributes (arm, burn) --- */
static ssize_t sfp_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
    return sysfs_emit(buf, "%d\n", fuse_armed);
}

static ssize_t sfp_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count) {
    if (kstrtoint(buf, 0, &fuse_armed)) return -EINVAL;
    return count;
}

static struct kobj_attribute sfp_arm = __ATTR(arm, 0600, sfp_show, sfp_store);
static struct kobj_attribute sfp_burn = __ATTR(burn, 0600, sfp_show, sfp_store);

/* --- 2. Logic Helpers --- */
static int wait_for_complete(void) {
    int cnt = 0;
    const int LIMIT = 3000;
    u32 reg;
    // Assuming 0x00 is your INGR register
    while ((reg = ioread32be(SFP_REG_ADDR(0x00))) != 0 && cnt < LIMIT) {
        if (reg & INGR_ERR) return -EIO;
        udelay(1000);
        cnt++;
    }
    return (cnt >= LIMIT) ? -ETIMEDOUT : 0;
}

static ssize_t sfp_read_array(char *buf, unsigned int start, unsigned int end) {
    unsigned int off, i = 0;
    u32 *words = (u32 *)buf;
    for (off = start; off <= end; off += 4)
        words[i++] = ioread32be(SFP_REG_ADDR(off));
    return i * 4;
}

static ssize_t sfp_write_array(const char *buf, unsigned int start, unsigned int end) {
    unsigned int off, i = 0;
    const u32 *words = (const u32 *)buf;
    for (off = start; off <= end; off += 4, i++) {
        iowrite32be(words[i], SFP_REG_ADDR(off));
        if (wait_for_complete() < 0) return -EIO;
    }
    return i * 4;
}

/* --- 3. Binary Callbacks --- */

/* Handler for single u32 registers */
static ssize_t sfp_bin_reg_read(struct file *f, struct kobject *k, struct bin_attribute *a, char *b, loff_t o, size_t c) {
    struct sfp_bin_attribute *sattr = to_sfp_bin(a);
    u32 val = ioread32be(SFP_REG_ADDR(sattr->reg_offset));
    if (c < 4) return -EINVAL;
    memcpy(b, &val, 4);
    return 4;
}

static ssize_t sfp_bin_reg_write(struct file *f, struct kobject *k, struct bin_attribute *a, char *b, loff_t o, size_t c) {
    struct sfp_bin_attribute *sattr = to_sfp_bin(a);
    u32 val;
    if (c < 4) return -EINVAL;
    memcpy(&val, b, 4);
    iowrite32be(val, SFP_REG_ADDR(sattr->reg_offset));
    return 4;
}

/* Macro to generate functions for arrays (drvr, otpmk, etc) */
#define SFP_ARRAY_HANDLER(_name, _start, _end) \
static ssize_t sfp_read_##_name(struct file *f, struct kobject *k, struct bin_attribute *a, char *b, loff_t o, size_t c) \
{ return sfp_read_array(b, _start, _end); } \
static ssize_t sfp_write_##_name(struct file *f, struct kobject *k, struct bin_attribute *a, char *b, loff_t o, size_t c) \
{ return sfp_write_array(b, _start, _end); }

/* Generates: sfp_read_drvr, sfp_write_drvr, etc. */
SFP_ARRAY_HANDLER(drvr,  0x20, 0x24) 
SFP_ARRAY_HANDLER(otpmk, 0x30, 0x4C) 
SFP_ARRAY_HANDLER(srkh,  0x50, 0x6C) 
SFP_ARRAY_HANDLER(ouid,  0x70, 0x80) 

/* --- 4. Attribute Registration --- */

#define BIN_REG_RW(_name, _off) { .bin_attr = { .attr = { .name = #_name, .mode = 0600 }, .size = 4, .read = sfp_bin_reg_read, .write = sfp_bin_reg_write }, .reg_offset = _off }
#define BIN_ARRAY_RW(_fn_suffix, _sysfs_name, _size) { .attr = { .name = _sysfs_name, .mode = 0600 }, .size = _size, .read = sfp_read_##_fn_suffix, .write = sfp_write_##_fn_suffix }

static struct sfp_bin_attribute sfp_regs[] = {
    BIN_REG_RW(sfp_ingr,    0x00),
    BIN_REG_RW(sfp_svhesr,  0x04),
    BIN_REG_RW(sfp_sfpcr,   0x08),
    BIN_REG_RW(sfp_version, 0x0C),
    BIN_REG_RW(sfp_ospr0,   0x10),
    BIN_REG_RW(sfp_ospr1,   0x14),
    BIN_REG_RW(sfp_dcvr0,   0x18),
    BIN_REG_RW(sfp_dcvr1,   0x1C),
};

static struct bin_attribute sfp_arrays[] = {
    BIN_ARRAY_RW(drvr,  "sfp_drvr",  8),
    BIN_ARRAY_RW(otpmk, "sfp_otpmk", 32),
    BIN_ARRAY_RW(srkh,  "sfp_srkh",  32),
    BIN_ARRAY_RW(ouid,  "sfp_ouid",  20),
};

/* --- 5. Grouping and Initialization --- */

static struct attribute *sfp_attrs[] = { 
    &sfp_arm.attr, 
    &sfp_burn.attr, 
    NULL 
};

static struct bin_attribute *sfp_bin_list[ARRAY_SIZE(sfp_regs) + ARRAY_SIZE(sfp_arrays) + 1];

static const struct attribute_group sfp_group = { 
    .attrs = sfp_attrs, 
    .bin_attrs = sfp_bin_list 
};

static struct kobject *sfp_kobj;

static int __init sfp_init(void) {
    int i, j = 0;
    
    // Populate the flattened list of binary attributes
    for (i = 0; i < ARRAY_SIZE(sfp_regs); i++) 
        sfp_bin_list[j++] = &sfp_regs[i].bin_attr;
    for (i = 0; i < ARRAY_SIZE(sfp_arrays); i++) 
        sfp_bin_list[j++] = &sfp_arrays[i];
    sfp_bin_list[j] = NULL;

    // Create /sys/sfp/
    sfp_kobj = kobject_create_and_add("sfp", NULL);
    if (!sfp_kobj) return -ENOMEM;

    return sysfs_create_group(sfp_kobj, &sfp_group);
}

static void __exit sfp_exit(void) { 
    kobject_put(sfp_kobj); 
}

module_init(sfp_init);
module_exit(sfp_exit);
MODULE_LICENSE("GPL");

