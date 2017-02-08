CC := gcc -O2

all:
	$(CC) writetest.c -o writetest
	$(CC) show_block_details.c -o show_block_details

clean:
	rm -f btier.db
	rm -f show_block_details
	rm -f writetest

patch_in_tee:
	@:ifndef KERN_TREE
	$(error KERN_TREE is not set)
	@:endif
	./patch-in-tree.sh $(KERN_TREE)
