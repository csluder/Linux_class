# Kbuild variable to specify the object file to build
obj-m += caesar.o
obj-m += skel.o
obj-m += kobject.o
obj-m += kref.o
obj-m += luks_crypto.o

# Path to the kernel build directory
KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

# Default target: build the module
all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# Install target: installs to /lib/modules/$(shell uname -r)/updates/ by default
# Kernels 6.1+ changed the install directory from extra to updates.  It can
# be made to install to extra by setting INSTALL_MOD_DIR=extra.
# $(MAKE) -C $(KDIR) M=$(PWD) INSTALL_MOD_DIR=extra modules_install
#
# Find the actual System.map on your Pi
SYSTEM_MAP := /boot/System.map-$(shell uname -r)

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	# If the automatic depmod failed, run it manually here
	depmod -a $(shell uname -r)

# Clean target: removes build artifacts
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

