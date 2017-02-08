CC := gcc -O2

all:
	$(CC) tools/writetest.c -o tools/writetest
	$(CC) tools/show_block_details.c -o tools/show_block_details

clean:
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
