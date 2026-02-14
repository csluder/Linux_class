#include <linux/module.h>
#include <linux/platform_device.h>

static int split_probe(struct platform_device *pdev) {
    dev_info(&pdev->dev, "Probe routine triggered in driver_core.\n");
    return 0;
}

static void split_remove(struct platform_device *pdev) {
    dev_info(&pdev->dev, "Remove routine triggered in driver_core.\n");
}

/* Define the structure */
struct platform_driver split_exported_driver = {
    .probe = split_probe,
    .remove = split_remove,
    .driver = {
        .name = "split_device",
        .owner = THIS_MODULE,
    },
};

/* EXPORT the structure symbol for the second module */
EXPORT_SYMBOL(split_exported_driver);

MODULE_LICENSE("GPL");
