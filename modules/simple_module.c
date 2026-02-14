#include <linux/module.h>
#include <linux/init.h>
#include <linux/string.h>

/**
 * Module Entry Point
 */
static int __init simple_init(void)
{
    int ret = 0;

    pr_info("test_module: Initializing...\n");

    return ret;
}

/**
 * Module Exit Point
 */
static void __exit simple_exit(void)
{
    pr_info("test_module: Exited.\n");
}

module_init(simple_init);
module_exit(simple_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Charles Sluder");
MODULE_DESCRIPTION("Module with init routine");
