#include <linux/module.h>   /* Essential for all loadable kernel modules */
#include <linux/blk-mq.h>   /* Modern Multi-Queue Block Layer (Mandatory in 6.x) */
#include <linux/blkdev.h>   /* Generic Block Device structures (gendisk, etc.) */
#include <linux/cdev.h>     /* Character Device support for the mmap interface */
#include <linux/mm.h>       /* Memory Management for demand paging logic */
#include <linux/slab.h>     /* Kernel memory allocation (kzalloc/vcalloc) */
#include <linux/vmalloc.h>  /* Virtual memory allocation for the page table */
#include <linux/hdreg.h>    /* Block device geometry support (HDIO_GETGEO) */
#include <linux/device.h>   /* Driver model support (classes and automatic /dev) */

#define CHR_NAME "rramjam"  /* Name for the character device (mmap interface) */
#define BLK_NAME "ramjam"   /* Name for the block device (/dev/ramjam0) */

/* Module parameter: Default size is ~1GB (262144 pages * 4KB) */
static unsigned int ramjam_pages = 262144;
module_param(ramjam_pages, uint, 0644);

/* Device Private Structure: Groups all resources for this specific instance */
struct general_ramjam {
    struct gendisk* disk;           /* Block device representation */
    struct blk_mq_tag_set tag_set;  /* blk-mq framework requirements */
    struct mutex mutex;             /* Synchronizes page table access */
    struct page** pages;            /* THE SPARSE PAGE TABLE (pointer to page ptrs) */
    struct cdev cdev;               /* Character device object */
    struct class* class;            /* Sysfs class for automatic /dev node creation */
    int major_blk;
    int major_chr;
} ramjam_dev;

/*
 * PERMISSIONS CALLBACK:
 * Modern 6.12 kernels use 'const struct device' for this signature.
 * This sets /dev/rramjam and /dev/ramjam0 to 0666 (world rw) on creation.
 */
static char* rramjam_devnode(const struct device* dev, umode_t* mode) {
    if (mode) *mode = 0666;
    return NULL;
}

/*
 * CORE DEMAND PAGING LOGIC:
 * Physical RAM is ONLY allocated here when first touched.
 * If 'allocate' is false, it returns NULL (read-from-empty returns zero).
 */
static struct page* ramjam_get_page(unsigned long pgoff, bool allocate) {
    if (pgoff >= ramjam_pages) return NULL;

    if (!ramjam_dev.pages[pgoff] && allocate) {
        /* Allocate a zeroed physical page on demand */
        ramjam_dev.pages[pgoff] = alloc_page(GFP_KERNEL | __GFP_ZERO);
    }
    return ramjam_dev.pages[pgoff];
}

/*
 * MODERN BLK-MQ REQUEST HANDLER:
 * Replaces the old 'request' or 'bio' handlers.
 * It processes a list of segments provided by the block layer.
 */
static blk_status_t ramjam_queue_rq(struct blk_mq_hw_ctx* hctx, const struct blk_mq_queue_data* bd) {
    struct request* rq = bd->rq;
    struct bio_vec bvec;
    struct req_iterator iter;
    /* Translate sector-based position to byte offset */
    uint32_t pos = blk_rq_pos(rq) << SECTOR_SHIFT;

    blk_mq_start_request(rq);
    mutex_lock(&ramjam_dev.mutex);

    /* Iterate through the data segments in this block request */
    rq_for_each_segment(bvec, rq, iter) {
        unsigned long pgoff = (pos + bvec.bv_offset) >> PAGE_SHIFT;
        struct page* page = ramjam_get_page(pgoff, rq_data_dir(rq) == WRITE);
        void* vaddr;

        if (page) {
            /* kmap_local_page is the 6.x standard for short-term mappings */
            vaddr = kmap_local_page(page) + (pos % PAGE_SIZE);
            if (rq_data_dir(rq) == WRITE)
                memcpy(vaddr, page_address(bvec.bv_page) + bvec.bv_offset, bvec.bv_len);
            else
                memcpy(page_address(bvec.bv_page) + bvec.bv_offset, vaddr, bvec.bv_len);
            kunmap_local(vaddr);
        }
        else if (rq_data_dir(rq) == READ) {
            /* Page doesn't exist yet? Return zeros for the read request */
            memzero_page(bvec.bv_page, bvec.bv_offset, bvec.bv_len);
        }
        pos += bvec.bv_len;
    }

    mutex_unlock(&ramjam_dev.mutex);
    blk_mq_end_request(rq, BLK_STS_OK);
    return BLK_STS_OK;
}

static const struct blk_mq_ops ramjam_mq_ops = { .queue_rq = ramjam_queue_rq };

/*
 * MMAP FAULT HANDLER:
 * Invoked when user-space accesses a memory-mapped address not yet in the MMU.
 */
static vm_fault_t ramjam_vma_fault(struct vm_fault* vmf) {
    struct page* page;
    mutex_lock(&ramjam_dev.mutex);

    /* Fetch or allocate physical page at the requested offset */
    page = ramjam_get_page(vmf->pgoff, true);
    if (!page) {
        mutex_unlock(&ramjam_dev.mutex);
        return VM_FAULT_SIGBUS;
    }

    get_page(page);     /* Increment reference count for the MMU */
    vmf->page = page;   /* Link physical page to user virtual address */

    mutex_unlock(&ramjam_dev.mutex);
    return 0;
}

static const struct vm_operations_struct ramjam_vm_ops = { .fault = ramjam_vma_fault };

/* Standard mmap entry point: Sets the custom fault handler for the VMA */
static int ramjam_mmap(struct file* file, struct vm_area_struct* vma) {
    vma->vm_ops = &ramjam_vm_ops;
    vma->vm_private_data = &ramjam_dev;
    return 0;
}

