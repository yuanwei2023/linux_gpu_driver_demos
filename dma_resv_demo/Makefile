obj-m := dma_resv_demo.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

run: all
	-sudo rmmod dma_resv_demo 2>/dev/null
	sudo dmesg -C
	sudo insmod dma_resv_demo.ko
	sudo rmmod dma_resv_demo
	dmesg
