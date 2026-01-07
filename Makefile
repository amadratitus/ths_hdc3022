obj-m += hdc3022.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	clear
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	clear
	$(MAKE) -C $(KDIR) M=$(PWD) clean
