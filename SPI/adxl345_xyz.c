#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/spi/spi.h>
#include <linux/of.h>
#include <linux/gpio/consumer.h>
#include <linux/property.h>

#include "adxl345.h"

#define ADXL345_MAX_SPI_FREQ_HZ         5000000
#define MAX_FREQ_NO_FIFODELAY	1500000
#define ADXL345_CMD_MULTB	(1 << 6)
#define ADXL345_CMD_READ	(1 << 7)
#define CMD_MASK  0x3f
#define ADXL345_WRITECMD(reg)	(reg & 0x3F)
#define ADXL345_READCMD(reg)	(ADXL345_CMD_READ | (reg & 0x3F))
#define ADXL345_READMB_CMD(reg) (ADXL345_CMD_READ | ADXL345_CMD_MULTB \
					| (reg & 0x3F))

struct spi_device *g_spi;
static const struct adxl34x_platform_data adxl34x_default_init = {
	.tap_threshold = 35,
	.tap_duration = 3,
	.tap_latency = 20,
	.tap_window = 20,
	.tap_axis_control = ADXL_TAP_X_EN | ADXL_TAP_Y_EN | ADXL_TAP_Z_EN,
	.act_axis_control = 0xFF,
	.activity_threshold = 6,
	.inactivity_threshold = 4,
	.inactivity_time = 3,
	.free_fall_threshold = 8,
	.free_fall_time = 0x20,
	.data_rate = 8,
	.data_range = ADXL_FULL_RES,

	.ev_code_tap = {BTN_TOUCH, BTN_TOUCH, BTN_TOUCH}, /* EV_KEY {x,y,z} */
	.power_mode = ADXL_AUTO_SLEEP | ADXL_LINK,
	.fifo_mode = ADXL_FIFO_STREAM,
	.watermark = 0,
};

static inline int adxl345_spi_read(struct spi_device *spi, u8 reg)
{
	return spi_w8r8(spi, ADXL345_READCMD(reg));
}

static int adxl345_spi_write(struct spi_device *spi,
			     unsigned char reg, unsigned char val)
{
	unsigned char buf[2];

	buf[0] = reg & CMD_MASK;
	buf[1] = val;

	return spi_write(spi, buf, sizeof(buf));
}

static int adxl345_spi_read_block(struct spi_device *spi,
				  unsigned char reg, int count,
				  void *buf)
{
	ssize_t status;

	reg = (ADXL345_CMD_READ | ADXL345_CMD_MULTB | (reg & 0x3F));
	status = spi_write_then_read(g_spi, &reg, 1, buf, count);

	return (status < 0) ? status : 0;
}

static ssize_t adxl345_devid_show(struct device *dev,
                                 struct device_attribute *attr, char *buf)
{
	unsigned int devid;
	ssize_t count;

	devid = adxl345_spi_read(g_spi, ADXL345_REG_DEVID);
        count = sprintf(buf, "0x%x\n", devid);

	return count;
}

static DEVICE_ATTR(devid, 0444, adxl345_devid_show, NULL);

static ssize_t adxl345_position_show(struct device *dev,
                                 struct device_attribute *attr, char *buf)
{
        ssize_t count;
	__le16 axis[3];
	int status;

	status = adxl345_spi_read_block(g_spi, ADXL345_DATAX0, ADXL345_DATAZ1 - ADXL345_DATAX0 + 1, axis);

        count = sprintf(buf, "(%d,%d,%d)\n", le16_to_cpu(axis[0]), le16_to_cpu(axis[1]), le16_to_cpu(axis[2]));

        return count;
}

static DEVICE_ATTR(position, 0444, adxl345_position_show, NULL);

static ssize_t adxl345_enable_store(struct device *dev,
                        struct device_attribute *attr, const char *buf, size_t count)
{
        int error;
        unsigned int val;
        unsigned int power_mode;

