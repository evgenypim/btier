/*
 * Btier : Tiered storage made easy.
 * Btier allows to create a virtual blockdevice that consists of multiple
 * physical devices. A common configuration would be to use SSD/SAS/SATA.
 *
 * Partly based up-on sbd and the loop driver.
 *
 * Redistributable under the terms of the GNU GPL.
 * Author: Mark Ruijter, mruijter@gmail.com
 *
 *
 * Btier2: bio make_request path rewrite to handle parallel bio requests, new
 * per-block fine grained locking mechanism; tier data moving rewrite to work
 * with other make_request devices better, such as mdraid10; VFS mode removed,
 * aio_thread and tier_thread removed; passing sync to all underlying devices,
 * and etc. Copyright (C) 2014 Jianjian Huo, <samuel.huo@gmail.com>
 *
 * Copyright (c) 2017 SoftNAS, LLC
 */

#include "btier.h"
#include "btier_main.h"
#include <linux/random.h>

#define TIER_VERSION "3.0.1"

MODULE_VERSION(TIER_VERSION);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mark Ruijter");
MODULE_AUTHOR("Evgeny Pimenov");

LIST_HEAD(device_list);
DEFINE_MUTEX(tier_devices_mutex);
struct workqueue_struct *btier_wq;

/*
 * The internal representation of our device.
 */
static char *devicenames;
static struct mutex ioctl_mutex;
static DEFINE_SPINLOCK(uselock);

static int tier_device_count(void)
{
	struct list_head *pos;
	int count = 0;

	list_for_each(pos, &device_list) { count++; }
	return count;
}

/*
 * Open and close.
 */
static int tier_open(struct block_device *bdev, fmode_t mode)
{
	struct tier_device *dev;

	dev = bdev->bd_inode->i_bdev->bd_disk->private_data;
	spin_lock(&uselock);
	dev->users++;
	spin_unlock(&uselock);
	return 0;
}

void set_debug_info(struct tier_device *dev, int state)
{
#ifndef MAX_PERFORMANCE
	spin_lock(&dev->dbg_lock);
	dev->debug_state |= state;
	spin_unlock(&dev->dbg_lock);
#endif
}

void clear_debug_info(struct tier_device *dev, int state)
{
#ifndef MAX_PERFORMANCE
	spin_lock(&dev->dbg_lock);
	if (dev->debug_state & state)
		dev->debug_state ^= state;
	spin_unlock(&dev->dbg_lock);
#endif
}

static void tier_release(struct gendisk *gd, fmode_t mode)
{
	struct tier_device *dev;

	dev = gd->private_data;
	spin_lock(&uselock);
	dev->users--;
	spin_unlock(&uselock);
}

/*
 * The device operations structure.
 */
static struct block_device_operations tier_ops = {
    .open = tier_open, .release = tier_release, .owner = THIS_MODULE,
};

extern struct attribute *tier_attrs[];

static struct attribute_group tier_attribute_group = {
    .name = "tier", .attrs = tier_attrs,
};

static int tier_sysfs_init(struct tier_device *dev)
{
	return sysfs_create_group(&disk_to_dev(dev->gd)->kobj,
				  &tier_attribute_group);
}

void btier_lock(struct tier_device *dev)
{
	atomic_set(&dev->migrate, MIGRATION_IO);
	down_write(&dev->qlock);
	if (0 != atomic_read(&dev->aio_pending))
		wait_event(dev->aio_event, 0 == atomic_read(&dev->aio_pending));
}

void btier_unlock(struct tier_device *dev)
{
	atomic_set(&dev->migrate, NO_MIGRATION);
	up_write(&dev->qlock);
}

void btier_clear_statistics(struct tier_device *dev)
{
	u64 curblock;
	u64 blocks = dev->size >> BLK_SHIFT;
	int i;
	struct blockinfo *binfo = NULL;

	btier_lock(dev);

	for (curblock = 0; curblock < blocks; curblock++) {
		binfo = get_blockinfo(dev, curblock, 0);
		if (dev->inerror) {
			break;
		}
		if (binfo->device != 0) {
			atomic64_set(&binfo->total_hits, 0);
			atomic64_set(&binfo->hits_ts, 0);
		}
	}
	for (i = 0; i < dev->attached_devices; i++) {
		atomic64_set(&dev->backdev[i]->devmagic->total_hits, 0);
	}
	atomic64_set(&dev->total_hits, 0);

	btier_unlock(dev);
}

static void tier_sysfs_exit(struct tier_device *dev)
{
	sysfs_remove_group(&disk_to_dev(dev->gd)->kobj, &tier_attribute_group);
}

u64 get_average_hits(struct backing_device *backdev)
{
	u64 total_hits = atomic64_read(&backdev->devmagic->total_hits);
	return btier_div(total_hits, backdev->devicesize >> BLK_SHIFT);
}

void copy_data_policy(struct data_policy *dtapolicy, struct physical_data_policy *phy_dtapolicy)
{
	phy_dtapolicy->keep_free = atomic_read(&dtapolicy->keep_free);
	phy_dtapolicy->sequential_landing = atomic_read(&dtapolicy->sequential_landing);
	phy_dtapolicy->migration_enabled = atomic_read(&dtapolicy->migration_enabled);
	phy_dtapolicy->migration_interval = atomic64_read(&dtapolicy->migration_interval);
}

void copy_physical_data_policy(struct physical_data_policy *phy_dtapolicy, struct data_policy *dtapolicy)
{
	atomic_set(&dtapolicy->keep_free, phy_dtapolicy->keep_free);
	atomic_set(&dtapolicy->sequential_landing, phy_dtapolicy->sequential_landing);
	atomic_set(&dtapolicy->migration_enabled, phy_dtapolicy->migration_enabled);
	atomic64_set(&dtapolicy->migration_interval, phy_dtapolicy->migration_interval);
}

/* copy devicemagic to physical_devicemagic */
void copy_devicemagic(struct devicemagic *dmagic, struct physical_devicemagic *phy_dmagic)
{
	phy_dmagic->magic = dmagic->magic;
	phy_dmagic->device = dmagic->device;
	phy_dmagic->clean = dmagic->clean;
	phy_dmagic->blocknr_journal = dmagic->blocknr_journal;
	phy_dmagic->binfo_journal_new = dmagic->binfo_journal_new;
	phy_dmagic->binfo_journal_old = dmagic->binfo_journal_old;

	phy_dmagic->devicesize = atomic64_read(&dmagic->devicesize);

	phy_dmagic->total_device_size = dmagic->total_device_size;
	phy_dmagic->total_bitlist_size = dmagic->total_bitlist_size;
	phy_dmagic->bitlistsize = dmagic->bitlistsize;
	phy_dmagic->blocklistsize = atomic64_read(&dmagic->blocklistsize);
	phy_dmagic->startofbitlist = dmagic->startofbitlist;
	phy_dmagic->startofblocklist = atomic64_read(&dmagic->startofblocklist);

	copy_data_policy(&dmagic->dtapolicy, &phy_dmagic->dtapolicy);

	memcpy(phy_dmagic->fullpathname, dmagic->fullpathname, 1025);
	memcpy(phy_dmagic->uuid, dmagic->uuid, UUID_LEN);
}

/* copy physical_devicemagic to devicemagic */
void copy_physical_devicemagic(struct physical_devicemagic *phy_dmagic, struct devicemagic *dmagic)
{
	dmagic->magic = phy_dmagic->magic;
	dmagic->device = phy_dmagic->device;
	dmagic->clean = phy_dmagic->clean;
	dmagic->blocknr_journal = phy_dmagic->blocknr_journal;
	dmagic->binfo_journal_new = phy_dmagic->binfo_journal_new;
	dmagic->binfo_journal_old = phy_dmagic->binfo_journal_old;

	atomic64_set(&dmagic->devicesize, phy_dmagic->devicesize);

	dmagic->total_device_size = phy_dmagic->total_device_size;
	dmagic->total_bitlist_size = phy_dmagic->total_bitlist_size;
	dmagic->bitlistsize = phy_dmagic->bitlistsize;
	atomic64_set(&dmagic->blocklistsize, phy_dmagic->blocklistsize);
	dmagic->startofbitlist = phy_dmagic->startofbitlist;
	atomic64_set(&dmagic->startofblocklist, phy_dmagic->startofblocklist);

	copy_physical_data_policy(&phy_dmagic->dtapolicy, &dmagic->dtapolicy);
//FIX: BUG: WTF 1025 !!!
	memcpy(dmagic->fullpathname, phy_dmagic->fullpathname, 1025);
	memcpy(dmagic->uuid, phy_dmagic->uuid, UUID_LEN);
}

static struct devicemagic *read_device_magic(struct tier_device *dev,
					     int device,
					     struct devicemagic *dmagic)
{
	struct physical_devicemagic *phy_dmagic;
	
	phy_dmagic = kzalloc(sizeof(struct physical_devicemagic), GFP_NOFS);
	if (phy_dmagic == NULL){
		pr_info("cannot allocate memory for physical_devicemagic device %u", device);
		EXIT_FUNC;
		return NULL;
	}

	if (dmagic == NULL)
	    dmagic = kzalloc(sizeof(struct devicemagic), GFP_NOFS);
	if (dmagic == NULL) {
		EXIT_FUNC;
		return NULL;
	}

	tier_file_read(dev, device, phy_dmagic, sizeof(*phy_dmagic), 0);
	
	if (phy_dmagic->magic != TIER_DEVICE_BIT_MAGIC) {
		const char *devicename =
		    dev->backdev[device]->fds->f_path.dentry->d_name.name;
		pr_warn("read_device_magic : device %s missing magic\n",
			  devicename);
	}

	copy_physical_devicemagic(phy_dmagic, dmagic);

	kfree(phy_dmagic);
	return dmagic;
}

static void write_device_magic(struct tier_device *dev, int device)
{
	int res;
	struct backing_device *backdev = dev->backdev[device];
	struct devicemagic *dmagic = backdev->devmagic;
	struct physical_devicemagic *phy_dmagic;


	/* Make copy rather than hold lock over write */
	phy_dmagic = kzalloc(sizeof(struct physical_devicemagic), GFP_NOFS);
	if (phy_dmagic == NULL) {
		pr_err("write_device_magic : unable to alloc physical_devicemagic buf for device %u\n", device);
		EXIT_FUNC;
		return;
	}

	if (dmagic->magic != TIER_DEVICE_BIT_MAGIC)
		pr_warn("write_device_magic : device %u bad devmagic\n",
		        device);

	down_read(&backdev->magic_lock);
	copy_devicemagic(dmagic, phy_dmagic);
	up_read(&backdev->magic_lock);

	res = tier_file_write(dev, device, phy_dmagic, sizeof(*phy_dmagic), 0);

	if (res != 0)
		pr_err("write_device_magic : unable to write magic for "
			 "device %u\n", device);

	res = vfs_fsync_range(backdev->fds, 0, sizeof(phy_dmagic), 0);
	if (res != 0)
		pr_err("write_device_magic : unable to sync magic for "
			 "device %u\n", device);

	kfree(phy_dmagic);
}

static int mark_offset_as_used(struct tier_device *dev, int device, u64 offset)
{
	u64 boffset;
	u64 bloffset;
	u8 allocated = ALLOCATED;
	struct backing_device *backdev = dev->backdev[device];
	int ret;

	boffset = offset >> BLK_SHIFT;
	bloffset = backdev->startofbitlist + boffset;

	ret = tier_file_write(dev, device, &allocated, 1, bloffset);
	vfs_fsync_range(backdev->fds, bloffset, bloffset + 1, FSMODE);

	spin_lock(&backdev->dev_alloc_lock);
	backdev->bitlist[boffset] = allocated;
	spin_unlock(&backdev->dev_alloc_lock);

	return ret;
}

