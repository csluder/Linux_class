# Object file to be created (must match your .c filename)

obj-m += adxl345_tap.o


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

