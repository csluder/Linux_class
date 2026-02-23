#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/cdev.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/mutex.h>
#include <linux/device.h>

#define RAMJAM_NAME "ramjam0"
#define RRAMJAM_NAME "rramjam"

/*
 * PILLAR 1: THE DATA STORE (Sparse Array)
 * We simulate a large disk (1GB) by allocating an array of pointers.
 * No actual RAM is consumed for the data until a page is "touched."
 */
static unsigned int ramjam_pages = 262144;
module_param(ramjam_pages, uint, 0644);

struct general_ramjam {
    struct gendisk* disk;
    struct blk_mq_tag_set tag_set;
    struct mutex mutex;
    struct page** pages;

    struct cdev cdev;
    struct class* chr_class;
    struct device* chr_device;
    int blk_major;
    int chr_major;
} ramjam_dev;

/* --- 2. THE BLOCK INTERFACE (bio-based) --- */
/* This demonstrates how the OS communicates with a disk at the sector level */
static void ramjam_submit_bio(struct bio* bio) {
    struct bio_vec bvec;
    struct bvec_iter iter;
    sector_t sector = bio->bi_iter.bi_sector;

    mutex_lock(&ramjam_dev.mutex);

    /* bio_for_each_segment iterates through the memory buffers in the I/O request */
    bio_for_each_segment(bvec, bio, iter) {
        uint32_t pg_idx = (sector << SECTOR_SHIFT) >> PAGE_SHIFT;
        struct page* page = (pg_idx < ramjam_pages) ? ramjam_dev.pages[pg_idx] : NULL;

        /* DEMAND ALLOCATION: Allocate physical RAM only when a WRITE occurs */
        if (!page && bio_data_dir(bio) == WRITE && pg_idx < ramjam_pages) {
            page = alloc_page(GFP_KERNEL | __GFP_ZERO);
            ramjam_dev.pages[pg_idx] = page;
        }

        if (page) {
            void* vaddr = kmap_local_page(page);
            void* src_dst = kmap_local_page(bvec.bv_page) + bvec.bv_offset;

            if (bio_data_dir(bio) == WRITE)
                memcpy(vaddr + (bvec.bv_offset % PAGE_SIZE), src_dst, bvec.bv_len);
            else
                memcpy(src_dst, vaddr + (bvec.bv_offset % PAGE_SIZE), bvec.bv_len);

            kunmap_local(src_dst);
            kunmap_local(vaddr);
        }
        else if (bio_data_dir(bio) == READ) {
            /* Unallocated regions return zeros (Simulates a fresh disk) */
            memzero_page(bvec.bv_page, bvec.bv_offset, bvec.bv_len);
        }
        sector += (bvec.bv_len >> SECTOR_SHIFT);
    }
    mutex_unlock(&ramjam_dev.mutex);
    bio_endio(bio); // Signal I/O completion
}

static const struct block_device_operations ramjam_blk_ops = {
    .owner = THIS_MODULE,
    .submit_bio = ramjam_submit_bio,
};

/* --- 3. THE CHARACTER INTERFACE (Demand Paging via mmap) --- */

/* The Fault Handler: This is called by the CPU when a process touches a null PTE */
static vm_fault_t ramjam_vm_fault(struct vm_fault* vmf) {
    struct general_ramjam* dev = vmf->vma->vm_private_data;
    uint32_t pg_idx = vmf->pgoff;
    struct page* page;

    if (pg_idx >= ramjam_pages)
        return VM_FAULT_SIGBUS;

    mutex_lock(&dev->mutex);
    page = dev->pages[pg_idx];

    /* DEMAND PAGING: If the page isn't in RAM, allocate it now */
    if (!page) {
        page = alloc_page(GFP_KERNEL | __GFP_ZERO);
        if (!page) {
            mutex_unlock(&dev->mutex);
            return VM_FAULT_OOM;
        }
        dev->pages[pg_idx] = page;
    }

    get_page(page);    // Increment refcount for the hardware mapping
    vmf->page = page;  // "Plug" the page into the process's page table
    mutex_unlock(&dev->mutex);

    return 0;
}

static const struct vm_operations_struct ramjam_vm_ops = {
    .fault = ramjam_vm_fault,
};

static int rramjam_mmap(struct file* filp, struct vm_area_struct* vma) {
    unsigned long size = vma->vm_end - vma->vm_start;

    /* Comparison check against total capacity */
    if (((vma->vm_pgoff << PAGE_SHIFT) + size) > ((unsigned long)ramjam_pages << PAGE_SHIFT))
        return -EINVAL;

    /* Attach our fault handler to this virtual memory area */
    vma->vm_ops = &ramjam_vm_ops;
    vma->vm_private_data = &ramjam_dev;

    return 0;
}

/* --- 4. KERNEL GLUE & SYSTEM INTEGRATION --- */

static const struct file_operations rramjam_fops = {
    .owner = THIS_MODULE,
    .mmap = rramjam_mmap,
};

/* udev callback to set /dev/rramjam permissions to 666 */
static char* rramjam_devnode(const struct device* dev, umode_t* mode) {
    if (mode) *mode = 0666;
    return NULL;
}

