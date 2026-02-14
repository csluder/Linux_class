#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/machine.h>
#include <linux/interrupt.h>
#include <linux/timer.h>
#include <linux/property.h>      /* Essential for device_get_named_child_node */
#include <linux/mod_devicetable.h>
#include <linux/irq.h>           /* Required for irq_get_irq_data */

/*
 * PRIVATE CONTEXT STRUCTURE
 * In Linux drivers, we encapsulate all "instance" data here.
 * This allows multiple instances of the hardware to coexist.
 */
struct l3harris_ctx {
	struct gpio_desc* red;    /* Opaque handle for LED GPIO */
	struct gpio_desc* blue;
	struct gpio_desc* pir_desc;
	struct timer_list flash_timer;
	struct device* dev;       /* Pointer to the underlying device for logging */
	int irq;                  /* Virtual IRQ number assigned by the kernel */
	bool state;               /* Current toggle state for the blinker */
};

/**
 * KERNEL TIMER HANDLER
 * Timers run in "Interrupt Context" (Atomic). We cannot sleep here.
 * This function handles the periodic blinking and checks the PIR state.
 */
static void flash_timer_handler(struct timer_list* t)
{
	struct l3harris_ctx* ctx = from_timer(ctx, t, flash_timer);

	/*
	 * POLLING FOR FALLING EDGE:
	 * If the interrupt controller misses the falling edge, we manually
	 * verify the physical state. If 0 (Low), we stop rescheduling.
	 */
	if (!gpiod_get_raw_value(ctx->pir_desc)) {
		dev_info(ctx->dev, "PIR Low detected (polling): Cleaning up LEDs\n");
		gpiod_set_value(ctx->red, 0);
		gpiod_set_value(ctx->blue, 0);
		return; /* Stopping the timer chain */
	}

	/* Toggle LEDs using logical values */
	ctx->state = !ctx->state;
	gpiod_set_value(ctx->red, ctx->state);
	gpiod_set_value(ctx->blue, !ctx->state);

	/* Reschedule the timer for 500ms from now */
	mod_timer(&ctx->flash_timer, jiffies + msecs_to_jiffies(500));
}

/**
 * INTERRUPT SERVICE ROUTINE (Threaded)
 * Threaded IRQs are modern best practice. The Hard IRQ (NULL below)
 * just acknowledges the hardware, while this thread does the work.
 */
static irqreturn_t pir_irq_handler(int irq, void* dev_id)
{
	struct l3harris_ctx* ctx = dev_id;

	/* Detect Rising Edge to start the flashing sequence */
	if (gpiod_get_raw_value(ctx->pir_desc)) {
		if (!timer_pending(&ctx->flash_timer)) {
			dev_info(ctx->dev, "PIR Interrupt: Starting LEDs\n");
			/* Jiffies is 10ms on RPI 4 and 5 (HZ = 100) */
			mod_timer(&ctx->flash_timer, jiffies + msecs_to_jiffies(10));
		}
	}
	return IRQ_HANDLED;
}

/**
 * PROBE FUNCTION (The Constructor)
 * Called when the 'compatible' string in the DT matches this driver.
 */
static int l3harris_probe(struct platform_device* pdev)
{
	struct device* dev = &pdev->dev;
	struct fwnode_handle* child, * grandchild;
	struct l3harris_ctx* ctx;
	struct irq_data* idata;
	struct gpio_chip* chip;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx) return -ENOMEM;
	ctx->dev = dev;

	/*
	 * RETRIEVING INFO FROM NESTED DEVICE TREE
	 * We use fwnodes to traverse: Main Node -> Child (leds) -> Grandchild (led_red)
	 */
	device_for_each_child_node(dev, child) {
		if (fwnode_name_eq(child, "leds")) {
			fwnode_for_each_child_node(child, grandchild) {
				if (fwnode_name_eq(grandchild, "led_red"))
					ctx->red = devm_fwnode_gpiod_get_index(dev, grandchild, NULL, 0, GPIOD_OUT_LOW, "red");
				else if (fwnode_name_eq(grandchild, "led_blue"))
					ctx->blue = devm_fwnode_gpiod_get_index(dev, grandchild, NULL, 0, GPIOD_OUT_LOW, "blue");
			}
		}
		/* EXTRACTING IRQ WITHOUT A GPIO ENTRY */
		if (fwnode_name_eq(child, "sr501")) {
			ctx->irq = fwnode_irq_get(child, 0);
		}
	}

	/* Error checking descriptors */
	if (IS_ERR_OR_NULL(ctx->red) || IS_ERR_OR_NULL(ctx->blue))
		return -ENODEV;

	/*
	 * RESOLVING GPIO DESCRIPTOR FROM IRQ
	 * Since the PIR has no 'gpios' property, we manually map IRQ -> hwirq -> desc.
	 */
	idata = irq_get_irq_data(ctx->irq);
	if (!idata || !(chip = irq_data_get_irq_chip_data(idata)))
		return -ENODEV;

	/* Claim the pin manually to allow the timer to poll its state */
	ctx->pir_desc = gpiochip_request_own_desc(chip, idata->hwirq, "pir-sensor",
		GPIO_LOOKUP_FLAGS_DEFAULT, GPIOD_IN);
	if (IS_ERR(ctx->pir_desc)) return PTR_ERR(ctx->pir_desc);

	/* CONFIGURING THE INTERRUPT
	 * IRQF_ONESHOT: Required for threaded IRQs (disables line until thread finishes).
	 * IRQF_TRIGGER_RISING: We want to wake up when motion starts.
	 */
	timer_setup(&ctx->flash_timer, flash_timer_handler, 0);
	ret = devm_request_threaded_irq(dev, ctx->irq, NULL, pir_irq_handler,
		IRQF_TRIGGER_RISING | IRQF_ONESHOT,
		"l3harris-pir", ctx);

	if (ret) {
		gpiochip_free_own_desc(ctx->pir_desc);
		return ret;
	}

	platform_set_drvdata(pdev, ctx);
	return 0;
}

/**
 * REMOVE FUNCTION (The Destructor)
 * Cleanup resources that were not managed by the 'devm_' framework.
 */
static void l3harris_remove(struct platform_device* pdev)
{
	struct l3harris_ctx* ctx = platform_get_drvdata(pdev);
	if (ctx) {
		del_timer_sync(&ctx->flash_timer);
		if (ctx->pir_desc)
			gpiochip_free_own_desc(ctx->pir_desc); /* Manually free 'own' desc */
	}
}

/* DRIVER REGISTRATION DATA */
static const struct of_device_id l3harris_of_match[] = {
	{.compatible = "l3harris,demo-bus" },
	{ }
};
MODULE_DEVICE_TABLE(of, l3harris_of_match);

static struct platform_driver l3harris_driver = {
	.probe = l3harris_probe,
	.remove_new = l3harris_remove, /* remove_new is for void return types in 6.11+ */
	.driver = {
		.name = "l3harris_pir_led",
		.of_match_table = l3harris_of_match
	},
};
module_platform_driver(l3harris_driver);

MODULE_LICENSE("GPL");
