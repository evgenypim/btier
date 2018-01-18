KVER ?= $(shell uname -r)
KDIR := /lib/modules/$(KVER)/build
PWD := $(shell pwd)

obj-m += btier.o
btier-objs :=
btier-objs += btier_common.o
btier-objs += btier_sysfs.o
btier-objs += btier_main.o
btier-objs += btier_request.o

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

install_modules:
	install -D -m 755 kernel/btier/btier.ko $(DESTDIR)/lib/modules/`uname -r`/kernel/drivers/block/btier.ko

uninstall_modules:
	rm $(DESTDIR)/lib/modules/`uname -r`/kernel/drivers/block/btier.ko

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

pretty:
	cd kernel/btier;$(KDIR)/scripts/Lindent *.c
	cd kernel/btier;$(KDIR)/scripts/Lindent *.h
	cd kernel/btier;rm -f *.c~
	cd kernel/btier;rm -f *.h~
	cd cli;$(KDIR)/scripts/Lindent *.c
	cd cli;rm -f *.c~
