#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/blk-mq.h>
#include <linux/cdev.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>

#define DEFAULT_PAGES 262144
static unsigned int ramjam_pages = DEFAULT_PAGES;
module_param(ramjam_pages, uint, 0644);

struct general_ramjam {
	struct gendisk* disk;
	struct blk_mq_tag_set tag_set;
	uint8_t* buffer;
	size_t size;
	struct cdev cdev;
	struct class* class;
	dev_t dev_num;
	int major;
} ramjam_dev;

/* --- Block Layer Handler --- */
static blk_status_t ramjam_queue_rq(struct blk_mq_hw_ctx* hctx, const struct blk_mq_queue_data* bd) {
	struct request* rq = bd->rq;
	struct general_ramjam* dev = rq->q->queuedata;
	struct req_iterator iter;
	struct bio_vec bvec;
	loff_t pos = blk_rq_pos(rq) << SECTOR_SHIFT;

	blk_mq_start_request(rq);
	rq_for_each_segment(bvec, rq, iter) {
		void* kaddr = kmap_local_page(bvec.bv_page) + bvec.bv_offset;
		if (rq_data_dir(rq) == WRITE)
			memcpy(dev->buffer + pos, kaddr, bvec.bv_len);
		else
			memcpy(kaddr, dev->buffer + pos, bvec.bv_len);
		pos += bvec.bv_len;
		kunmap_local(kaddr);
	}
	blk_mq_end_request(rq, BLK_STS_OK);
	return BLK_STS_OK;
}

static const struct blk_mq_ops ramjam_mq_ops = { .queue_rq = ramjam_queue_rq };
static const struct block_device_operations ramjam_bdops = { .owner = THIS_MODULE };

/* --- VMA Operations (Demand Paging) --- */

static vm_fault_t ramjam_vma_fault(struct vm_fault* vmf)
{
	struct vm_area_struct* vma = vmf->vma;
	struct general_ramjam* dev = vma->vm_private_data;
	struct page* page;
	unsigned long offset;
	void* vaddr;

	/* Calculate offset into our ramdisk buffer */
	offset = (unsigned long)(vmf->address - vma->vm_start) + (vma->vm_pgoff << PAGE_SHIFT);

	if (offset >= dev->size)
		return VM_FAULT_SIGBUS;

	/* Get the logical address in vmalloc space */
	vaddr = dev->buffer + offset;

	/* Find the 'struct page' associated with this vmalloc address */
	page = vmalloc_to_page(vaddr);
	if (!page)
		return VM_FAULT_SIGBUS;

	/* Increment reference count so the page isn't freed while mapped */
	get_page(page);
	vmf->page = page;

	pr_debug("ramjam: Faulted page at offset %lu\n", offset);

	return 0;
}

static const struct vm_operations_struct ramjam_vm_ops = {
	.fault = ramjam_vma_fault,
};

/* --- Character Device Handlers --- */

static int ramjam_mmap(struct file* filp, struct vm_area_struct* vma) {
	/*
	 * Instead of mapping now, we attach our ops.
	 * The fault handler will be called when the user touches the memory.
	 */
	vma->vm_ops = &ramjam_vm_ops;
	vma->vm_private_data = &ramjam_dev;

	pr_info("ramjam: VMA initialized for demand paging\n");
	return 0;
}

static const struct file_operations ramjam_fops = {
	.owner = THIS_MODULE,
	.mmap = ramjam_mmap,
	.open = nonseekable_open,
};

/* --- Initialization --- */

static int __init ramjam_init(void) {
	struct queue_limits lim = {
		.logical_block_size = SECTOR_SIZE,
		.physical_block_size = SECTOR_SIZE,
		.max_hw_sectors = 1024,
		.max_segments = 128,
		.max_segment_size = PAGE_SIZE,
	};
	int ret;

	ramjam_dev.major = register_blkdev(0, "ramjam");
	if (ramjam_dev.major < 0) return ramjam_dev.major;

	ramjam_dev.size = (size_t)ramjam_pages * PAGE_SIZE;
	ramjam_dev.buffer = vmalloc_user(ramjam_dev.size);
	if (!ramjam_dev.buffer) { ret = -ENOMEM; goto err_blkdev; }

	memset(&ramjam_dev.tag_set, 0, sizeof(ramjam_dev.tag_set));
	ramjam_dev.tag_set.ops = &ramjam_mq_ops;
	ramjam_dev.tag_set.nr_hw_queues = 1;
	ramjam_dev.tag_set.queue_depth = 128;
	ramjam_dev.tag_set.numa_node = NUMA_NO_NODE;
	ramjam_dev.tag_set.flags = BLK_MQ_F_SHOULD_MERGE;

	if (blk_mq_alloc_tag_set(&ramjam_dev.tag_set)) { ret = -ENOMEM; goto err_vm; }

	ramjam_dev.disk = blk_mq_alloc_disk(&ramjam_dev.tag_set, &lim, &ramjam_dev);
	if (IS_ERR(ramjam_dev.disk)) { ret = PTR_ERR(ramjam_dev.disk); goto err_tag; }

	ramjam_dev.disk->major = ramjam_dev.major;
	ramjam_dev.disk->first_minor = 0;
	ramjam_dev.disk->minors = 1;
	ramjam_dev.disk->fops = &ramjam_bdops;
	snprintf(ramjam_dev.disk->disk_name, DISK_NAME_LEN, "ramjam0");
	set_capacity(ramjam_dev.disk, ramjam_dev.size >> SECTOR_SHIFT);

	if (alloc_chrdev_region(&ramjam_dev.dev_num, 0, 1, "rramjam")) goto err_disk;
	ramjam_dev.class = class_create("rramjam");
	cdev_init(&ramjam_dev.cdev, &ramjam_fops);
	cdev_add(&ramjam_dev.cdev, ramjam_dev.dev_num, 1);
	device_create(ramjam_dev.class, NULL, ramjam_dev.dev_num, NULL, "rramjam");

	ret = add_disk(ramjam_dev.disk);
	if (ret) goto err_cdev;

	return 0;

err_cdev:
	device_destroy(ramjam_dev.class, ramjam_dev.dev_num);
	class_destroy(ramjam_dev.class);
	cdev_del(&ramjam_dev.cdev);
	unregister_chrdev_region(ramjam_dev.dev_num, 1);
err_disk:
	put_disk(ramjam_dev.disk);
err_tag:
	blk_mq_free_tag_set(&ramjam_dev.tag_set);
err_vm:
	vfree(ramjam_dev.buffer);
err_blkdev:
	unregister_blkdev(ramjam_dev.major, "ramjam");
	return ret;
}

static void __exit ramjam_exit(void) {
	device_destroy(ramjam_dev.class, ramjam_dev.dev_num);
	class_destroy(ramjam_dev.class);
	cdev_del(&ramjam_dev.cdev);
	unregister_chrdev_region(ramjam_dev.dev_num, 1);
	del_gendisk(ramjam_dev.disk);
	put_disk(ramjam_dev.disk);
	blk_mq_free_tag_set(&ramjam_dev.tag_set);
	vfree(ramjam_dev.buffer);
	unregister_blkdev(ramjam_dev.major, "ramjam");
}

module_init(ramjam_init);
module_exit(ramjam_exit);
MODULE_LICENSE("GPL");
