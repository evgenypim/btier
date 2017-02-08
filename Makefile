KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
CC := gcc -O2

VERSION := 2.0.1
PROG := btier
KERNEL_PKG := /var/lib/dkms/$(PROG)/$(VERSION)/deb/$(PROG)-dkms_$(VERSION)_all.deb

all: modules userspace
install: install_modules install_userspace
uninstall: uninstall_modules uninstall_userspace

dkms_add:
	dkms add ./
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

userspace:
	gzip -c usr/share/man/man1/btier_setup.1 > usr/share/man/man1/btier_setup.1.gz
	gzip -c usr/share/man/man1/btier_inspect.1 > usr/share/man/man1/btier_inspect.1.gz
	$(CC) -D_FILE_OFFSET_BITS=64 cli/btier_setup.c -o cli/btier_setup
	$(CC) -D_FILE_OFFSET_BITS=64 cli/btier_inspect.c -o cli/btier_inspect

modules:
	$(MAKE) -Wall -C $(KDIR) M=$(PWD)/kernel/btier modules

install_modules:
	install -D -m 755 kernel/btier/btier.ko $(DESTDIR)/lib/modules/`uname -r`/kernel/drivers/block/btier.ko
	install -D -m 755 usr/share/initramfs-tools/hooks/btier $(DESTDIR)/usr/share/initramfs-tools/hooks/btier
	install -D -m 755 usr/share/initramfs-tools/scripts/init-premount/btier $(DESTDIR)/usr/share/initramfs-tools/scripts/init-premount/btier
	update-initramfs -u

install_userspace:
	install -D -m 755 -s cli/btier_setup $(DESTDIR)/sbin/btier_setup
	install -D -m 755 -s cli/btier_inspect $(DESTDIR)/sbin/btier_inspect
	install -D -m 755 etc/init.d/btier $(DESTDIR)/etc/init.d/btier
	install -D -m 644 etc/bttab_example $(DESTDIR)/etc/bttab_example
	install -D -m 644 usr/share/man/man1/btier_setup.1.gz $(DESTDIR)/usr/share/man/man1/btier_setup.1.gz
	install -D -m 644 usr/share/man/man1/btier_inspect.1.gz $(DESTDIR)/usr/share/man/man1/btier_inspect.1.gz

uninstall_modules:
	rm $(DESTDIR)/lib/modules/`uname -r`/kernel/drivers/block/btier.ko
	rm $(DESTDIR)/usr/share/initramfs-tools/hooks/btier
	rm $(DESTDIR)/usr/share/initramfs-tools/scripts/init-premount/btier
	update-initramfs -u

uninstall_userspace:
	rm $(DESTDIR)/sbin/btier_setup
	rm $(DESTDIR)/sbin/btier_inspect
	rm $(DESTDIR)/etc/init.d/btier
	rm -f $(DESTDIR)/etc/bttab_example
	rm $(DESTDIR)/usr/share/man/man1/btier_setup.1.gz
	rm $(DESTDIR)/usr/share/man/man1/btier_inspect.1.gz

clean:
	rm -f usr/share/man/man1/btier_inspect.1.gz
	rm -f usr/share/man/man1/btier_setup.1.gz
	rm -f cli/btier_setup
	rm -f cli/btier_inspect
	rm -f kernel/btier/*.c~ kernel/btier/*.o kernel/btier/*.ko 
	rm -f kernel/btier/btier.mod.c kernel/btier/modules.order 
	rm -f kernel/btier/Module.symvers kernel/btier/.*.cmd 
	rm -f kernel/btier/btier.ko.unsigned
	rm -rf kernel/btier/.tmp_versions
	rm -f kernel/btier/Makefile.xen
	rm -f tools/btier.db
	rm -f tools/show_block_details
	rm -f tools/writetest

pretty: 
	cd kernel/btier;$(KDIR)/scripts/Lindent *.c
	cd kernel/btier;$(KDIR)/scripts/Lindent *.h
	cd kernel/btier;rm -f *.c~
	cd kernel/btier;rm -f *.h~
	cd cli;$(KDIR)/scripts/Lindent *.c
	cd cli;rm -f *.c~
