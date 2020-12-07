ifeq ($(KERNELRELEASE),)
	KERNELDIR ?= ~/linux-develop/linux-4.19.78/
	PWD ?= $(shell pwd)
modules:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules
modules_install:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules_install
clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
else
#	obj-m := one-wire.o
#	obj-m := mini6410_1wire_interrupt.o
	obj-m := mini6410_1wire_hrtimer.o
endif
