VERSION := 2.2.0

CC := gcc -O2

all: userspace
install: install_dkms_src install_userspace

deb:
	$(MAKE) -f ./debian/rules clean
	fakeroot $(MAKE) -f ./debian/rules binary

userspace:
	gzip -c usr/share/man/man1/btier_setup.1 > usr/share/man/man1/btier_setup.1.gz
	gzip -c usr/share/man/man1/btier_inspect.1 > usr/share/man/man1/btier_inspect.1.gz
	$(CC) -D_FILE_OFFSET_BITS=64 cli/btier_setup.c -o cli/btier_setup
	$(CC) -D_FILE_OFFSET_BITS=64 cli/btier_inspect.c -o cli/btier_inspect

install_userspace:
	install -D -m 755 usr/share/initramfs-tools/hooks/btier $(DESTDIR)/usr/share/initramfs-tools/hooks/btier
	install -D -m 755 usr/share/initramfs-tools/scripts/init-premount/btier $(DESTDIR)/usr/share/initramfs-tools/scripts/init-premount/btier
	install -D -m 755 -s cli/btier_setup $(DESTDIR)/sbin/btier_setup
	install -D -m 755 -s cli/btier_inspect $(DESTDIR)/sbin/btier_inspect
	install -D -m 644 etc/bttab_example $(DESTDIR)/etc/bttab_example
	install -D -m 644 etc/init.d/btier $(DESTDIR)/etc/init.d/btier
	install -D -m 644 usr/share/man/man1/btier_setup.1.gz $(DESTDIR)/usr/share/man/man1/btier_setup.1.gz
	install -D -m 644 usr/share/man/man1/btier_inspect.1.gz $(DESTDIR)/usr/share/man/man1/btier_inspect.1.gz

install_dkms_src:
	install -d -m 744 $(DESTDIR)/usr/src/btier-$(VERSION)
	install -m 644 kernel/btier/*.h $(DESTDIR)/usr/src/btier-$(VERSION)/
	install -m 644 kernel/btier/*.c $(DESTDIR)/usr/src/btier-$(VERSION)/
	install -m 644 kernel/btier/dkms.conf $(DESTDIR)/usr/src/btier-$(VERSION)/
	install -m 644 kernel/btier/Makefile $(DESTDIR)/usr/src/btier-$(VERSION)/
	install -d -m 744 $(DESTDIR)/usr/src/btier-$(VERSION)/test_lookup_bdev_func
	install -m 644 kernel/btier/test_lookup_bdev_func/Makefile $(DESTDIR)/usr/src/btier-$(VERSION)/test_lookup_bdev_func/
	install -m 644 kernel/btier/test_lookup_bdev_func/*.c $(DESTDIR)/usr/src/btier-$(VERSION)/test_lookup_bdev_func/


uninstall_userspace:
	rm $(DESTDIR)/sbin/btier_setup
	rm $(DESTDIR)/sbin/btier_inspect
	rm $(DESTDIR)/etc/init.d/btier
	rm -f $(DESTDIR)/etc/bttab_example
	rm $(DESTDIR)/usr/share/man/man1/btier_setup.1.gz
	rm $(DESTDIR)/usr/share/man/man1/btier_inspect.1.gz
	rm -r $(DESTDIR)/usr/src/btier-$(VERSION)

clean:
	$(MAKE) -C kernel/btier clean
	rm -f usr/share/man/man1/btier_inspect.1.gz
	rm -f usr/share/man/man1/btier_setup.1.gz
	rm -f cli/btier_setup
	rm -f cli/btier_inspect

pretty:
	cd kernel/btier;$(KDIR)/scripts/Lindent *.c
	cd kernel/btier;$(KDIR)/scripts/Lindent *.h
	cd kernel/btier;rm -f *.c~
	cd kernel/btier;rm -f *.h~
	cd cli;$(KDIR)/scripts/Lindent *.c
	cd cli;rm -f *.c~
