#ifndef _BTIER_MAIN_H_
#define _BTIER_MAIN_H_

static loff_t tier_get_size(struct file *);
static int tier_file_write(struct tier_device *, unsigned int, void *, size_t,
			   loff_t);
static int tier_file_read(struct tier_device *, unsigned int, void *, const int,
			  loff_t);
static void free_blocklist(struct tier_device *);

#endif /* _BTIER_MAIN_H_ */
