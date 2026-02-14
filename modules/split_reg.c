#include <linux/module.h>
#include <linux/platform_device.h>

/* Access the structure from the other module */
extern struct platform_driver split_exported_driver;
static struct platform_device *split_pdev;

static int __init activator_init(void) {
    int ret;
    pr_info("Activator: Registering driver and device...\n");

    // Register the driver exported by Module 1
    ret = platform_driver_register(&split_exported_driver);
    if (ret) return ret;

    // Register the device to trigger the probe in Module 1
    split_pdev = platform_device_register_simple("split_device", -1, NULL, 0);
    if (IS_ERR(split_pdev)) {
        platform_driver_unregister(&split_exported_driver);
        return PTR_ERR(split_pdev);
    }

    return 0;
}

static void __exit activator_exit(void) {
    platform_device_unregister(split_pdev);
    platform_driver_unregister(&split_exported_driver);
    pr_info("Activator: Unregistered driver and device.\n");
}

module_init(activator_init);
module_exit(activator_exit);
MODULE_LICENSE("GPL");

