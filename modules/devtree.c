#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h> /* For of_device_id */


static char *image = NULL;
module_param(image, charp, 0444);

static void run_check(struct device *dev) {
    if (image)
        dev_info(dev, "Logic executed with image flag: %s\n", image);
    else
        dev_info(dev, "Logic executed. No image flag provided.\n");
}

static int dt_probe(struct platform_device *pdev) {
    dev_info(&pdev->dev, "Probe triggered by Device Tree match!\n");
    run_check(&pdev->dev);
    return 0;
}

static void dt_remove(struct platform_device *pdev) {
    dev_info(&pdev->dev, "Device removed.\n");
}

/*
 *  Define the Match Table
 * The 'compatible' string must match the entry in your .dts file.
 */
static const struct of_device_id dt_ids[] = {
    { .compatible = "l3harris,platform-device" },
    { /* sentinel */ }
};

MODULE_DEVICE_TABLE(of, dt_ids);

/* Link the table to the driver */
struct platform_driver dt_driver = {
    .probe = dt_probe,
    .remove = dt_remove,
    .driver = {
        .name = "device_tree",
        .owner = THIS_MODULE,
        .of_match_table = dt_ids, 
    },
};

static int __init dt_init(void) {
    pr_info("Registering driver to watch for DT hardware...\n");
    return platform_driver_register(&dt_driver);
    return 0;
}

static void __exit dt_exit(void) {
    pr_info("Removing driver\n");
    platform_driver_unregister(&dt_driver);
}

module_init(dt_init);
module_exit(dt_exit);
MODULE_LICENSE("GPL");