static blk_status_t ramjam_queue_rq(struct blk_mq_hw_ctx* hctx, const struct blk_mq_queue_data* bd) {
    return BLK_STS_IOERR;
}
static const struct blk_mq_ops ramjam_mq_ops = { .queue_rq = ramjam_queue_rq };

static int __init ramjam_init(void) {
    int ret;
    dev_t devt;
    struct queue_limits lim = {
        .logical_block_size = PAGE_SIZE,
        .physical_block_size = PAGE_SIZE,
        .io_min = PAGE_SIZE,
        .io_opt = PAGE_SIZE,
        .max_hw_sectors = 1024,
        .max_segments = 64,
    };

    ramjam_dev.pages = vcalloc(ramjam_pages, sizeof(struct page*));
    if (!ramjam_dev.pages) return -ENOMEM;
    mutex_init(&ramjam_dev.mutex);

    /* --- Char Node Setup --- */
    ret = alloc_chrdev_region(&devt, 0, 1, RRAMJAM_NAME);
    if (ret) goto out_free_pages;
    ramjam_dev.chr_major = MAJOR(devt);

    ramjam_dev.chr_class = class_create(RRAMJAM_NAME);
    if (IS_ERR(ramjam_dev.chr_class)) {
        ret = PTR_ERR(ramjam_dev.chr_class);
        goto out_unregister_chr;
    }
    ramjam_dev.chr_class->devnode = rramjam_devnode;

    cdev_init(&ramjam_dev.cdev, &rramjam_fops);
    ret = cdev_add(&ramjam_dev.cdev, devt, 1);
    if (ret) goto out_destroy_class;

    ramjam_dev.chr_device = device_create(ramjam_dev.chr_class, NULL, devt, NULL, RRAMJAM_NAME);
    if (IS_ERR(ramjam_dev.chr_device)) {
        ret = PTR_ERR(ramjam_dev.chr_device);
        goto out_cdev_del;
    }

    /* --- Block Device Setup --- */
    ret = register_blkdev(0, RAMJAM_NAME);
    if (ret < 0) goto out_destroy_device;
    ramjam_dev.blk_major = ret;

    memset(&ramjam_dev.tag_set, 0, sizeof(ramjam_dev.tag_set));
    ramjam_dev.tag_set.ops = &ramjam_mq_ops;
    ramjam_dev.tag_set.nr_hw_queues = 1;
    ramjam_dev.tag_set.queue_depth = 128;
    ramjam_dev.tag_set.numa_node = NUMA_NO_NODE;

    ret = blk_mq_alloc_tag_set(&ramjam_dev.tag_set);
    if (ret) goto out_unregister_blk;

    ramjam_dev.disk = blk_mq_alloc_disk(&ramjam_dev.tag_set, &lim, NULL);
    if (IS_ERR(ramjam_dev.disk)) {
        ret = PTR_ERR(ramjam_dev.disk);
        goto out_free_tags;
    }

    ramjam_dev.disk->major = ramjam_dev.blk_major;
    ramjam_dev.disk->first_minor = 0;
    ramjam_dev.disk->minors = 1;
    ramjam_dev.disk->fops = &ramjam_blk_ops;
    ramjam_dev.disk->private_data = &ramjam_dev;
    snprintf(ramjam_dev.disk->disk_name, 32, RAMJAM_NAME);
    set_capacity(ramjam_dev.disk, (sector_t)ramjam_pages * (PAGE_SIZE / 512));

    ret = add_disk(ramjam_dev.disk);
    if (ret) goto out_put_disk;

    return 0;

out_put_disk:
    put_disk(ramjam_dev.disk);
out_free_tags:
    blk_mq_free_tag_set(&ramjam_dev.tag_set);
out_unregister_blk:
    unregister_blkdev(ramjam_dev.blk_major, RAMJAM_NAME);
out_destroy_device:
    device_destroy(ramjam_dev.chr_class, MKDEV(ramjam_dev.chr_major, 0));
out_cdev_del:
    cdev_del(&ramjam_dev.cdev);
out_destroy_class:
    class_destroy(ramjam_dev.chr_class);
out_unregister_chr:
    unregister_chrdev_region(MKDEV(ramjam_dev.chr_major, 0), 1);
out_free_pages:
    vfree(ramjam_dev.pages);
    return ret;
}

static void __exit ramjam_exit(void) {
    del_gendisk(ramjam_dev.disk);
    put_disk(ramjam_dev.disk);
    blk_mq_free_tag_set(&ramjam_dev.tag_set);
    unregister_blkdev(ramjam_dev.blk_major, RAMJAM_NAME);
    device_destroy(ramjam_dev.chr_class, MKDEV(ramjam_dev.chr_major, 0));
    cdev_del(&ramjam_dev.cdev);
    class_destroy(ramjam_dev.chr_class);
    unregister_chrdev_region(MKDEV(ramjam_dev.chr_major, 0), 1);

    for (int i = 0; i < ramjam_pages; i++)
        if (ramjam_dev.pages[i]) __free_page(ramjam_dev.pages[i]);
    vfree(ramjam_dev.pages);
}

module_init(ramjam_init);
module_exit(ramjam_exit);
MODULE_LICENSE("GPL");