/* Find free block, return number, FREE_LIST_END if no free blocks */
u64 allocate_backdev_block(struct backing_device *backdev) {
	u64 blocknr = backdev->first_free;
	if ( FREE_LIST_END != backdev->first_free ) {
		backdev->first_free = backdev->free_offset_list[blocknr];
		backdev->free_offset_list[blocknr] = FREE_LIST_ALLOCATED_MARK;
	}
	
	return blocknr;
}

/* free allocated block return blocknr if success or FREE_LIST_END */
u64 free_backdev_block(struct backing_device *backdev, u64 blocknr) {

	if ( FREE_LIST_ALLOCATED_MARK != backdev->free_offset_list[blocknr] ) {
		pr_crit("free_backdev_block: device: %s blocknr: %llu backdev->free_offset_list[blocknr]: %lld "
				"FREE_LIST_ALLOCATED_MARK != backdev->free_offset_list[blocknr]", backdev->fds->f_path.dentry->d_name.name, blocknr , backdev->free_offset_list[blocknr]);
		return FREE_LIST_END;
	}

	backdev->free_offset_list[blocknr] = backdev->first_free;
	backdev->first_free = blocknr;
	return blocknr;
}

void clear_dev_list(struct tier_device *dev, struct blockinfo *binfo)
{
	u64 offset;
	u64 boffset;
	u64 bloffset;
	u8 unallocated = UNALLOCATED;
	struct backing_device *backdev = dev->backdev[binfo->device - 1];

	offset = binfo->offset - backdev->startofdata;
	boffset = offset >> BLK_SHIFT;
	bloffset = backdev->startofbitlist + boffset;

	tier_file_write(dev, binfo->device - 1, &unallocated, 1, bloffset);
	vfs_fsync_range(backdev->fds, bloffset, bloffset + 1, FSMODE);

	spin_lock(&backdev->dev_alloc_lock);
	if ( free_backdev_block(backdev, boffset) != boffset ) {
		pr_crit("clear_dev_list: free_backdev_block failed");
		tiererror(dev, "free_offset_list corruption detected");
	}

	if (backdev->bitlist) {
		backdev->bitlist[boffset] = unallocated;
		atomic64_dec(&backdev->allocated_blocks);
	}
	spin_unlock(&backdev->dev_alloc_lock);
}

int allocate_dev(struct tier_device *dev, u64 blocknr, struct blockinfo *binfo, int device)
{
	struct backing_device *backdev = dev->backdev[device];
	u64 relative_offset = 0;
	int ret = 0;
	u64 dev_blocknr;

	if ( 0 == binfo->device ) {
		spin_lock(&backdev->dev_alloc_lock);
		dev_blocknr = allocate_backdev_block(backdev);
		spin_unlock(&backdev->dev_alloc_lock);
#ifdef VERBOSE_DEBUG
		pr_info("allocate_dev device: %d blocknr: %llu dev_blocknr: %lld", device, blocknr, dev_blocknr);
#endif
		if ( FREE_LIST_END != dev_blocknr) {
			relative_offset = ( dev_blocknr << BLK_SHIFT );
			binfo->offset = backdev->startofdata + relative_offset;
#ifdef VERBOSE_DEBUG
			pr_info("allocate_dev device: %d blocknr: %llu dev_blocknr: %llu relative_offset: %llu", device, blocknr, dev_blocknr, relative_offset);
#endif
			if (binfo->offset + BLKSIZE > backdev->endofdata) {
				pr_info("allocate_dev device: %d blocknr: %llu WTF?????", device, blocknr);
			} else {
				binfo->device = device + 1;
				ret = mark_offset_as_used(dev, device, relative_offset);
				atomic64_inc(&backdev->allocated_blocks);
			}
		} else {
#ifdef VERBOSE_DEBUG
			pr_info("allocate_dev device: %d blocknr: %llu no free blocks", device, blocknr);
#endif
		}
	}
	return ret;
}

static int tier_file_write(struct tier_device *dev, unsigned int device,
			   void *buf, size_t len, loff_t pos)
{
	struct backing_device *backdev = dev->backdev[device];
	ssize_t bw;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,14,0)
	mm_segment_t old_fs = get_fs();
#endif

	set_debug_info(dev, VFSWRITE);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,14,0)
	bw = kernel_write(backdev->fds, buf, len, &pos);
#else
	set_fs(get_ds());
	bw = vfs_write(backdev->fds, buf, len, &pos);
	set_fs(old_fs);
#endif
	clear_debug_info(dev, VFSWRITE);

	/*
	 * there is no need to set dirty, since all meta operations are
	 * synchronized with actual device.
	 */
	// backdev->dirty = 1;
	if (unlikely(pos == 0)) {
		struct devicemagic *dmagic = buf;

		if (len != sizeof(*dmagic) ||
		    dmagic->magic != TIER_DEVICE_BIT_MAGIC)
			pr_warn("tier_file_write : invalid magic for "
			    "device %u\n", device);
	}

	if (likely(bw == len))
		return 0;
	pr_err("Write error on device %s at offset %llu, length %llu\n",
	       backdev->fds->f_path.dentry->d_name.name,
	       (unsigned long long)pos, (unsigned long long)len);
	if (bw >= 0)
		bw = -EIO;
	return bw;
}

/**
 * tier_file_read - helper for reading data
 */
static int tier_file_read(struct tier_device *dev, unsigned int device,
			  void *buf, const int len, loff_t pos)
{
	struct backing_device *backdev = dev->backdev[device];
	struct file *file = backdev->fds;
	ssize_t bw;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,14,0)
	mm_segment_t old_fs = get_fs();
#endif

	set_debug_info(dev, VFSREAD);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,14,0)
	bw = kernel_read(file, buf, len, &pos);
#else
	set_fs(get_ds());
	bw = vfs_read(file, buf, len, &pos);
	set_fs(old_fs);
#endif
	clear_debug_info(dev, VFSREAD);
	if (likely(bw == len))
		return 0;
	pr_err("Read error at byte offset %llu, length %i.\n",
	       (unsigned long long)pos, len);
	if (bw >= 0)
		bw = -EIO;
	return bw;
}

static int sync_device(struct tier_device *dev, int device)
{
	int ret = 0;
	struct backing_device *backdev = dev->backdev[device];
	if (backdev->dirty) {
		ret = vfs_fsync(backdev->fds, 0);
		if (ret != 0)
			pr_err("sync_device failed for device %u\n", device);
		else
			backdev->dirty = 0;
	}

	return ret;
}

int tier_sync(struct tier_device *dev)
{
	int res;
	int ret = 0;
	int i;
	set_debug_info(dev, PRESYNC);
	for (i = 0; i < dev->attached_devices; i++) {
		res = sync_device(dev, i);
		if (res != 0)
			ret = res;
	}
	clear_debug_info(dev, PRESYNC);
	return ret;
}

void *as_sprintf(const char *fmt, ...)
{
	/* Guess we need no more than 100 bytes. */
	int n, size = 100;
	void *p;
	va_list ap;
	p = kmalloc(size, GFP_ATOMIC);
	if (!p) {
		pr_err("as_sprintf : alloc failed\n");
		return NULL;
	}
	while (1) {
		/* Try to print in the allocated space. */
		va_start(ap, fmt);
		n = vsnprintf(p, size, fmt, ap);
		va_end(ap);
		/* If that worked, return the string. */
		if (n > -1 && n < size)
			return p;
		/* Else try again with more space. */
		if (n > -1)	   /* glibc 2.1 */
			size = n + 1; /* precisely what is needed */
		else		      /* glibc 2.0 */
			size *= 2;    /* twice the old size */
		p = krealloc(p, size, GFP_ATOMIC);
	}
}

void tiererror(struct tier_device *dev, char *msg)
{
	dev->inerror = 1;
	pr_crit("tiererror : %s\n", msg);
}

/* if a physical_blockinfo has same content as blockinfo */
static bool same_blockinfo(struct physical_blockinfo *phy_binfo,
			   struct blockinfo *binfo)
{
	if (phy_binfo->device != binfo->device)
		return false;
	if (phy_binfo->offset != binfo->offset)
		return false;

	return true;
}

/* copy blockinfo to physical_blockinfo */
static void copy_blockinfo(struct physical_blockinfo *phy_binfo,
			   struct blockinfo *binfo)
{
	phy_binfo->device = binfo->device;
	phy_binfo->offset = binfo->offset;
}

/* copy physical_blockinfo to blockinfo  */
static void copy_physical_blockinfo(struct blockinfo *binfo,
				    struct physical_blockinfo *phy_binfo)
{
	binfo->device = phy_binfo->device;
	binfo->offset = phy_binfo->offset;
	atomic64_set(&binfo->hits_ts, 0);
}

/* Delayed metadata update routine */
static void update_blocklist(struct tier_device *dev, u64 blocknr,
			     struct blockinfo *binfo)
{
	struct physical_blockinfo phy_binfo;
	int res;

	if (dev->inerror)
		return;

	res = tier_file_read(dev, 0, &phy_binfo, sizeof(phy_binfo),
			     dev->backdev[0]->startofblocklist +
				 (blocknr * sizeof(phy_binfo)));
	if (res != 0)
		tiererror(dev, "tier_file_read : returned an error");

	if (!same_blockinfo(&phy_binfo, binfo)) {
		(void)write_blocklist(dev, blocknr, binfo, WD);
	}

}

/* When write_blocklist is called with write_policy set to
 * WD(isk) the data is written to disk without updating the cache
 * WC(ache) only updates the cache. This is used for statistics only
 * since this data is not critical.
 * WA(ll) writes to all, cache and disk.
 */
int write_blocklist(struct tier_device *dev, u64 blocknr,
		    struct blockinfo *binfo, int write_policy)
{
	int ret = 0;
	struct backing_device *backdev = dev->backdev[0];

	if (write_policy != WD) {
		/*
		 * Only update blocklist if this is actually a
		 * separate copy of binfo.
		 */
		if (binfo != backdev->blocklist[blocknr])
			memcpy(backdev->blocklist[blocknr], binfo,
			       sizeof(struct blockinfo));
	}

	if (write_policy != WC) {
		u64 blocklist_offset;
		struct physical_blockinfo phy_binfo;

		blocklist_offset = backdev->startofblocklist +
		    (blocknr * sizeof(struct physical_blockinfo));
		copy_blockinfo(&phy_binfo, binfo);

		ret = tier_file_write(dev, 0, &phy_binfo, sizeof(phy_binfo),
				      blocklist_offset);
		if (ret != 0) {
			pr_crit("write_blocklist failed to write blockinfo\n");
			return ret;
		}
		ret = vfs_fsync_range(backdev->fds, blocklist_offset,
				      blocklist_offset + sizeof(phy_binfo),
				      FSMODE);
	}

	return ret;
}

static void write_blocklist_journal(struct tier_device *dev, u64 blocknr,
				    struct blockinfo *newdevice,
				    struct blockinfo *olddevice)
{
	int device = olddevice->device - 1;
	struct backing_device *backdev = dev->backdev[device];
	struct devicemagic *olddev_magic = backdev->devmagic;
	
	down_write(&backdev->magic_lock);
	copy_blockinfo(&olddev_magic->binfo_journal_old, olddevice);
	copy_blockinfo(&olddev_magic->binfo_journal_new, newdevice);
	olddev_magic->blocknr_journal = blocknr;
	up_write(&backdev->magic_lock);
	write_device_magic(dev, device);
}

