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

static const struct blk_mq_ops ramjam_mq_ops = {
	.queue_rq = ramjam_queue_rq,
};

/* 6.12+ Upstream: Minimal block operations are now mandatory */
static const struct block_device_operations ramjam_bdops = {
	.owner = THIS_MODULE,
};

static int ramjam_mmap(struct file* filp, struct vm_area_struct* vma) {
#ifdef OLD_WAY
	static int ramjam_mmap(struct file* file, struct vm_area_struct* vma)
	{
		char* vpage;
		u32 offset;
		int size;
		int i;
		unsigned long pfn;
		int count;

		offset = vma->vm_pgoff << PAGE_SHIFT;
		size = vma->vm_end - vma->vm_start - offset;
		count = size >> PAGE_SHIFT;

		vpage = &ramjam_dev.buffer[offset];
		if (!is_vmalloc_addr(vpage)) {
			pfn = virt_to_phys(vpage) >> PAGE_SHIFT;
			remap_pfn_range(vma, vma->vm_start + offset, pfn, size, vma->vm_page_prot);
			for (i = 0; i < count * PAGE_SIZE; i += PAGE_SIZE) {
				get_page(virt_to_page(((unsigned long)vpage) + i));
			}
		}
		else {
			for (; size; vpage += PAGE_SIZE) {

				pfn = vmalloc_to_pfn(vpage);
				get_page(vmalloc_to_page(vpage));
				remap_pfn_range(vma, vma->vm_start + offset, pfn, PAGE_SIZE, vma->vm_page_prot);


				size -= PAGE_SIZE;
				offset += PAGE_SIZE;
			}
		}

		return 0;
	}
#endif /* OLD_WAY */

	return remap_vmalloc_range(vma, ramjam_dev.buffer, 0);
}

static const struct file_operations ramjam_fops = {
	.owner = THIS_MODULE,
	.mmap = ramjam_mmap,
	.open = nonseekable_open,
};

static int __init ramjam_init(void) {
	/* 6.12+ Upstream: queue_limits MUST be zeroed and include max_hw_sectors */
	struct queue_limits lim = {
		.logical_block_size = SECTOR_SIZE,
		.physical_block_size = SECTOR_SIZE,
		.max_hw_sectors = 1024,
		.max_segments = 128,
		.max_segment_size = PAGE_SIZE,
	};
	int ret;

	/* 1. Major registration remains necessary for sysfs major/minor mapping */
	ramjam_dev.major = register_blkdev(0, "ramjam");
	if (ramjam_dev.major < 0) return ramjam_dev.major;

	/* 2. Memory Allocation */
	ramjam_dev.size = (size_t)ramjam_pages * PAGE_SIZE;
	ramjam_dev.buffer = vmalloc_user(ramjam_dev.size);
	if (!ramjam_dev.buffer) { ret = -ENOMEM; goto err_blkdev; }

	/* 3. Tag Set Setup */
	memset(&ramjam_dev.tag_set, 0, sizeof(ramjam_dev.tag_set));
	ramjam_dev.tag_set.ops = &ramjam_mq_ops;
	ramjam_dev.tag_set.nr_hw_queues = 1;
	ramjam_dev.tag_set.queue_depth = 128;
	ramjam_dev.tag_set.numa_node = NUMA_NO_NODE;
	ramjam_dev.tag_set.flags = BLK_MQ_F_SHOULD_MERGE;

	if (blk_mq_alloc_tag_set(&ramjam_dev.tag_set)) { ret = -ENOMEM; goto err_vm; }

	/* 4. Upstream 6.12 Allocation Pattern */
	ramjam_dev.disk = blk_mq_alloc_disk(&ramjam_dev.tag_set, &lim, &ramjam_dev);
	if (IS_ERR(ramjam_dev.disk)) {
		ret = PTR_ERR(ramjam_dev.disk);
		goto err_tag;
	}

	/* 5. CRITICAL UPSTREAM FIXES.  The kernel is 6.12+ and it panics without these */
	ramjam_dev.disk->major = ramjam_dev.major;
	ramjam_dev.disk->first_minor = 0;
	ramjam_dev.disk->minors = 1;         // Mandatory for device_add_disk
	ramjam_dev.disk->fops = &ramjam_bdops; // Mandatory for internal queue linking
	ramjam_dev.disk->private_data = &ramjam_dev;
	snprintf(ramjam_dev.disk->disk_name, DISK_NAME_LEN, "ramjam0");

	set_capacity(ramjam_dev.disk, ramjam_dev.size >> SECTOR_SHIFT);

	/* 6. Chardev Setup */
	if (alloc_chrdev_region(&ramjam_dev.dev_num, 0, 1, "rramjam")) goto err_disk;
	ramjam_dev.class = class_create("rramjam");
	cdev_init(&ramjam_dev.cdev, &ramjam_fops);
	cdev_add(&ramjam_dev.cdev, ramjam_dev.dev_num, 1);
	device_create(ramjam_dev.class, NULL, ramjam_dev.dev_num, NULL, "rramjam");

	/* Expose the disk to the user level */
	ret = add_disk(ramjam_dev.disk);
	if (ret) {
		/* Upstream add_disk failure requires put_disk, not just del_gendisk */
		goto err_cdev;
	}

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

