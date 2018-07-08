KVER ?= $(shell uname -r)
KDIR := /lib/modules/$(KVER)/build
PWD := $(shell pwd)

obj-m += test_lookup_bdev_2_arg.o

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(info $$obj-m is [${obj-m}])
	$(MAKE) -C $(KDIR) M=$(PWD) clean