static void clean_blocklist_journal(struct tier_device *dev, int device)
{
	struct backing_device *backdev = dev->backdev[device];
	struct devicemagic *devmagic = backdev->devmagic;

	down_write(&backdev->magic_lock);
	memset(&devmagic->binfo_journal_old, 0, sizeof(struct physical_blockinfo));
	memset(&devmagic->binfo_journal_new, 0, sizeof(struct physical_blockinfo));
	devmagic->clean = CLEAN;
	devmagic->blocknr_journal = 0;
	up_write(&backdev->magic_lock);
	write_device_magic(dev, device);
}

static void recover_journal(struct tier_device *dev, int device)
{
	u64 blocknr;
	struct backing_device *backdev = dev->backdev[device];
	struct devicemagic *devmagic = backdev->devmagic;
	struct blockinfo binfo;

	if (0 == devmagic->binfo_journal_old.device) {
		pr_info(
		    "recover_journal : journal is clean, no need to recover\n");
		return;
	}

	blocknr = devmagic->blocknr_journal;
	copy_physical_blockinfo(&binfo, &devmagic->binfo_journal_old);
	write_blocklist(dev, blocknr, &binfo, WD);

	if (0 != devmagic->binfo_journal_new.device) {
		copy_physical_blockinfo(&binfo, &devmagic->binfo_journal_new);
		clear_dev_list(dev, &binfo);
	}
	clean_blocklist_journal(dev, device);

	pr_info(
	    "recover_journal : recovered pending migration of blocknr %llu\n",
	    blocknr);
}

sector_t sector_divide(u64 size, u32 sector_size)
{
	u32 bit_shift = 0;
	u32 s = sector_size;
	do {
		bit_shift++;
		s >>= 1;
	} while (s);
	bit_shift--;
	return size >> bit_shift;
}

void discard_on_real_device(struct tier_device *dev, struct blockinfo *binfo)
{
	struct block_device *bdev;
	sector_t sector, nr_sects, endsector;
	u64 endoffset;
	unsigned int sector_size;
	unsigned long flags = 0;
	struct request_queue *dq;
	struct backing_device *backdev = dev->backdev[binfo->device - 1];
	int ret;

	bdev = backdev->bdev;
	if (!bdev) {
		pr_debug("No bdev for device %u\n", binfo->device - 1);
		return;
	}

	if (!dev->discard_to_devices || !dev->discard)
		return;

	/*
	 * Check if this device supports discard
	 * return when it does not
	*/
	dq = bdev_get_queue(bdev);
	if (!blk_queue_discard(dq))
		return;

	sector_size = bdev_logical_block_size(bdev);
	sector = sector_divide(binfo->offset, sector_size);
	if (sector * sector_size < binfo->offset)
	    sector++;
	endoffset = binfo->offset + BLKSIZE;
	endsector = sector_divide(endoffset, sector_size);
	if (endsector <= sector)
		return;
	nr_sects = endsector - sector;

	ret = blkdev_issue_discard(bdev, sector, nr_sects, GFP_NOFS,
				   flags);
	if (0 == ret)
		pr_debug("discarded : device %s : sector %llu, nrsects "
			 "%llu, sectorsize %u\n",
			 backdev->devmagic->fullpathname,
			 (unsigned long long)sector,
			 (unsigned long long)nr_sects, sector_size);
}

/* When a block is migrated to a different tier
 * the readcount and writecount are reset to 0.
 * The block now has hit_collecttime seconds to
 * collect enough hits. After which it is compared
 * to the average hits that blocks have had on this
 * device. Should the block score less then average
 * hits - hysteresis then it will be migrated to an
 * even lower tier.

 * Although reads and writes are counted seperately
 * for now they are threated equally.

 * We can in the future differentiate between SLC
 * and MLC SSD's and store chunks with high read and
 * low write frequency on MLC SSD. And chunks that
 * are often re-written on SLC SSD.

 * Return : 0 on success, < 0 on error
 */
static int copyblock(struct tier_device *dev, struct blockinfo *newdevice,
		     struct blockinfo *olddevice, u64 curblock)
{
	int devicenr = newdevice->device - 1;
	int res = 0;

	if (newdevice->device == olddevice->device) {
		pr_err("copyblock : refuse to migrate block to current device "
		       "%u -> %u\n",
		       newdevice->device - 1, olddevice->device - 1);
		return -EEXIST;
	}

	newdevice->device = 0;
	allocate_dev(dev, curblock, newdevice, devicenr);

	/* No space on the device to copy to is not an error */
	if (0 == newdevice->device) {
		if (dev->migrate_verbose)
			pr_info("copyblock : blocknr %llu no free blocks left on device %d", curblock, devicenr);
		return -ENOSPC;
	}

	/* the actual data moving */
	res = tier_moving_block(dev, olddevice, newdevice);
	if (res != 0) {
		pr_err("copyblock : read/write failed, cancelling operation\n");
		return res;
	}

	write_blocklist_journal(dev, curblock, newdevice, olddevice);
	write_blocklist(dev, curblock, newdevice, WA);
	sync_device(dev, newdevice->device - 1);
	clean_blocklist_journal(dev, olddevice->device - 1);

	if (dev->migrate_verbose)
		pr_info("migrated blocknr %llu from device %u-%llu to device "
			"%u-%llu\n",
			curblock, olddevice->device - 1, olddevice->offset,
			newdevice->device - 1, newdevice->offset);
	return 0;
}

int migrate_direct(struct tier_device *dev, u64 blocknr, int device)
{
	if (NORMAL_IO == atomic_read(&dev->wqlock))
		return -EAGAIN;
	if (0 == atomic_add_unless(&dev->mgdirect.direct, 1, 1))
		return -EAGAIN;
	dev->mgdirect.blocknr = blocknr;
	dev->mgdirect.newdevice = device;
	wake_up(&dev->migrate_event);
	return 0;
}

static int load_bitlists(struct tier_device *dev)
{
	int device;
	u64 cur;
	struct backing_device *backdev;
	int res = 0;

	for (device = 0; device < dev->attached_devices; device++) {
		backdev = dev->backdev[device];
		backdev->bitlist = vzalloc(backdev->bitlistsize);
		if (!backdev->bitlist) {
			pr_info("Failed to allocate memory to load bitlist %u "
				"in memory\n",
				device);
			res = -ENOMEM;
			break;
		}
		for (cur = 0; cur < backdev->bitlistsize; cur += PAGE_SIZE) {
			tier_file_read(dev, device, &backdev->bitlist[cur],
				       PAGE_SIZE,
				       backdev->startofbitlist + cur);
		}
	}
	return res;
}

static void free_bitlists(struct tier_device *dev)
{
	int device;
	//FREE free_offset_list;
	for (device = 0; device < dev->attached_devices; device++) {
		pr_info("free_bitlists on %s", dev->backdev[device]->fds->f_path.dentry->d_name.name);
		if (dev->backdev[device]->bitlist) {
			vfree(dev->backdev[device]->bitlist);
			dev->backdev[device]->bitlist = NULL;
		}
	}
}

static int load_blocklist(struct tier_device *dev)
{
	int alloc_failed = 0;
	u64 curblock;
	u64 blocks = dev->size >> BLK_SHIFT;
	u64 listentries =
	    btier_div(dev->blocklistsize, sizeof(struct physical_blockinfo));
	struct backing_device *backdev = dev->backdev[0];
	int res = 0;
	struct physical_blockinfo phy_binfo;
	struct blockinfo *binfo;

	pr_info("listentries %llu valloc %llu\n", listentries,
		sizeof(struct blockinfo *) * listentries);
	backdev->blocklist = vzalloc(sizeof(struct blockinfo *) * listentries);
	if (!backdev->blocklist)
		return -ENOMEM;

	for (curblock = 0; curblock < blocks; curblock++) {
		binfo = kzalloc(sizeof(struct blockinfo), GFP_KERNEL);
		if (!binfo) {
			alloc_failed = 1;
			break;
		}

		backdev->blocklist[curblock] = binfo;

		res = tier_file_read(dev, 0, &phy_binfo, sizeof(phy_binfo),
				     backdev->startofblocklist +
					 (curblock * sizeof(phy_binfo)));
		if (res != 0)
			tiererror(dev, "tier_file_read : returned an error");

		copy_physical_blockinfo(binfo, &phy_binfo);
	}

	if (alloc_failed) {
		res = -ENOMEM;
		free_blocklist(dev);
	}

	return res;
}


/* */
static void flush_blocklist(struct tier_device *dev)
{
	struct physical_blockinfo *phy_binfo;
	u64 blocks = dev->size >> BLK_SHIFT;
	u64 cur = 0;
	int res;
	u64 listentries = btier_div(dev->blocklistsize, sizeof(struct physical_blockinfo));
	size_t size = sizeof(struct physical_blockinfo) * listentries;
	u64 diff_size = 0;

	ENTER_FUNC;
	pr_info("blocks: %llu listentries: %llu size: %zu", blocks, listentries, size);

	phy_binfo = vzalloc(size);
	if (!phy_binfo) {
		pr_info("flush_blocklist: cannot allocate memory for phy_binfo");
		EXIT_FUNC;
		return;
	}

	if (dev->inerror) {
		EXIT_FUNC;
		return;
	}

	res = tier_file_read(dev, 0, phy_binfo, size, dev->backdev[0]->startofblocklist);
	if (res != 0) {
		pr_crit("tier_file_read : returned an error");
		EXIT_FUNC;
		return;
	}

	for ( cur = 0; cur < blocks; cur++) {
		if ( !same_blockinfo(phy_binfo + cur, dev->backdev[0]->blocklist[cur]) ) {
			diff_size += 1;
			copy_blockinfo(phy_binfo + cur, dev->backdev[0]->blocklist[cur]);
		}
	}

	res = tier_file_write(dev, 0, phy_binfo, size, dev->backdev[0]->startofblocklist);
	if (res != 0) {
		pr_crit("flush_blocklist: failed to write blockinfo\n");
		EXIT_FUNC;
		return;
	}

	res = vfs_fsync_range(dev->backdev[0]->fds, dev->backdev[0]->startofblocklist, size, FSMODE);
	if (res != 0) {
		pr_crit("flush_blocklist: failed to vfs_fsync_range blockinfo\n");
		EXIT_FUNC;
		return;
	}

	vfree(phy_binfo);
	pr_info("blocks: %llu listentries: %llu size: %zu diff_size: %llu", blocks, listentries, size, diff_size);;
	EXIT_FUNC;
}

static void free_blocklist(struct tier_device *dev)
{
	u64 blocks = dev->size >> BLK_SHIFT;

	struct backing_device *backdev = dev->backdev[0];
	if (!backdev->blocklist)
		return;
	pr_info("free_blocklist blocks count: %llu", blocks);

	flush_blocklist(dev);

	vfree(backdev->blocklist);
	backdev->blocklist = NULL;
}

static int need_migrate_down( struct backing_device *backdev ) {
	int keep_free = atomic_read(&backdev->devmagic->dtapolicy.keep_free);
	u64 devblocks = backdev->devicesize >> BLK_SHIFT;
	u64 allocated = atomic64_read(&backdev->allocated_blocks);
	return (devblocks - allocated) * 100 < devblocks * keep_free;
}

static int __move_block(struct tier_device *dev, struct blockinfo *newbinfo, struct blockinfo *orgbinfo, u64 curblock) {
	int res = 0;

	if ( newbinfo->device == 0 || newbinfo->device > dev->attached_devices ) {
		pr_warn("__move_block: newbinfo->device: %d == 0 or > %d", newbinfo->device, dev->attached_devices);
		return res;
	}

	if (orgbinfo->device != newbinfo->device) {
		res = copyblock(dev, newbinfo, orgbinfo, curblock);
		if (res == 0) {
			clear_dev_list(dev, orgbinfo);
			discard_on_real_device(dev, orgbinfo);
		} else {
			/* copyblock failed, restore the old settings */
			memcpy(newbinfo, orgbinfo, sizeof(*orgbinfo));
		}
	}

	return res;
}

