obj-m += dm-qcow2.o
dm-qcow2-y := dm-qcow2-target.o dm-qcow2-map.o dm-qcow2-cmd.o

KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
