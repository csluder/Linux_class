#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/mtd/mtd.h>
#include <linux/mutex.h>

struct sim_nor_data {
	void *buffer;
	size_t size;
	struct mtd_info mtd;
	struct mutex lock;
};

/* --- MTD Callbacks --- */

static int sim_nor_point(struct mtd_info *mtd, loff_t from, size_t len,
			 size_t *retlen, void **virt, resource_size_t *phys) {
	struct sim_nor_data *data = mtd->priv;
	if (from + len > mtd->size) return -EINVAL;
	*virt = data->buffer + from;
	*retlen = len;
	if (phys) *phys = 0;
	return 0;
}

static int sim_nor_unpoint(struct mtd_info *mtd, loff_t from, size_t len) {
	return 0;
}

static int sim_nor_erase(struct mtd_info *mtd, struct erase_info *instr) {
	struct sim_nor_data *data = mtd->priv;
	if (instr->addr + instr->len > mtd->size) return -EINVAL;
	mutex_lock(&data->lock);
	memset(data->buffer + instr->addr, 0xff, instr->len);
	mutex_unlock(&data->lock);
	return 0;
}

static int sim_nor_read(struct mtd_info *mtd, loff_t from, size_t len,
			size_t *retlen, u_char *buf) {
	struct sim_nor_data *data = mtd->priv;
	if (from + len > mtd->size) return -EINVAL;
	mutex_lock(&data->lock);
	memcpy(buf, data->buffer + from, len);
	*retlen = len;
	mutex_unlock(&data->lock);
	return 0;
}

static int sim_nor_write(struct mtd_info *mtd, loff_t to, size_t len,
			 size_t *retlen, const u_char *buf) {
	struct sim_nor_data *data = mtd->priv;
	if (to + len > mtd->size) return -EINVAL;
	mutex_lock(&data->lock);
	memcpy(data->buffer + to, buf, len);
	*retlen = len;
	mutex_unlock(&data->lock);
	return 0;
}

/* --- Modern Sysfs using  Attribute Groups --- */

static ssize_t flash_size_show(struct device *dev, struct device_attribute *attr, char *buf) {
	struct sim_nor_data *data = dev_get_drvdata(dev);
	return sprintf(buf, "%zu\n", data->size);
}

static ssize_t flash_size_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count) {
	struct sim_nor_data *data = dev_get_drvdata(dev);
	size_t new_size;
	void *new_buf;

	if (kstrtoul(buf, 10, (unsigned long *)&new_size)) return -EINVAL;
        new_size = new_size * data->mtd.erasesize;

	mutex_lock(&data->lock);
	mtd_device_unregister(&data->mtd);

	new_buf = vmalloc(new_size);
	if (!new_buf) {
		/* Attempt to restore original MTD if allocation fails */
		mtd_device_register(&data->mtd, NULL, 0);
		mutex_unlock(&data->lock);
		return -ENOMEM;
	}

	vfree(data->buffer);
	data->buffer = new_buf;
	data->size = new_size;
	data->mtd.size = new_size;

	mtd_device_register(&data->mtd, NULL, 0);
	mutex_unlock(&data->lock);

	dev_info(dev, "Simulated flash resized to %zu bytes\n", new_size);
	return count;
}

static DEVICE_ATTR_RW(flash_size);

static struct attribute *sim_nor_attrs[] = {
	&dev_attr_flash_size.attr,
	NULL,
};

static const struct attribute_group sim_nor_group = {
	.attrs = sim_nor_attrs,
};

static const struct attribute_group *sim_nor_groups[] = {
	&sim_nor_group,
	NULL,
};

/* --- Platform Driver Routines --- */

static int sim_nor_probe(struct platform_device *pdev) {
	struct sim_nor_data *data;
	int ret;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data) return -ENOMEM;

	data->size = 64 * 1024; // 64KB Default
	data->buffer = vmalloc(data->size);
	if (!data->buffer) return -ENOMEM;

	mutex_init(&data->lock);
	platform_set_drvdata(pdev, data);

	/* Setup MTD structure */
	data->mtd.name = "sim_nor_flash";
	data->mtd.type = MTD_NORFLASH;
	data->mtd.flags = MTD_CAP_NORFLASH;
	data->mtd.size = data->size;
	data->mtd.erasesize = 4096;
	data->mtd.writesize = 1;
	data->mtd.owner = THIS_MODULE;
	data->mtd.priv = data;
	data->mtd._erase = sim_nor_erase;
	data->mtd._read = sim_nor_read;
	data->mtd._write = sim_nor_write;
	data->mtd._point = sim_nor_point;
	data->mtd._unpoint = sim_nor_unpoint;
	data->mtd.dev.parent = &pdev->dev;

	ret = mtd_device_register(&data->mtd, NULL, 0);
	if (ret) {
		vfree(data->buffer);
		return ret;
	}

	dev_info(&pdev->dev, "Simulated MTD NOR Probed Successfully\n");
	return 0;
}

static void sim_nor_remove(struct platform_device *pdev) {
	struct sim_nor_data *data = platform_get_drvdata(pdev);
	if (data) {
		mtd_device_unregister(&data->mtd);
		vfree(data->buffer);
	}
	dev_info(&pdev->dev, "Simulated MTD NOR Released\n");
}

static struct platform_driver sim_nor_driver = {
	.probe = sim_nor_probe,
	.remove = sim_nor_remove,
	.driver = { 
		.name = "sim_nor", 
		.owner = THIS_MODULE,
		.dev_groups = sim_nor_groups,
	},
};

/* --- Module Init/Exit --- */

static struct platform_device *sim_nor_device;

static int __init sim_nor_init(void) {
	int ret = platform_driver_register(&sim_nor_driver);
	if (ret) return ret;

	sim_nor_device = platform_device_register_simple("sim_nor", -1, NULL, 0);
	if (IS_ERR(sim_nor_device)) {
		platform_driver_unregister(&sim_nor_driver);
		return PTR_ERR(sim_nor_device);
	}
	return 0;
}

static void __exit sim_nor_exit(void) {
	platform_device_unregister(sim_nor_device);
	platform_driver_unregister(&sim_nor_driver);
}

module_init(sim_nor_init);
module_exit(sim_nor_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Modern Simulated MTD NOR Driver for 6.12+");