static int migrate_block_up(struct tier_device *dev, struct blockinfo *binfo, u64 curblock) {
	int res = 0;
	int device, fromdev;
	struct blockinfo orgbinfo;
	u64 hits_ts;

	if (binfo->device <= 1) /* already on tier0 */
		return res;

	hits_ts = atomic64_read(&binfo->hits_ts);
	if (dev->migrate_verbose)
		pr_info("migrate_block_up: blocknr: %llu, block hits_ts: %llu, migration_total_hits: %llu", curblock, hits_ts, dev->migration_total_hits);

	memcpy(&orgbinfo, binfo, sizeof(*binfo));
	if (1){
		binfo->device--;
		res = __move_block(dev, binfo, &orgbinfo, curblock);
	} else {
		fromdev = binfo->device;
		for ( device = 1; device < fromdev; device ++) {
			if (dev->migrate_verbose)
				pr_info("migrate_block_up: blocknr: %llu trying %d -> %d", curblock, binfo->device, device);
			binfo->device = device;
			res = __move_block(dev, binfo, &orgbinfo, curblock);
			if (orgbinfo.device != binfo->device) {
				break;
			}
		}
	}


	return res;
}

static int migrate_block_down(struct tier_device *dev, struct blockinfo *binfo, u64 curblock) {
	int res = 0;
	struct blockinfo orgbinfo;
	u64 hits_ts;

	if (binfo->device == 0)
		return res;
	
	hits_ts = atomic64_read(&binfo->hits_ts);
	if (dev->migrate_verbose)
		pr_info("migrate_block_down: blocknr: %llu, block hits_ts: %llu, migration_total_hits: %llu", curblock, hits_ts, dev->migration_total_hits);

	memcpy(&orgbinfo, binfo, sizeof(*binfo));

	if (binfo->device + 1 <= dev->attached_devices) {
		binfo->device++;
	}
	if (binfo->device > dev->attached_devices) {
		binfo->device = orgbinfo.device;
	}

	res = __move_block(dev, binfo, &orgbinfo, curblock);

	return res;
}

static int migrate_block(struct tier_device *dev, struct blockinfo *binfo, u64 curblock) {
	int res = 0;
	struct backing_device *backdev;
	u64 hits_ts;

	if (!binfo) {
		pr_warn("migrate_block: blocknr: %llu binfo is NULL", curblock);
		return res;
	}

	hits_ts = atomic64_read(&binfo->hits_ts);
	backdev = dev->backdev[binfo->device - 1];

	if ( hits_ts > dev->migration_total_hits && binfo->device > 1 ) { //migrate up candidate
		res = migrate_block_up(dev, binfo, curblock);
	} else if ( need_migrate_down( backdev ) && binfo->device < dev->attached_devices ) { //migrate down candidate
		res = migrate_block_down(dev, binfo, curblock);
	}
	update_blocklist(dev, curblock, binfo);
	return res;
}

static void walk_blocklist(struct tier_device *dev)
{
	u64 blocks = dev->size >> BLK_SHIFT;
	u64 curblock;
	struct blockinfo *binfo;
	int interrupted = 0;
	int res = 0;
	struct data_policy *dtapolicy = &dev->backdev[0]->devmagic->dtapolicy;
	time_t timeout = atomic_read(&dtapolicy->migration_enabled);
	time_t started_at = get_seconds();

	btier_lock(dev);
	
	if (dev->migrate_verbose)
		pr_info("walk_blocklist start from : %llu migration_total_hits : %llu\n", dev->resumeblockwalk, dev->migration_total_hits);

	for (curblock = dev->resumeblockwalk; curblock < blocks; curblock++) {
		if (get_seconds() >= started_at + timeout) {
			dev->resumeblockwalk = curblock;
			interrupted = 1;
			if (dev->migrate_verbose)
				pr_info("walk_block_list interrupted by timeout %lus", timeout);
			break;
		}
		if (dev->stop || !atomic_read(&dtapolicy->migration_enabled) ||
		    dev->inerror) {
			pr_info("walk_block_list ends on stop or disabled\n");
			break;
		}
		binfo = get_blockinfo(dev, curblock, 0);
		if (dev->inerror) {
			pr_err("walk_block_list stops, device is inerror\n");
			break;
		}


		if (binfo->device != 0) {
			res = migrate_block(dev, binfo, curblock);
		}

		if (NORMAL_IO == atomic_read(&dev->wqlock)) { // stop walk_block_list if normal_io
			dev->resumeblockwalk = curblock;
			interrupted = 1;
			if (dev->migrate_verbose)
				 pr_info("walk_block_list interrupted by normal io\n");
            break;
		}
	}
	if (dev->inerror) {
		btier_unlock(dev);
		return;
	}
	tier_sync(dev);
	if (!interrupted) {
		dev->resumeblockwalk = 0;
		dev->migration_total_hits = atomic64_read(&dev->total_hits);
		dev->migrate_timer.expires =
		    jiffies +
		    msecs_to_jiffies(atomic64_read(&dtapolicy->migration_interval) * 1000);
	} else {
		dev->migrate_timer.expires = jiffies + msecs_to_jiffies(3000);
	}
	if (!dev->stop && atomic_read(&dtapolicy->migration_enabled)) {
		if (!timer_pending(&dev->migrate_timer))
			add_timer(&dev->migrate_timer);
		else
			mod_timer(&dev->migrate_timer,
				  dev->migrate_timer.expires);
	}

	btier_unlock(dev);
}

void do_migrate_direct(struct tier_device *dev)
{
	struct blockinfo orgbinfo;
	struct backing_device *backdev0 = dev->backdev[0];
	struct data_policy *dtapolicy = &backdev0->devmagic->dtapolicy;
	u64 blocknr = dev->mgdirect.blocknr;
	int newdevice = dev->mgdirect.newdevice;
	int res;
	struct blockinfo *binfo;

	btier_lock(dev);
	if (atomic_read(&dtapolicy->migration_enabled)) {
		atomic_set(&dtapolicy->migration_enabled, 0);
		if (timer_pending(&dev->migrate_timer))
		    del_timer_sync(&dev->migrate_timer);
		pr_info("migration is disabled for %s due to user controlled data migration\n", dev->devname);
	}

	if (dev->migrate_verbose)
		pr_info("sysfs request migrate blocknr %llu to device %u\n",
		        blocknr, newdevice);
	binfo = get_blockinfo(dev, blocknr, 0);
	if (!binfo)
		goto end_error;

	/* can't migrate unallocated block */
	if (binfo->device == 0)
		goto end_error;

	if (binfo->device - 1 == newdevice) {
		res = -EEXIST;
		pr_err("do_migrate_direct : failed to migrate blocknr %llu, "
		       "already on device %u\n", blocknr, newdevice);
		goto end_error;
	}

	memcpy(&orgbinfo, binfo, sizeof(*binfo));
	binfo->device = newdevice + 1;

	res = copyblock(dev, binfo, &orgbinfo, blocknr);
	if (res == 0) {
		clear_dev_list(dev, &orgbinfo);
		discard_on_real_device(dev, &orgbinfo);
	} else {
		/* copyblock failed, restore the old settings */
		memcpy(binfo, &orgbinfo, sizeof(orgbinfo));
		pr_err("do_migrate_direct : failed to migrate blocknr %llu "
		       "from device %u to device %u: %d\n",
		       blocknr, orgbinfo.device - 1, newdevice, res);
	}
end_error:
	btier_unlock(dev);
}

static void data_migrator(struct work_struct *work)
{
	struct tier_device *dev = ((struct tier_work *)work)->device;
	struct backing_device *backdev0 = dev->backdev[0];
	struct data_policy *dtapolicy = &backdev0->devmagic->dtapolicy;
	
	ENTER_FUNC;
	while (!dev->stop) {
		wait_event_interruptible(
		    dev->migrate_event,
		    MIGRATION_TIMER_EXPIRED == atomic_read(&dev->migrate) || dev->stop ||
			1 == atomic_read(&dev->mgdirect.direct));
		if (dev->migrate_verbose)
			pr_info("data_migrator woke up\n");
		if (dev->stop)
			break;

		if (1 == atomic_read(&dev->mgdirect.direct)) {
			if (dev->migrate_verbose)
				pr_info("do_migrate_direct\n");
			do_migrate_direct(dev);
			atomic_set(&dev->mgdirect.direct, 0);
			continue;
		}

		if (NORMAL_IO == atomic_read(&dev->wqlock)) {
			if (dev->migrate_verbose)
				pr_info("NORMAL_IO pending: backoff\n");
			dev->migrate_timer.expires =
			    jiffies + msecs_to_jiffies(300);
			if (!dev->stop && atomic_read(&dtapolicy->migration_enabled)) {
				mod_timer(&dev->migrate_timer, dev->migrate_timer.expires);
			}

			atomic_set(&dev->migrate, NO_MIGRATION);
			continue;
		}

		walk_blocklist(dev);
		if (dev->migrate_verbose)
			pr_info("data_migrator goes back to sleep\n");
	}
	kfree(work);
	pr_info("data_migrator halted\n");
	EXIT_FUNC;
}

static int init_devicenames(void)
{
	int i;
	/* Allow max 26 devices to be configured */
	devicenames = kmalloc(sizeof(char) * BTIER_MAX_DEVS, GFP_KERNEL);
	if (!devicenames) {
		pr_err("init_devicenames : alloc failed\n");
		return -ENOMEM;
	}
	for (i = 0; i < BTIER_MAX_DEVS; i++) {
		/* sdtiera/b/c/d../z */
		devicenames[i] = 'a' + i;
	}
	return 0;
}

static void release_devicename(char *devicename)
{
	int pos;
	char d;

	if (!devicename)
		return;
	d = devicename[6]; /*sdtierN */
			   /* Restore the char in devicenames */
	pos = d - 'a';
	devicenames[pos] = d;
	kfree(devicename);
}

static char *reserve_devicename(unsigned int *devnr)
{
	char device;
	char *retname;
	int i;
	for (i = 0; i < BTIER_MAX_DEVS; i++) {
		device = devicenames[i];
		if (device != 0)
			break;
	}
	if (0 == device) {
		pr_err("Maximum number of devices exceeded\n");
		return NULL;
	}
	retname = as_sprintf("sdtier%c", device);
	*devnr = i;
	devicenames[i] = 0;
	return retname;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,15,0)
static void migrate_timer_expired(struct timer_list *t)
{
	struct tier_device *dev = from_timer(dev, t, migrate_timer);

	if (0 == atomic_read(&dev->migrate)) {
		atomic_set(&dev->migrate, MIGRATION_TIMER_EXPIRED);
		wake_up(&dev->migrate_event);
	}
}
#else //LINUX_VERSION_CODE >= KERNEL_VERSION(4,15,0)
static void migrate_timer_expired(unsigned long q)
{
	struct tier_device *dev = (struct tier_device *)q;

	if (0 == atomic_read(&dev->migrate)) {
		atomic_set(&dev->migrate, MIGRATION_TIMER_EXPIRED);
		wake_up(&dev->migrate_event);
	}
}
#endif //LINUX_VERSION_CODE >= KERNEL_VERSION(4,15,0)

static void tier_check(struct tier_device *dev, int devicenr)
{
	pr_info("device %s is not clean, check forced\n",
		dev->backdev[devicenr]->fds->f_path.dentry->d_name.name);
	recover_journal(dev, devicenr);
}

/* Zero out the bitlist starting at offset startofbitlist
   with size bitlistsize */
