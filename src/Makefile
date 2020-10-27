obj-m := scull.o
scull-objs := main.o

PWD := $(shell pwd)

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
all:
	make -C $(KERNELDIR)  M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
