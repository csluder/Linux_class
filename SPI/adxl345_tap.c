#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/interrupt.h>
#include <linux/property.h>
#include <linux/mod_devicetable.h>

/* ADXL345 Register Map */
#define ADXL345_REG_DEVID        0x00
#define ADXL345_REG_THRESH_TAP   0x1D
#define ADXL345_REG_DUR          0x21
#define ADXL345_REG_LATENT       0x22
#define ADXL345_REG_WINDOW       0x23
#define ADXL345_REG_BW_RATE      0x2C
#define ADXL345_REG_POWER_CTL    0x2D
#define ADXL345_REG_INT_ENABLE   0x2E
#define ADXL345_REG_INT_MAP      0x2F
#define ADXL345_REG_INT_SOURCE   0x30
#define ADXL345_REG_DATA_FORMAT  0x31
#define ADXL345_REG_TAP_AXES     0x2A

struct adxl345_data {
	struct spi_device *spi;
};

/* SPI Helper: Read register */
static int adxl345_read(struct spi_device *spi, u8 reg)
{
	u8 addr = reg | 0x80;
	return spi_w8r8(spi, addr);
}

/* SPI Helper: Write register */
static int adxl345_write(struct spi_device *spi, u8 reg, u8 val)
{
	u8 buf[] = { reg & 0x3F, val };
	return spi_write(spi, buf, sizeof(buf));
}

/* Debug Helper: Full register dump */
static void adxl345_dump_regs(struct spi_device *spi, const char *msg)
{
	dev_info(&spi->dev, "--- Debug Snapshot: %s ---\n", msg);
	dev_info(&spi->dev, "POWER: 0x%02x, FORMAT: 0x%02x, BW_RATE: 0x%02x\n",
		 adxl345_read(spi, ADXL345_REG_POWER_CTL),
		 adxl345_read(spi, ADXL345_REG_DATA_FORMAT),
		 adxl345_read(spi, ADXL345_REG_BW_RATE));
	dev_info(&spi->dev, "THRESH: 0x%02x, DUR: 0x%02x, LATENT: 0x%02x\n",
		 adxl345_read(spi, ADXL345_REG_THRESH_TAP),
		 adxl345_read(spi, ADXL345_REG_DUR),
		 adxl345_read(spi, ADXL345_REG_LATENT));
	dev_info(&spi->dev, "INT_EN: 0x%02x, INT_SOURCE: 0x%02x\n",
		 adxl345_read(spi, ADXL345_REG_INT_ENABLE),
		 adxl345_read(spi, ADXL345_REG_INT_SOURCE));
}

/* Threaded IRQ Handler */
static irqreturn_t adxl345_irq_thread(int irq, void *dev_id)
{
	struct adxl345_data *data = dev_id;
	int source = adxl345_read(data->spi, ADXL345_REG_INT_SOURCE);

	if (source < 0)
		return IRQ_NONE;

	/* Debug: Show what triggered the pin */
	dev_info(&data->spi->dev, "IRQ Event (INT_SOURCE: 0x%02x)\n", source);

	if (source & 0x40)
		dev_info(&data->spi->dev, ">>> SUCCESS: SINGLE TAP VALIDATED <<<\n");

	return IRQ_HANDLED;
}

static int adxl345_probe(struct spi_device *spi)
{
	struct adxl345_data *data;
	int ret, id;

	data = devm_kzalloc(&spi->dev, sizeof(*data), GFP_KERNEL);
	if (!data) return -ENOMEM;
	data->spi = spi;

	/* SPI Communication Sanity Check */
	id = adxl345_read(spi, ADXL345_REG_DEVID);
	if (id != 0xE5) {
		dev_err(&spi->dev, "Communication Error: ID 0x%02x (Exp 0xE5)\n", id);
		return -ENODEV;
	}

	/* Configure Hardware with Anti-Hang Logic */
	adxl345_write(spi, ADXL345_REG_POWER_CTL, 0x00);
	
	/* BW_RATE: Lowering to 25Hz (0x08) helps stabilize interrupts */
	adxl345_write(spi, ADXL345_REG_BW_RATE, 0x08);
	adxl345_write(spi, ADXL345_REG_DATA_FORMAT, 0x00);
	
	/* THRESH: 0x28 (approx 2.5g) prevents table bumps from hanging the OS */
	adxl345_write(spi, ADXL345_REG_THRESH_TAP, 0x28);
	adxl345_write(spi, ADXL345_REG_DUR, 0x20);        // 20ms hit window
	
	/* LATENT: 0x50 (~62ms) ensures we ignore the table's resonance "tail" */
	adxl345_write(spi, ADXL345_REG_LATENT, 0x50);
	adxl345_write(spi, ADXL345_REG_TAP_AXES, 0x07);    // Enable X, Y, Z
	
	adxl345_write(spi, ADXL345_REG_INT_MAP, 0x00);     // Everything to INT1
	adxl345_write(spi, ADXL345_REG_INT_ENABLE, 0x40);  // Enable Single Tap
	
	/* Clear state before ARMing */
	adxl345_read(spi, ADXL345_REG_INT_SOURCE);
	adxl345_write(spi, ADXL345_REG_POWER_CTL, 0x08);

	/* Dump state  on load */
	adxl345_dump_regs(spi, "Anti-Hang Configuration");

	/* Register Edge-Rising IRQ (Matches DT <24 1>) */
	ret = devm_request_threaded_irq(&spi->dev, spi->irq, NULL,
					adxl345_irq_thread,
					IRQF_TRIGGER_RISING | IRQF_ONESHOT,
					"adxl345_tap", data);
	if (ret) {
		dev_err(&spi->dev, "IRQ Request failed: %d\n", ret);
		return ret;
	}

	dev_info(&spi->dev, "Unified ADXL345 Driver Probed successfully\n");
	return 0;
}

static const struct of_device_id adxl345_of_match[] = { { .compatible = "adi,adxl345_spi" }, { } };
static const struct spi_device_id adxl345_spi_ids[] = { { "adxl345_spi", 0 }, { } };

static struct spi_driver adxl345_driver = {
	.driver = { 
		.name = "adxl345_unified", 
		.of_match_table = adxl345_of_match,
	},
	.probe = adxl345_probe,
	.id_table = adxl345_spi_ids,
};
module_spi_driver(adxl345_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Presentation Project");