static void wipe_bitlist(struct tier_device *dev, int device,
			 u64 startofbitlist, u64 bitlistsize)
{
	char *buffer;
	u64 offset = 0;

	buffer = kzalloc(PAGE_SIZE, GFP_KERNEL);
	while (offset + PAGE_SIZE < bitlistsize) {
		tier_file_write(dev, device, buffer, PAGE_SIZE,
				startofbitlist + offset);
		offset += PAGE_SIZE;
	}
	if (offset < bitlistsize)
		tier_file_write(dev, device, buffer, bitlistsize - offset,
				startofbitlist + offset);
	kfree(buffer);
}

u64 allocated_on_device(struct tier_device *dev, int device)
{
	u_char *buffer = NULL;
	u64 offset = 0;
	int i;
	u64 allocated = 0;
	int hascache = 0;

	if (dev->backdev[device]->bitlist)
		hascache = 1;
	buffer = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!buffer) {
		tiererror(dev, "allocated_on_device : alloc failed");
		return 0 - 1;
	}
	if (!hascache) {
		while (offset < dev->backdev[device]->bitlistsize) {
			tier_file_read(dev, device, buffer, PAGE_SIZE,
				       dev->backdev[device]->startofbitlist +
					   offset);
			offset += PAGE_SIZE;
			for (i = 0; i < PAGE_SIZE; i++) {
				if (buffer[i] == ALLOCATED)
					allocated += BLKSIZE;
			}
		}
		if (offset < dev->backdev[device]->bitlistsize) {
			pr_info("WTF1??????????");
			tier_file_read(
			    dev, device, buffer,
			    dev->backdev[device]->bitlistsize - offset,
			    dev->backdev[device]->startofbitlist + offset);
		}
	} else {
		while (offset < dev->backdev[device]->bitlistsize) {
			memcpy(buffer, &dev->backdev[device]->bitlist[offset],
			       PAGE_SIZE);
			offset += PAGE_SIZE;
			for (i = 0; i < PAGE_SIZE; i++) {
				if (buffer[i] == ALLOCATED)
					allocated += BLKSIZE;
			}
		}
		if (offset < dev->backdev[device]->bitlistsize) {
			pr_info("WTF2??????????");
			memset(buffer, 0, PAGE_SIZE);
			memcpy(buffer, &dev->backdev[device]->bitlist[offset],
			       dev->backdev[device]->bitlistsize - offset);
		}
	}
	for (i = 0; i < dev->backdev[device]->bitlistsize - offset; i++) {
		if (i >= PAGE_SIZE) {
			pr_err("allocated_on_device : buffer overflow, should "
			       "never happen\n");
			break;
		}
		if (buffer[i] == ALLOCATED)
			allocated += BLKSIZE;
	}
	kfree(buffer);
	return allocated;
}

static void repair_bitlists(struct tier_device *dev)
{
	u64 blocknr;
	struct blockinfo *binfo;
	u64 relative_offset;
	unsigned int i;

	pr_info("repair_bitlists : clearing and rebuilding bitlists\n");
	for (i = 0; i < dev->attached_devices; i++) {
		wipe_bitlist(dev, i, dev->backdev[i]->startofbitlist,
			     dev->backdev[i]->bitlistsize);
	}

	for (blocknr = 0; blocknr<dev->size>> BLK_SHIFT; blocknr++) {
		binfo = get_blockinfo(dev, blocknr, 0);
		if (dev->inerror)
			return;
		if (0 != binfo->device) {
			if (binfo->device > dev->attached_devices) {
				pr_err("repair_bitlists : cleared corrupted "
				       "blocklist entry for blocknr %llu\n",
				       blocknr);
				memset(binfo, 0, sizeof(struct blockinfo));
				continue;
			}
			if (BLKSIZE + binfo->offset >
			    dev->backdev[binfo->device - 1]->devicesize) {
				pr_err("repair_bitlists : cleared corrupted "
				       "blocklist entry for blocknr %llu\n",
				       blocknr);
				memset(binfo, 0, sizeof(struct blockinfo));
				continue;
			}
			relative_offset =
			    binfo->offset -
			    dev->backdev[binfo->device - 1]->startofdata;
			mark_offset_as_used(dev, binfo->device - 1,
					    relative_offset);
		}
	}
}

static void btier_uuid(char *buf, struct tier_device *dev)
{
	unsigned char xbuf[UUID_LEN / 2];
	int i, n, len;
	const char *name;
	u32 hash = 5381;

	/* djb2 hash */
	for (i = 0; i < dev->attached_devices; i++) {
		name = dev->backdev[i]->fds->f_path.dentry->d_name.name;
		len = strlen(name);
		for (n = 0; n < len; n++) {
			hash = hash * 33 ^ name[n];
		}
	}
	prandom_seed(hash);
	prandom_bytes(xbuf, UUID_LEN / 2);

	for (n = 0; n < UUID_LEN / 2; n++) {
		sprintf(buf + (n * 2), "%02X", xbuf[n]);
	}
}

//##!!
static int check_dev_already_exists(struct tier_device *dev) {
	struct tier_device *tier;
	pr_info("check_dev_already_exists: for uuid: %s", dev->backdev[0]->devmagic->uuid);
	list_for_each_entry(tier, &device_list, list) {
		if ( tier == dev ) continue;
		pr_info("check_dev_already_exists: dev: %s uuid: %s", tier->devname, tier->backdev[0]->devmagic->uuid);
		if (memcmp(tier->backdev[0]->devmagic->uuid, dev->backdev[0]->devmagic->uuid, UUID_LEN) == 0) {
			pr_warn("tier device with uuid: %s already exists as dev: %s", tier->backdev[0]->devmagic->uuid, tier->devname);
			return -EEXIST;
		}
	}
	return 0;
}

static int order_devices(struct tier_device *dev)
{
	static const char zhash[UUID_LEN];
	char uuid[UUID_LEN + 1];
	int i, err;
	int clean = 1;
	struct data_policy *dtapolicy;
	const char *devicename;
	struct backing_device *backdev;
	struct backing_device *backdev_tmp_list[MAX_BACKING_DEV];
	ENTER_FUNC;

	/* Allocate and load */
	for (i = 0; i < dev->attached_devices; i++) {
		backdev = dev->backdev[i];
		read_device_magic(dev, i, backdev->devmagic);
		pr_info("fullpathname: %s", backdev->devmagic->fullpathname);
		init_rwsem(&backdev->magic_lock);
		spin_lock_init(&backdev->dev_alloc_lock);
	}

	/* Check and swap */
	for (i = 0; i < dev->attached_devices; i++) {
		backdev_tmp_list[i] = dev->backdev[i];
	}

	for (i = 0; i < dev->attached_devices; i++) {
		dev->backdev[backdev_tmp_list[i]->devmagic->device] = backdev_tmp_list[i];
	}

	/* Generate UUID */
	btier_uuid(uuid, dev);

	// if (0 != memcmp(dev->backdev[0]->devmagic->uuid, zhash, UUID_LEN)) {
	// 	if( 0 != (err = check_dev_already_exists(dev))) {
	// 		return err;
	// 	}
	// }

	/* Mark as inuse */
	for (i = 0; i < dev->attached_devices; i++) {
		backdev = dev->backdev[i];
		if (CLEAN != backdev->devmagic->clean) {
			tier_check(dev, i);
			clean = 0;
		}

		pr_info("zhash: %s", zhash);
		if (0 == memcmp(backdev->devmagic->uuid, zhash, UUID_LEN))
			memcpy(backdev->devmagic->uuid, uuid, UUID_LEN);
		backdev->devmagic->clean = DIRTY;
		write_device_magic(dev, i);
		dtapolicy = &backdev->devmagic->dtapolicy;
		devicename = backdev->fds->f_path.dentry->d_name.name;

		pr_info("device %s tier uuid: %s registered as tier %u\n", devicename, backdev->devmagic->uuid, i);
		if (atomic_read(&dtapolicy->keep_free) > 100 || atomic_read(&dtapolicy->keep_free) < 0)
			atomic_set(&dtapolicy->keep_free, TIERKEEPFREE);
	}

	dtapolicy = &dev->backdev[0]->devmagic->dtapolicy;
	if (atomic_read(&dtapolicy->sequential_landing) >= dev->attached_devices)
		atomic_set(&dtapolicy->sequential_landing, 0);
	if (0 == atomic64_read(&dtapolicy->migration_interval))
		atomic64_set(&dtapolicy->migration_interval, MIGRATE_INTERVAL);

	if (!clean)
		repair_bitlists(dev);

	EXIT_FUNC;
	return 0;
}

static int alloc_moving_bio(struct tier_device *dev)
{
	int bvecs, bv;
	struct bio *bio;
	struct page *page;

	bvecs = BLKSIZE >> PAGE_SHIFT;

	bio = bio_alloc(GFP_NOIO, bvecs);
	if (!bio) {
		tiererror(dev, "bio_alloc failed from alloc_moving_bio\n");
		return -ENOMEM;
	}
	dev->moving_bio = bio;

	for (bv = 0; bv < bvecs; bv++) {
		page = alloc_page(GFP_NOIO);
		if (page == NULL) {
			while (bv > 0) {
				bv--;
				__free_page(bio->bi_io_vec[bv].bv_page);
			}
			tiererror(dev, "alloc_moving_bio: alloc_page failed\n");
			return -ENOMEM;
		}

		bio->bi_io_vec[bv].bv_len = PAGE_SIZE;
		bio->bi_io_vec[bv].bv_offset = 0;
		bio->bi_io_vec[bv].bv_page = page;
	}

	bio_get(bio);

	return 0;
}

static void free_moving_bio(struct tier_device *dev)
{
	int bvecs, bv;
	struct bio *bio = dev->moving_bio;
	struct page *page;

	bvecs = BLKSIZE >> PAGE_SHIFT;

	for (bv = 0; bv < bvecs; bv++) {
		page = bio->bi_io_vec[bv].bv_page;
		if (page)
			__free_page(page);
		bio->bi_io_vec[bv].bv_page = NULL;
	}

	bio_put(bio);
	dev->moving_bio = NULL;
}

static int alloc_blocklock(struct tier_device *dev)
{
	size_t size;
	u64 i, blocks = dev->size >> BLK_SHIFT;

	size = blocks * sizeof(struct rw_semaphore);

	dev->block_lock = vzalloc(size);

	if (!dev->block_lock)
		return -ENOMEM;

	for (i = 0; i < blocks; i++) {
		init_rwsem(dev->block_lock + i);
	}

	return 0;
}

static void free_blocklock(struct tier_device *dev)
{
	if (!dev->block_lock)
		return;

	vfree(dev->block_lock);
	dev->block_lock = NULL;
}


static void init_hits(struct tier_device *dev) {
	struct devicemagic *devmagic;
	unsigned int device;

	for (device = 0; device < dev->attached_devices; device++) {
		devmagic = dev->backdev[device]->devmagic;
		atomic64_set(&devmagic->total_hits, 0);
	}
	dev->migration_total_hits = 0;
	atomic64_set(&dev->total_hits, 0);
}

