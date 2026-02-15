# Object file to be created (must match your .c filename)
obj-m += spy.o
obj-m += netlink.o
obj-m += simple_netlink.o
obj-m += eeprom.o
obj-m += i2cfw_eeprom.o
obj-m += adxl345_tap.o
obj-m += gpio_example.o
obj-m += tasklet.o
obj-m += hybrid.o
obj-m += workqueue.o
obj-m += threaded_irq.o
obj-m += napi.o

# The KDIR variable should point to your Yocto-built kernel source or SDK headers
# When building on the target (RPi4), this is the standard path:
KDIR ?= /lib/modules/$(shell uname -r)/build

# Current directory
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

# Installation target
install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

