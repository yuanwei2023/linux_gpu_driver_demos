obj-m := ttm_demo.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

run: all
	@sudo rmmod ttm_demo 2>/dev/null || true
	@if lsmod | grep -q '^ttm_demo '; then \
		echo "ttm_demo is still loaded (old instance pinned). Reboot once, then retry."; \
		exit 1; \
	fi
	sudo modprobe ttm
	sudo dmesg -C
	sudo insmod ttm_demo.ko
	sudo rmmod ttm_demo
	dmesg