//!!!!!!!!!!
static int init_free_offset_lists(struct tier_device *dev) {
	int res = 0;
	struct backing_device *backdev;
	size_t size;
	int device;
	u64 cur, i, next;
	u64 first_free, devblocks;
	
	ENTER_FUNC;
	for (device = 0; device < dev->attached_devices; device++) {
		backdev = dev->backdev[device];
		devblocks = (backdev->endofdata - backdev->startofdata) >> BLK_SHIFT;
		size = devblocks * sizeof(backdev->free_offset_list);
		pr_info("init_free_offset_lists d: %d, size: %zu", device, size);

		backdev->free_offset_list = vzalloc(size);
		if (NULL == backdev->free_offset_list) {
			pr_info("Failed to allocate memory for free_offset_list");
			res = -ENOMEM;
			break;
		}

		if (NULL == backdev->bitlist) {
			pr_info("Failed to prepare free_offset_list for backdev %d bitlist is NULL", device);
			res = -ENOMSG;
			break;
		}

		pr_info("init_free_offset_lists d: %d, backdev->bitlistsize: %llu devblocks: %llu", device, backdev->bitlistsize, devblocks);
		
		for ( i = 0; i < devblocks; i++ ) {
			backdev->free_offset_list[i] = i + 1;
		}

		cur = devblocks - 1;
		first_free = FREE_LIST_END;
		atomic64_set(&backdev->allocated_blocks, 0);

		for ( i = 0; i < devblocks; i++ ) {
			if ( FREE_LIST_END == first_free ) {
				if ( ALLOCATED == backdev->bitlist[i] ) {
					backdev->free_offset_list[i] = FREE_LIST_ALLOCATED_MARK;
					atomic64_inc(&backdev->allocated_blocks);
					continue;
				}
				first_free = i;
				cur = first_free;
				continue;
			}

			next = backdev->free_offset_list[cur];
			if ( ALLOCATED == backdev->bitlist[next] ) {
				backdev->free_offset_list[cur]++;
				backdev->free_offset_list[next] = FREE_LIST_ALLOCATED_MARK;
				atomic64_inc(&backdev->allocated_blocks);
			} else {
				cur = backdev->free_offset_list[cur];
			}
			next++;
		}
		backdev->free_offset_list[cur] = FREE_LIST_END;

		pr_info("init_free_offset_lists d: %d, new first_free = %lld", device, first_free);

		backdev->first_free = first_free;
	}
	EXIT_FUNC;
	return res;
}

static void free_free_offset_lists(struct tier_device *dev) {
	struct backing_device *backdev;
	int device;

	ENTER_FUNC;
	for (device = 0; device < dev->attached_devices; device++) {
		backdev = dev->backdev[device];
		if ( NULL != backdev->free_offset_list )
			vfree(backdev->free_offset_list);
	}
	EXIT_FUNC;
}

static int tier_device_register(struct tier_device *dev)
{
	int devnr;
	int ret = 0;
	struct tier_work *migratework;
	struct devicemagic *magic = dev->backdev[0]->devmagic;
	struct data_policy *dtapolicy = &magic->dtapolicy;
	struct request_queue *q;

	if (dev->logical_block_size < MIN_LOGICAL_BLOCK_SIZE || dev->logical_block_size > MAX_LOGICAL_BLOCK_SIZE ||
	    (dev->logical_block_size & (dev->logical_block_size - 1)) != 0) {
		pr_info("tier_device logical_block_size = %u out of range", dev->logical_block_size);
		pr_info("set dev->logical_block_size to minimum value %u", MIN_LOGICAL_BLOCK_SIZE);
		dev->logical_block_size = MIN_LOGICAL_BLOCK_SIZE;
	}
	dev->nsectors = sector_divide(dev->size, dev->logical_block_size);
	dev->size = dev->nsectors * dev->logical_block_size;
	pr_info("tier_device dev->nsectors = %zu", dev->nsectors);
	pr_info("tier_device dev->logical_block_size = %u", dev->logical_block_size);
	if (dev->size > BTIER_MAX_SIZE) {
		kfree(dev);
		pr_err("BTIER max supported device size of 2PB is exceeded %llu > %llu\n", dev->size, BTIER_MAX_SIZE);
		return -ENOMSG;
	}
	dev->active = 1;
	dev->devname = reserve_devicename(&devnr);
	if (!dev->devname)
		return -ENOMEM;

	pr_info("%s size : 0x%llx (%llu)\n", dev->devname, dev->size, dev->size);
	spin_lock_init(&dev->dbg_lock);
	spin_lock_init(&dev->io_seq_lock);

	if (!(dev->bio_task = mempool_create_slab_pool(32, bio_task_cache)) ||
	    !(dev->bio_meta =
		  mempool_create_kmalloc_pool(32, sizeof(struct bio_meta))) ||
	    alloc_blocklock(dev) || alloc_moving_bio(dev) ||
	    !(q = blk_alloc_queue(GFP_KERNEL))) {
		pr_err("Memory allocation failed in tier_device_register \n");
		ret = -ENOMEM;
		goto out;
	}

	ret = load_blocklist(dev);
	if (0 != ret)
		goto out;

	init_hits(dev);

	ret = load_bitlists(dev);
	if (0 != ret)
		goto out;

	ret = init_free_offset_lists(dev);
	if ( 0 != ret)
		goto out;

	init_waitqueue_head(&dev->migrate_event);
	init_waitqueue_head(&dev->aio_event);

	dev->migrate_verbose = 0;
	dev->stop = 0;

	atomic_set(&dev->migrate, NO_MIGRATION);
	atomic_set(&dev->wqlock, NO_IO);
	atomic_set(&dev->aio_pending, 0);
	atomic_set(&dev->mgdirect.direct, 0);
	atomic64_set(&dev->stats.seq_reads, 0);
	atomic64_set(&dev->stats.rand_reads, 0);
	atomic64_set(&dev->stats.seq_writes, 0);
	atomic64_set(&dev->stats.rand_writes, 0);
	init_rwsem(&dev->qlock);

	/* Set queue make_request_fn */
	blk_queue_make_request(q, tier_make_request);
	dev->rqueue = q;
	q->queuedata = (void *)dev;

	/*
	 * Add limits and tell the block layer that we are not a rotational
	 * device and that we support discard aka trim.
	 */
	blk_queue_logical_block_size(q, dev->logical_block_size);
	blk_queue_io_opt(q, BLKSIZE);
	blk_queue_max_discard_sectors(q, dev->size / 512);
	q->limits.max_segments = BIO_MAX_PAGES;
	q->limits.max_hw_sectors =
	    q->limits.max_segment_size * q->limits.max_segments;
	q->limits.max_sectors = q->limits.max_hw_sectors;
	q->limits.discard_granularity = BLKSIZE;
	q->limits.discard_alignment = BLKSIZE;
	set_bit(QUEUE_FLAG_NONROT, &q->queue_flags);
	set_bit(QUEUE_FLAG_DISCARD, &q->queue_flags);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,8,0)
	blk_queue_write_cache(q, true, true);
#else
	blk_queue_flush(q, REQ_FLUSH | REQ_FUA);
#endif

	/*
	 * Get registered.
	 */
	dev->major_num = register_blkdev(0, dev->devname);
	if (dev->major_num <= 0) {
		pr_warning("tier: unable to get major number\n");
		goto out;
	}

	/*
	 * And the gendisk structure.
	 * We support 256 (kernel default) partitions.
	 */
	dev->gd = alloc_disk(DISK_MAX_PARTS);
	if (!dev->gd)
		goto out_unregister;
	dev->gd->major = dev->major_num;
	dev->gd->first_minor = 0;
	dev->gd->fops = &tier_ops;
	dev->gd->private_data = dev;
	strcpy(dev->gd->disk_name, dev->devname);
	set_capacity(dev->gd, dev->nsectors * (dev->logical_block_size >> SECTOR_SHIFT));
	dev->gd->queue = q;

	migratework = kzalloc(sizeof(*migratework), GFP_KERNEL);
	if (!migratework) {
		pr_err("Failed to allocate memory for migratework\n");
		ret = -ENOMEM;
		goto out_unregister;
	}
	migratework->device = dev;
	dev->managername = as_sprintf("%s-manager", dev->devname);
	dev->aioname = as_sprintf("%s-aio", dev->devname);
	dev->migration_wq =
	    alloc_workqueue(dev->managername, WQ_MEM_RECLAIM | WQ_UNBOUND, 1);
	if (!dev->migration_wq) {
		pr_err("Unable to create migration workqueue for %s\n",
		       dev->managername);
		ret = -ENOMEM;
		goto out_unregister;
	}
	INIT_WORK((struct work_struct *)migratework, data_migrator);
	queue_work(dev->migration_wq, (struct work_struct *)migratework);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,15,0)
	timer_setup(&dev->migrate_timer, migrate_timer_expired, 0);
#else
	init_timer(&dev->migrate_timer);
	dev->migrate_timer.data = (unsigned long)dev;
	dev->migrate_timer.function = migrate_timer_expired;
#endif
	dev->migrate_timer.expires =
	    jiffies + msecs_to_jiffies(atomic64_read(&dtapolicy->migration_interval) * 1000);
	add_timer(&dev->migrate_timer);

	add_disk(dev->gd);
	tier_sysfs_init(dev);

	/* let user-space know about the new size */
	kobject_uevent(&disk_to_dev(dev->gd)->kobj, KOBJ_CHANGE);
#ifdef MAX_PERFORMANCE
	pr_info("MAX_PERFORMANCE IS ENABLED, no internal statistics\n");
#endif
	pr_info("write mode = bio, vfs is no longer supported\n");
	return ret;

out_unregister:
	unregister_blkdev(dev->major_num, dev->devname);
out:
	return ret;
}

static int register_new_device_size(struct tier_device *dev, u64 newdevsize)
{
	int ret;

	free_bitlists(dev);
	free_free_offset_lists(dev);
	free_blocklist(dev);
	free_blocklock(dev);

	dev->nsectors = sector_divide(newdevsize, dev->logical_block_size);
	dev->size = dev->nsectors * dev->logical_block_size;
	dev->backdev[0]->devmagic->total_device_size = dev->size;
	write_device_magic(dev, 0);

	ret = alloc_blocklock(dev);
	if (ret != 0) {
		tiererror(dev, "alloc failed for new block_lock");
		return ret;
	}
	ret = load_blocklist(dev);
	if (ret != 0) {
		tiererror(dev, "loading new blocklist failed");
		return ret;
	}
	ret = load_bitlists(dev);
	if (ret != 0) {
		tiererror(dev, "loading new bitlists failed");
		return ret;
	}
	ret = init_free_offset_lists(dev);
	if (ret != 0) {
		tiererror(dev, "init new free_offset_lists failed");
		return ret;
	}

	blk_queue_max_discard_sectors(dev->rqueue, dev->size >> SECTOR_SHIFT);
	set_capacity(dev->gd, dev->size >> SECTOR_SHIFT);
	revalidate_disk(dev->gd);
	/* let user-space know about the new size */
	kobject_uevent(&disk_to_dev(dev->gd)->kobj, KOBJ_CHANGE);

	return ret;
}

static loff_t tier_get_size(struct file *file)
{
	loff_t size;

	// Compute loopsize in bytes
	size = i_size_read(file->f_mapping->host);
	// *
	// * Unfortunately, if we want to do I/O on the device,
	// * the number of 512-byte sectors has to fit into a sector_t.
	// *
	return size  & ~( ( 1 << SECTOR_SHIFT ) - 1 );
}

static int tier_set_fd(struct tier_device *dev, struct fd_s *fds,
		       struct backing_device *backdev)
{
	int error = -EBADF;
	struct file *file = NULL;
	struct block_device *bdev;
	char *fullname;
	struct devicemagic *dmagic = NULL;
	ssize_t bw;
	loff_t pos = 0;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,14,0)
	mm_segment_t old_fs = get_fs();
#endif

	file = fget(fds->fd);
	if (!file)
		return error;

	if (!(file->f_mode & FMODE_WRITE)) {
		error = -EPERM;
		goto end_error;
	}

	dmagic = kzalloc(sizeof(struct devicemagic), GFP_KERNEL);
	if (dmagic == NULL) {
		error = -ENOMEM;
		goto end_error;
	}
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,14,0)
	bw = kernel_read(file, dmagic, sizeof(*dmagic), &pos);
#else
	set_fs(get_ds());
	bw = vfs_read(file, (char *)dmagic, sizeof(*dmagic), &pos);
	set_fs(old_fs);
