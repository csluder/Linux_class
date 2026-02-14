C_FLAGS := -Wall

obj-m += devtree.o
obj-m += simple_module.o
obj-m += split_reg.o split_core.o

test1-y = simple_module.o
test2-y = devtree.o
reg-y = split_reg.o
core-y = split_core.o

all:
	make -C /usr/lib/modules/`uname -r`/build M=$(PWD) modules

clean:
	make -C /usr/lib/modules/`uname -r`/build  M=$(PWD) clean