static const struct file_operations ramjam_fops = { .owner = THIS_MODULE, .mmap = ramjam_mmap };
static const struct block_device_operations ramjam_blk_ops = { .owner = THIS_MODULE };

/*
 * INITIALIZATION:
 * Implements the 6.12 "Atomic Queue Limits" and Multi-Major registration.
 */
static int __init ramjam_init(void) {
    dev_t devt;
    int ret;

    /* 6.12 Modern Requirement: Define hardware/software alignment upfront */
    struct queue_limits lim = {
        .logical_block_size = PAGE_SIZE,
        .physical_block_size = PAGE_SIZE,
        .io_min = PAGE_SIZE,
    };

    /* Pre-allocate the page table (array of pointers) - sparse, no physical pages yet */
    ramjam_dev.pages = vcalloc(ramjam_pages, sizeof(struct page*));
    if (!ramjam_dev.pages) return -ENOMEM;
    mutex_init(&ramjam_dev.mutex);

    /* --- Character Device Setup (/dev/rramjam) --- */
    ret = alloc_chrdev_region(&devt, 0, 1, CHR_NAME);
    if (ret < 0) goto err_pages;
    ramjam_dev.major_chr = MAJOR(devt);

    ramjam_dev.class = class_create(CHR_NAME);
    if (IS_ERR(ramjam_dev.class)) {
        ret = PTR_ERR(ramjam_dev.class);
        goto err_chr_region;
    }
    ramjam_dev.class->devnode = rramjam_devnode; /* Enforce 0666 */

    device_create(ramjam_dev.class, NULL, devt, NULL, CHR_NAME);
    cdev_init(&ramjam_dev.cdev, &ramjam_fops);
    cdev_add(&ramjam_dev.cdev, devt, 1);

    /* --- Block Device Setup (/dev/ramjam0) --- */
    ramjam_dev.major_blk = register_blkdev(0, BLK_NAME "_blk");
    if (ramjam_dev.major_blk < 0) {
        ret = ramjam_dev.major_blk;
        goto err_class;
    }

    /* blk-mq Framework configuration */
    ramjam_dev.tag_set.ops = &ramjam_mq_ops;
    ramjam_dev.tag_set.nr_hw_queues = num_online_cpus(); /* Utilize all RPi 5 cores */
    ramjam_dev.tag_set.queue_depth = 128;
    ramjam_dev.tag_set.numa_node = NUMA_NO_NODE;
    ramjam_dev.tag_set.flags = BLK_MQ_F_SHOULD_MERGE;
    ret = blk_mq_alloc_tag_set(&ramjam_dev.tag_set);
    if (ret) goto err_blkdev;

    /* 6.12 Atomic Allocation: Links Tag Set and Queue Limits to Disk */
    ramjam_dev.disk = blk_mq_alloc_disk(&ramjam_dev.tag_set, &lim, &ramjam_dev);
    if (IS_ERR(ramjam_dev.disk)) {
        ret = PTR_ERR(ramjam_dev.disk);
        goto err_tag;
    }

    /* Metadata setup for /dev/ramjam0 */
    ramjam_dev.disk->major = ramjam_dev.major_blk;
    ramjam_dev.disk->first_minor = 0;
    ramjam_dev.disk->minors = 1; /* Upstream patch requirement: Explicit minor range */
    ramjam_dev.disk->fops = &ramjam_blk_ops;
    ramjam_dev.disk->private_data = &ramjam_dev;

    snprintf(ramjam_dev.disk->disk_name, 32, BLK_NAME "0");
    set_capacity(ramjam_dev.disk, (sector_t)ramjam_pages * (PAGE_SIZE / 512));

    /* Add the disk to the system. This triggers the /dev/ramjam0 node creation */
    ret = add_disk(ramjam_dev.disk);
    if (ret) goto err_disk;

    pr_info("rramjam: Nodes /dev/%s and /dev/%s0 initialized (0666)\n", CHR_NAME, BLK_NAME);
    return 0;

    /* --- Sophisticated Error Handling: Reverse Destruction --- */
err_disk:       put_disk(ramjam_dev.disk);
err_tag:        blk_mq_free_tag_set(&ramjam_dev.tag_set);
err_blkdev:     unregister_blkdev(ramjam_dev.major_blk, BLK_NAME "_blk");
err_class:      class_destroy(ramjam_dev.class);
err_chr_region: unregister_chrdev_region(devt, 1);
err_pages:      vfree(ramjam_dev.pages);
    return ret;
}

static void __exit ramjam_exit(void) {
    int i;
    dev_t devt = MKDEV(ramjam_dev.major_chr, 0);

    /* 1. Unregister block device */
    del_gendisk(ramjam_dev.disk);
    put_disk(ramjam_dev.disk);
    blk_mq_free_tag_set(&ramjam_dev.tag_set);
    unregister_blkdev(ramjam_dev.major_blk, BLK_NAME "_blk");

    /* 2. Unregister character device and class */
    device_destroy(ramjam_dev.class, devt);
    class_destroy(ramjam_dev.class);
    cdev_del(&ramjam_dev.cdev);
    unregister_chrdev_region(devt, 1);

    /* 3. Free all physical pages allocated during demand paging */
    for (i = 0; i < ramjam_pages; i++) {
        if (ramjam_dev.pages[i]) __free_page(ramjam_dev.pages[i]);
    }
    vfree(ramjam_dev.pages); /* Free the pointer table */
}

module_init(ramjam_init);
module_exit(ramjam_exit);
MODULE_LICENSE("GPL");

