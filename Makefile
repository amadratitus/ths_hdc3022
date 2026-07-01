obj-m += hdc3022.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f Modules.symvers modules.order

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a

uninstall:
	rm -f /lib/modules/$(shell uname -r)/extra/hdc3022.ko
	depmod -a

check:
	@perl $(PWD)/checkpatch.pl --no-tree --file --terse hdc3022.c

.PHONY: all clean install uninstall check