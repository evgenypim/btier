#include <linux/fs.h>
#include <linux/module.h>

static int __init test_init(void)
{
	struct block_device *bdev = lookup_bdev(NULL, 0);
	return 0;
}

static void __exit test_exit(void)
{
}

module_init(test_init);
module_exit(test_exit);