#endif
	if (dmagic->magic != TIER_DEVICE_BIT_MAGIC) {
		pr_err("device %s has invalid magic\n",
		       file->f_path.dentry->d_name.name);
		error = -EINVAL;
		goto end_error;
	}
	if (dev->attached_devices > 0) {
		if (memcmp(dmagic->uuid, dev->backdev[0]->devmagic->uuid,
		           UUID_LEN) != 0) {
			error = -EINVAL;
			pr_err("device %s UUID does not match %s != %s\n",
			       file->f_path.dentry->d_name.name, dmagic->uuid, dev->backdev[0]->devmagic->uuid);
			goto end_error;
		}
	}
	backdev->devmagic = dmagic;

	fullname = as_sprintf("/dev/%s", file->f_path.dentry->d_name.name);
	if (!fullname) {
		error = -ENOMEM;
		goto end_error;
	}
	// bdev = lookup_bdev(fullname);
	bdev = blkdev_get_by_path(fullname, FMODE_READ|FMODE_WRITE|FMODE_EXCL, dev);
	kfree(fullname);

	if (IS_ERR(bdev)) {
		pr_err("btier 2 no longer supports files as backend err: %ld\n", PTR_ERR(bdev));
		error = -ENOTBLK;
		goto end_error;
	}
	backdev->bdev = bdev;
	backdev->fds = file;
	error = 0;

	if (file->f_flags & O_SYNC) {
		/* Store this persistent on unload */
		file->f_flags ^= O_SYNC;
	}
end_error:
	if (error != 0) {
		if (dmagic != NULL)
			kfree(dmagic);
		fput(file);
	}
	return error;
}

/* Return the requested tier_device, use -1 for last */
static struct tier_device *tier_device_get(int devnr)
{
	struct list_head *pos;
	struct tier_device *ret = NULL;
	int nr = 0;

	list_for_each(pos, &device_list)
	{
		ret = list_entry(pos, struct tier_device, list);
		if (nr == devnr)
			break;
		nr++;
	}
	return ret;
}

static void tier_device_destroy(struct tier_device *dev)
{
	int i;


	pr_info("tier_device_destroy: %s\n", dev->devname);
	list_del(&dev->list);

	if (dev->active) {
		dev->stop = 1;
		dev->active = 0;

		/* wait all current requests to finish */
		if (0 != atomic_read(&dev->aio_pending))
			wait_event(dev->aio_event,
				   0 == atomic_read(&dev->aio_pending));

		wake_up(&dev->migrate_event);
		if (dev->migration_wq)
			destroy_workqueue(dev->migration_wq);

		tier_sysfs_exit(dev);
		del_timer_sync(&dev->migrate_timer);
		del_gendisk(dev->gd);
		put_disk(dev->gd);
		blk_cleanup_queue(dev->rqueue);

		pr_info("deregister device %s\n", dev->devname);
		unregister_blkdev(dev->major_num, dev->devname);

		kfree(dev->managername);
		kfree(dev->aioname);

		pr_info("release_devicename %s\n", dev->devname);
		release_devicename(dev->devname);

		tier_sync(dev);
		free_bitlists(dev);
		free_free_offset_lists(dev);
		free_blocklist(dev);
		free_blocklock(dev);
		free_moving_bio(dev);

		if (dev->bio_task)
			mempool_destroy(dev->bio_task);
		if (dev->bio_meta)
			mempool_destroy(dev->bio_meta);
	}

	pr_info("deattach back devices");
	for (i = 0; i < dev->attached_devices; i++) {
		pr_info("deattaching %s", dev->backdev[i]->fds->f_path.dentry->d_name.name);
		if (dev->stop)
			clean_blocklist_journal(dev, i);
		filp_close(dev->backdev[i]->fds, NULL);
		if (dev->backdev[i]->bdev != NULL)
			bdput(dev->backdev[i]->bdev);
		kfree(dev->backdev[i]->devmagic);

		pr_info("kfree backdev[%d]", i);
		kfree(dev->backdev[i]);
	}
	kfree(dev);
}

static int del_tier_device(char *devicename)
{
	struct tier_device *tier, *next;
	int res = 0;

	list_for_each_entry_safe(tier, next, &device_list, list)
	{
		if (tier->devname) {
			if (strstr(devicename, tier->devname)) {
				if (tier->users > 0)
					res = -EBUSY;
				else
					tier_device_destroy(tier);
			}
		}
	}
	return res;
}

static int determine_device_size(struct tier_device *dev)
{
	int i;
	struct backing_device *backdev;
	struct backing_device *backdev0 = dev->backdev[0];

	dev->size = backdev0->devmagic->total_device_size;
	backdev0->startofblocklist = atomic64_read(&backdev0->devmagic->startofblocklist);
	dev->blocklistsize = atomic64_read(&backdev0->devmagic->blocklistsize);
	pr_info("dev->blocklistsize               : 0x%llx (%llu)\n",
		dev->blocklistsize, dev->blocklistsize);
	backdev0->endofdata = backdev0->startofblocklist - 1;
	for (i = 0; i < dev->attached_devices; i++) {
		backdev = dev->backdev[i];
		backdev->bitlistsize = backdev->devmagic->bitlistsize;
		backdev->startofdata = TIER_HEADERSIZE;
		backdev->startofbitlist = backdev->devmagic->startofbitlist;
		backdev->devicesize = atomic64_read(&backdev->devmagic->devicesize);
		if (i > 0) {
			backdev->endofdata = backdev->startofbitlist - 1;
		}
		pr_info("backdev->devicesize      : 0x%llx (%llu)\n",
			backdev->devicesize, backdev->devicesize);
		pr_info("backdev->startofdata     : 0x%llx\n",
			backdev->startofdata);
		pr_info("backdev->bitlistsize     : 0x%llx\n",
			backdev->bitlistsize);
		pr_info("backdev->startofbitlist  : 0x%llx\n",
			backdev->startofbitlist);
		pr_info("backdev->endofdata       : 0x%llx\n",
			backdev->endofdata);
	}
	pr_info("dev->backdev[0]->startofblocklist: 0x%llx\n",
		backdev0->startofblocklist);
	return 0;
}

static u64 calc_new_devsize(struct tier_device *dev, int cdev, u64 curdevsize)
{
	int i;
	u64 devsize = 0;
	unsigned int header_size = TIER_HEADERSIZE;

	for (i = 0; i < dev->attached_devices; i++) {
		if (cdev == i) {
			devsize +=
			    curdevsize - TIER_DEVICE_PLAYGROUND - header_size;
			continue;
		}
		devsize += dev->backdev[i]->devicesize - TIER_DEVICE_PLAYGROUND;
	}
	return devsize;
}

static u64 new_total_bitlistsize(struct tier_device *dev, int cdev,
				 u64 curbitlistsize)
{
	int i;
	u64 bitlistsize = 0;

	for (i = 0; i < dev->attached_devices; i++) {
		if (cdev == i) {
			bitlistsize += curbitlistsize;
			continue;
		}
		bitlistsize += dev->backdev[i]->bitlistsize;
	}
	return bitlistsize;
}

/* Copy a list from one location to another
   Return : 0 on success -1 on error  */
static int copylist(struct tier_device *dev, int devicenr, u64 ostart,
		    u64 osize, u64 nstart)
{
	int res = 0;
	u64 offset = 0;
	char *buffer;

	pr_info("copylist device %u, ostart 0x%llx (%llu) osize  0x%llx "
		"(%llu), nstart 0x%llx (%llu) end 0x%llx (%llu)\n",
		devicenr, ostart, ostart, osize, osize, nstart, nstart,
		nstart + osize, nstart + osize);
	buffer = kzalloc(PAGE_SIZE, GFP_NOFS);
	if (buffer == NULL) {
		tiererror(dev, "copylist : alloc failed");
		return -1;
	}
	while (offset + PAGE_SIZE < osize) {
		res = tier_file_read(dev, devicenr, buffer, PAGE_SIZE,
		                     ostart + offset);
		if (res < 0)
			break;
		res = tier_file_write(dev, devicenr, buffer, PAGE_SIZE,
				      nstart + offset);
		if (res < 0)
			break;
		offset += PAGE_SIZE;
	}
	if (offset < osize && res == 0) {
		res = tier_file_read(dev, devicenr, buffer, osize - offset,
		                     ostart + offset);
		if (res == 0)
			res = tier_file_write(dev, devicenr, buffer,
					      osize - offset, nstart + offset);
	}
	if (res < 0) {
		pr_info("copylist has failed, not expanding : offset %llu, "
			"ostart %llu, osize %llu nstart %llu, res %d\n",
			offset, ostart, osize, nstart, res);
		res = -1;
	}
	kfree(buffer);
	return res;
}

/* migrate a bitlist from one location to another
   Afterwards changes the structures to point to the new bitlist
   so that the old bitlist location is no longer used
   Return : 0 on success, negative on error */
static int migrate_bitlist(struct tier_device *dev, int devicenr,
			   u64 newstartofbitlist, u64 newbitlistsize)
{
	struct backing_device *backdev = dev->backdev[devicenr];
	int res;

	pr_info("migrate_bitlist : device %u\n", devicenr);
	if (newstartofbitlist + newbitlistsize < backdev->devicesize) {
		pr_info("Device size has not grown enough to expand\n");
		return -1;
	}
	res = copylist(dev, devicenr, backdev->startofbitlist,
		       backdev->bitlistsize, newstartofbitlist);
	if (res != 0)
		return res;

	wipe_bitlist(dev, devicenr, newstartofbitlist + backdev->bitlistsize,
	             newbitlistsize - backdev->bitlistsize);
	// Make sure the new bitlist is synced to disk before
	// we continue
	res = vfs_fsync_range(backdev->fds, newstartofbitlist,
			      newstartofbitlist + newbitlistsize,
			      FSMODE);
	if (res != 0)
		return res;

	backdev->startofbitlist = newstartofbitlist;
	backdev->bitlistsize = newbitlistsize;
	backdev->devmagic->startofbitlist = newstartofbitlist;
	backdev->devmagic->bitlistsize = newbitlistsize;
	return res;
}

/* migrate a blocklist from one location to another
   Return : 0 on success, negative on error */
static int migrate_blocklist(struct tier_device *dev, u64 newstartofblocklist,
			     u64 newblocklistsize)
{
	struct backing_device *backdev0 = dev->backdev[0];
	int res;

	res = copylist(dev, 0, backdev0->startofblocklist, dev->blocklistsize,
	               newstartofblocklist);
	if (res != 0)
		return res;

	wipe_bitlist(dev, 0, newstartofblocklist + dev->blocklistsize,
		     newblocklistsize - dev->blocklistsize);
	res = vfs_fsync_range(backdev0->fds, newstartofblocklist,
			      newstartofblocklist + newblocklistsize,
			      FSMODE);
	if (res != 0)
		return res;

	dev->blocklistsize = newblocklistsize;
	backdev0->startofblocklist = newstartofblocklist;
	backdev0->endofdata = newstartofblocklist - 1;
	atomic64_set(&backdev0->devmagic->blocklistsize, newblocklistsize);
	atomic64_set(&backdev0->devmagic->startofblocklist, newstartofblocklist);
	return res;
}

/* When the blocklist needs to be expanded
   we have to move blocks of data out of the way
   then expand the bitlist and migrate it from it's
   current location to the new location.
   Since the blocklist is growing tier device 0
   will shrink in usable size. Therefore the bitlist
   may shrink as well. However to reduce complexity
   we let it be for now. */