        error = kstrtouint(buf, 10, &val);
        if (error)
                return error;

	g_spi->max_speed_hz = MAX_FREQ_NO_FIFODELAY;
	spi_setup(g_spi);

        if (val) {
                // Use autosleep mode. Page 13 ADXL345 Data Sheet
                power_mode = ADXL345_POWER_CTL_MEASURE |
                             ADXL345_POWER_CTL_LINK |
                             ADXL345_POWER_CTL_SLEEP;
        } else {
                power_mode = ADXL345_POWER_CTL_STANDBY;
        }

	adxl345_spi_write(g_spi, ADXL345_POWER_CTL, power_mode);
	adxl345_spi_write(g_spi, ADXL345_DATA_FORMAT, 11);

        return count;
}

static DEVICE_ATTR(enable, 0664, NULL, adxl345_enable_store);

static struct attribute *adxl345_attributes[] = {
	&dev_attr_devid.attr,
        &dev_attr_position.attr,
        &dev_attr_enable.attr,
        NULL
};

static const struct attribute_group adxl345_attr_group = {
        .attrs = adxl345_attributes,
};


static int adxl345_spi_probe(struct spi_device *spi)
{
	const struct adxl34x_platform_data __maybe_unused *pdata = &adxl34x_default_init;
        const struct spi_device_id __maybe_unused *id = spi_get_device_id(spi);
	struct gpio_desc *cs_gpiod;
	int status;

	g_spi = spi;
	status = sysfs_create_group(&spi->dev.kobj, &adxl345_attr_group);

	dev_info(&spi->dev, "adxl345 IRQ = %d\n", spi->irq);
	dev_info(&spi->dev, "adxl345 mode = %d\n", spi->mode);
	dev_info(&spi->dev, "adxl345 cs = %d\n", spi_get_chipselect(spi, 0));
	
	/* Get CS GPIO descriptor (replaces deprecated cs_gpio) */
	cs_gpiod = spi_get_csgpiod(spi, 0);
	if (cs_gpiod)
		dev_info(&spi->dev, "gpio descriptor = %p\n", cs_gpiod);
	
	dev_info(&spi->dev, "bits per word %d\n", spi->bits_per_word);
	
	/* Note: In kernel 6.12, statistics are per-CPU and require special handling
	 * Accessing them directly is not straightforward, so we skip detailed stats */
	dev_info(&spi->dev, "SPI device initialized successfully\n");

        /* Bail out if max_speed_hz exceeds 5 MHz */
        if (spi->max_speed_hz > ADXL345_MAX_SPI_FREQ_HZ) {
                dev_err(&spi->dev, "SPI CLK, %d Hz exceeds 5 MHz\n",
                        spi->max_speed_hz);
                return -EINVAL;
        }

	return 0;
}

static void adxl345_spi_remove(struct spi_device *spi)
{
	sysfs_remove_group(&spi->dev.kobj, &adxl345_attr_group);
}

static const struct spi_device_id adxl345_spi_id[] = {
        { "adxl345_spi", 0 },
        { }
};

MODULE_DEVICE_TABLE(spi, adxl345_spi_id);

static const struct of_device_id adxl345_of_match[] = {
        { .compatible = "adi,adxl345_spi" },
        { }
};

MODULE_DEVICE_TABLE(of, adxl345_of_match);

static struct spi_driver adxl345_spi_driver = {
        .driver = {
                .name   = "adxl345_spi",
                .of_match_table = adxl345_of_match,
        },
        .probe          = adxl345_spi_probe,
	.remove		= adxl345_spi_remove,
        .id_table       = adxl345_spi_id,
};

module_spi_driver(adxl345_spi_driver);

MODULE_AUTHOR("Eva Rachel Retuya <eraretuya@gmail.com>");
MODULE_DESCRIPTION("ADXL345 3-Axis Digital Accelerometer SPI driver");
MODULE_LICENSE("GPL");
