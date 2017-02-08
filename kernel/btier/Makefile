KVER ?= $(shell uname -r)
KDIR := /lib/modules/$(KVER)/build
PWD := $(shell pwd)

VERSION := 2.0.1
PROG := btier
KERNEL_PKG := /var/lib/dkms/$(PROG)/$(VERSION)/deb/$(PROG)-dkms_$(VERSION)_all.deb

dkms_add:
	dkms add $(PROG)/$(VERSION) 
	install -D -m 755 usr/share/initramfs-tools/hooks/btier $(DESTDIR)/usr/share/initramfs-tools/hooks/btier
	install -D -m 755 usr/share/initramfs-tools/scripts/init-premount/btier $(DESTDIR)/usr/share/initramfs-tools/scripts/init-premount/btier

dkms_build:
	dkms build $(PROG)/$(VERSION)

dkms_install:
	dkms install $(PROG)/$(VERSION)

dkms_remove:
	rm -f $(DESTDIR)/usr/share/initramfs-tools/hooks/btier
	rm -f $(DESTDIR)/usr/share/initramfs-tools/scripts/init-premount/btier
	dkms remove $(PROG)/$(VERSION) --all
	rm -r /usr/src/$(PROG)-$(VERSION)

dkms_mkdeb:
	mkdir -p ./deb
	dkms mkdeb $(PROG)/$(VERSION) --source-only
	[ -f $(KERNEL_PKG) ] && cp $(KERNEL_PKG) ./deb

modules:
	$(MAKE) -Wall -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -Wall -C $(KDIR) M=$(PWD) clean 

pretty: 
	cd kernel/btier;$(KDIR)/scripts/Lindent *.c
	cd kernel/btier;$(KDIR)/scripts/Lindent *.h
	cd kernel/btier;rm -f *.c~
	cd kernel/btier;rm -f *.h~
	cd cli;$(KDIR)/scripts/Lindent *.c
	cd cli;rm -f *.c~