static int migrate_data_if_needed(struct tier_device *dev, u64 startofblocklist,
				  u64 blocklistsize, int changeddevice)
{
	struct blockinfo binfo;
	int res = 0;
	int cbres = 0;
	u64 blocks = dev->size >> BLK_SHIFT;
	u64 curblock;
	struct blockinfo *orgbinfo;

	pr_info("migrate_data_if_needed\n");
	for (curblock = 0; curblock < blocks; curblock++) {
		/* Do not update the blocks metadata */
		orgbinfo = get_blockinfo(dev, curblock, 0);
		if (dev->inerror) {
			res = -EIO;
			break;
		}
		// Migrating blocks from device 0 + 1;
		if (orgbinfo->device != 1) {
			continue;
		}
		cbres = 1;
		pr_info(
		    "migrate_data_if_needed : blocknr %llu from device %u\n",
		    curblock, orgbinfo->device - 1);
		if (orgbinfo->offset >= startofblocklist &&
		    orgbinfo->offset <= startofblocklist + blocklistsize) {
			memcpy(&binfo, orgbinfo, sizeof(struct blockinfo));
			// Move the block to the device that has grown
			binfo.device = changeddevice + 1;
			pr_info("Call copyblock blocknr %llu from device %u to "
				"device %u\n",
				curblock, orgbinfo->device - 1,
				binfo.device - 1);
			cbres = copyblock(dev, &binfo, orgbinfo, curblock);
			if (cbres == 0) {
				clear_dev_list(dev, orgbinfo);
				/* update blocklist from copy of binfo */
				(void)write_blocklist(dev, curblock, &binfo, WC);
			} else {
				pr_err("migrate_data_if_needed : "
				       "failed to migrate blocknr %llu "
				       "from device %u to device %u: %d\n",
				       curblock, orgbinfo->device - 1,
				       binfo.device - 1, cbres);
			}
		}
		if (!cbres) {
			res = -1;
			break;
		}
	}
	pr_info("migrate_data_if_needed return %u\n", res);
	return res;
}

static int do_resize_tier(struct tier_device *dev, int devicenr, u64 newdevsize,
			  u64 newblocklistsize, u64 newbitlistsize)
{
	struct backing_device *backdev = dev->backdev[devicenr];
	struct backing_device *backdev0 = dev->backdev[0];
	int res = 0;
	u64 newstartofblocklist;
	u64 newstartofbitlist;

	pr_info("resize device %s devicenr %u from %llu to %llu\n",
		backdev->fds->f_path.dentry->d_name.name, devicenr,
		backdev->devicesize, newdevsize);
	newstartofbitlist = newdevsize - newbitlistsize;
	res = migrate_bitlist(dev, devicenr, newstartofbitlist, newbitlistsize);
	if (0 != res)
		return res;

	/* We might have moved the device 0 bitlist */
	newstartofblocklist =
	    backdev0->startofbitlist - newblocklistsize;

	/* When device 0 has grown we move the bitlist of the device to
	   the end of the device and then move the blocklist to the end
	   This does not require data migration

	   When another device has grown we may need to expand the blocklist
	   on device 0 as well. In that case we may need to migrate data
	   from device0 to another device to make room for the larger
	   blocklist */
	if (devicenr == 0) {
		res = migrate_blocklist(dev, newstartofblocklist, newblocklistsize);
		if (0 != res)
			return res;
	} else {
		if (newblocklistsize > dev->blocklistsize) {
			res = migrate_data_if_needed(dev, newstartofblocklist,
						     newblocklistsize,
						     devicenr);
			if (0 != res)
				return res;
			// This should be journalled. FIX FIX FIX
			// The blocklist needs to be protected at all cost.
			res = migrate_blocklist(dev, newstartofblocklist,
						newblocklistsize);
			if (0 != res)
				return res;
			write_device_magic(dev, 0);
		} else {
			pr_info("newstartofblocklist %llu, old start %llu, no "
				"migration needed\n", newstartofblocklist,
				backdev0->startofblocklist);
		}

		backdev->endofdata = newstartofbitlist - 1;
	}

	backdev->devicesize = newdevsize;
	atomic64_set(&backdev->devmagic->devicesize, newdevsize);
	write_device_magic(dev, devicenr);
	res = tier_sync(dev);
	return res;
}

void resize_tier(struct tier_device *dev)
{
	int count;
	int res = 1;
	loff_t curdevsize = 0;
	u64 newbitlistsize = 0;
	u64 newblocklistsize = 0;
	u64 newdevsize = 0;
	u64 newbitlistsize_total = 0;
	int found = 0;

	btier_lock(dev);

	pr_info("Start device resizing %s 0x%llx (%llu)\n", dev->devname, dev->size, dev->size);
	for (count = 0; count < dev->attached_devices; count++) {
		curdevsize = tier_get_size(dev->backdev[count]->fds);
		curdevsize = round_to_blksize(curdevsize);
		newbitlistsize = calc_bitlist_size(curdevsize);
		pr_info("device %u, curdevsize = %llu old = %llu\n", count,
		        curdevsize, dev->backdev[count]->devicesize);
		if (dev->backdev[count]->devicesize == curdevsize)
			continue;
		if (curdevsize - dev->backdev[count]->devicesize <
		    newbitlistsize) {
			pr_info("Ignoring unusable small devicesize change for "
				"device %u\n",
				count);
			continue;
		}
		newdevsize = calc_new_devsize(dev, count, curdevsize);
		newbitlistsize_total =
		    new_total_bitlistsize(dev, count, newbitlistsize);
		newblocklistsize =
		    calc_blocklist_size(newdevsize, newbitlistsize_total);
		newdevsize = newdevsize - newblocklistsize - newbitlistsize_total;
		// Make sure there is plenty of space
		if (curdevsize < dev->backdev[count]->devicesize +
				     newblocklistsize + newbitlistsize +
				     BLKSIZE) {
			pr_info("Ignoring unusable small devicesize change for "
				"device %u\n",
				count);
			continue;
		}
		found++;
		pr_info("newblocklistsize=%llu\n", newblocklistsize);
		res = do_resize_tier(dev, count, curdevsize, newblocklistsize,
				     newbitlistsize);
	}
	if (0 == found) {
		pr_info("Ignoring request to resize, no devices have changed "
			"in size\n");
	} else {
		if (res == 0) {
			pr_info("Device %s is resized from %llu to %llu\n",
				dev->devname, dev->size, newdevsize);
			(void)register_new_device_size(dev, newdevsize);
		}
	}
	btier_unlock(dev);
}

static long tier_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct tier_device *dev = NULL;
	struct tier_device *devnew = NULL;
	struct backing_device *backdev;
	int err = 0;
	char *dname;
	int devlen;
	struct fd_s fds;

	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;

	mutex_lock(&ioctl_mutex);
	mutex_lock(&tier_devices_mutex);

	/* Get last tier device */
	dev = tier_device_get(-1);
	if (dev == NULL && cmd != TIER_INIT) {
		err = -ENXIO;
		goto end_error;
	}
	switch (cmd) {
	case TIER_INIT:
		pr_info("tier_ioctl: TIER_INIT");
		/* Check if a device is being set up already */
		if (dev != NULL && dev->tier_device_number == 0)
		    tier_device_destroy(dev);
		err = -ENOMEM;
		devnew = kzalloc(sizeof(struct tier_device), GFP_KERNEL);
		if (devnew == NULL)
			break;
		list_add_tail(&devnew->list, &device_list);
		err = 0;
		break;
	case TIER_DESTROY:
		pr_info("tier_ioctl: TIER_DESTROY");
		if (dev->tier_device_number != 0) {
			err = -EBUSY;
			break;
		}
		tier_device_destroy(dev);
		err = 0;
		break;
	case TIER_SET_FD:
		pr_info("tier_ioctl: TIER_SET_FD");
		err = -EEXIST;
		if (dev->attached_devices > MAX_BACKING_DEV)
			break;
		if (0 != dev->tier_device_number)
			break;
		backdev = kzalloc(sizeof(struct backing_device), GFP_KERNEL);
		if (copy_from_user(&fds, (struct fd_s __user *)arg,
				   sizeof(fds))) {
			err = -EFAULT;
			break;
		}
		err = tier_set_fd(dev, &fds, backdev);
		if (err != 0) {
			kfree(backdev);
			break;
		}
		dev->backdev[dev->attached_devices] = backdev;
		dev->attached_devices++;
		break;
	case TIER_SET_SECTORSIZE:
		pr_info("tier_ioctl: TIER_SET_SECTORSIZE");
		err = -EEXIST;
		if (0 != dev->tier_device_number)
			break;
		err = 0;
		dev->logical_block_size = arg;
		pr_info("sectorsize : %d\n", dev->logical_block_size);
		break;
	case TIER_REGISTER:
		pr_info("tier_ioctl: TIER_REGISTER");
		err = -EEXIST;
		if (0 != dev->tier_device_number)
			break;
		if (0 == dev->attached_devices) {
			pr_err("Insufficient parameters entered");
		} else {
			dev->tier_device_number = tier_device_count();
			if (0 != (err = order_devices(dev)))
				break;

			pr_info("tier device count %u\n", dev->attached_devices);
			if (0 == (err = determine_device_size(dev)))
				err = tier_device_register(dev);
		}

		if (err != 0 || arg == 0)
			break;
		devlen = 1 + strlen(dev->devname);
		if (copy_to_user((char __user *)arg, dev->devname, devlen))
			err = -EFAULT;
		break;
	case TIER_DEREGISTER:
		pr_info("tier_ioctl: TIER_DEREGISTER");
		pr_info("TIER_DEREGISTER\n");
		err = -ENOMEM;
		devlen = 1 + strlen("/dev/sdtierX");
		dname = kzalloc(devlen, GFP_KERNEL);
		if (!dname)
			break;
		if (copy_from_user(dname, (char __user *)arg, devlen - 1)) {
			err = -EFAULT;
		} else {
			err = del_tier_device(dname);
		}
		kfree(dname);
		break;
	default:
		pr_info("tier_ioctl: default");
		err = dev->ioctl ? dev->ioctl(dev, cmd, arg) : -EINVAL;
	}
end_error:
	mutex_unlock(&tier_devices_mutex);
	mutex_unlock(&ioctl_mutex);
	return err;
}

static const struct file_operations _tier_ctl_fops = {.open = nonseekable_open,
						      .unlocked_ioctl =
							  tier_ioctl,
						      .owner = THIS_MODULE,
						      .llseek = noop_llseek};

static struct miscdevice _tier_misc = {.minor = MISC_DYNAMIC_MINOR,
				       .name = "tiercontrol",
				       .nodename = "tiercontrol",
				       .fops = &_tier_ctl_fops};

static int __init tier_init(void)
{
	int r;

	pr_info("btier module init max device size %llub\n", BTIER_MAX_SIZE);

	if (!(btier_wq = alloc_workqueue("kbtier", WQ_MEM_RECLAIM, 0)) ||
	    tier_request_init())
		goto end_nomem;

	/* First register out control device */
	pr_info("version    : %s\n", TIER_VERSION);

	r = misc_register(&_tier_misc);
	if (r) {
		pr_err("misc_register failed for control device");
		goto end_register_err;
	}

	/*
	 * Alloc our device names
	 */
	r = init_devicenames();
	mutex_init(&ioctl_mutex);

	return r;
end_register_err:
	tier_request_exit();
	return r;
end_nomem:
	return -ENOMEM;
}

static void __exit tier_exit(void)
{
	struct tier_device *tier, *next;

	if (btier_wq)
		destroy_workqueue(btier_wq);

	list_for_each_entry_safe(tier, next, &device_list, list)
	    tier_device_destroy(tier);

	misc_deregister(&_tier_misc);

	tier_request_exit();

	kfree(devicenames);
	mutex_destroy(&ioctl_mutex);
}

module_init(tier_init);
module_exit(tier_exit);
