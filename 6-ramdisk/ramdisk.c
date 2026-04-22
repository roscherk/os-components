#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/highmem.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Prokhor Arkhipov");
MODULE_DESCRIPTION("ramdisk - RAM-backed block device with sparse page storage");
MODULE_VERSION("0.1");

#define DEVICE_NAME "ramdisk"

static int disk_size_mb = 64;
module_param(disk_size_mb, int, 0444);
MODULE_PARM_DESC(disk_size_mb, "Disk size in megabytes (default: 64)");

static bool debug;
module_param(debug, bool, 0644);
MODULE_PARM_DESC(debug, "Log each bio and page allocation to dmesg (default: 0, toggleable via sysfs)");

#define ramdisk_dbg(fmt, ...) \
	do { if (debug) pr_info(DEVICE_NAME ": " fmt, ##__VA_ARGS__); } while (0)

static int major;
static struct gendisk *ramdisk;

static unsigned long n_pages;
static struct page **pages;

static void ramdisk_submit_bio(struct bio *bio)
{
	struct bvec_iter iter;
	struct bio_vec bv;
	sector_t sector = bio->bi_iter.bi_sector;

	ramdisk_dbg("bio %s sector=%llu size=%u segments=%u\n",
		    bio_data_dir(bio) == READ ? "READ" : "WRITE",
		    (unsigned long long)sector,
		    bio->bi_iter.bi_size,
		    bio_segments(bio));

	bio_for_each_segment(bv, bio, iter) {
		void *kaddr = kmap_local_page(bv.bv_page);
		char *buf = (char *)kaddr + bv.bv_offset;
		unsigned int remaining = bv.bv_len;

		while (remaining > 0) {
			unsigned long disk_off = (unsigned long)sector * SECTOR_SIZE;
			unsigned long pg_idx = disk_off >> PAGE_SHIFT;
			unsigned int pg_off = disk_off & (PAGE_SIZE - 1);
			unsigned int chunk = min_t(unsigned int, remaining, PAGE_SIZE - pg_off);

			if (WARN_ON_ONCE(pg_idx >= n_pages)) {
				kunmap_local(kaddr);
				bio_io_error(bio);
				return;
			}

			if (bio_data_dir(bio) == READ) {
				if (pages[pg_idx])
					memcpy(buf, page_address(pages[pg_idx]) + pg_off, chunk);
				else
					memset(buf, 0, chunk);
			} else {
				if (!pages[pg_idx]) {
					struct page *new_pg = alloc_page(GFP_NOIO | __GFP_ZERO);
					struct page *prev;

					if (!new_pg) {
						kunmap_local(kaddr);
						bio_io_error(bio);
						return;
					}
					prev = cmpxchg(&pages[pg_idx], NULL, new_pg);
					if (prev)
						__free_page(new_pg);
					else
						ramdisk_dbg("alloc page[%lu]\n", pg_idx);
				}
				memcpy(page_address(pages[pg_idx]) + pg_off, buf, chunk);
			}

			buf += chunk;
			remaining -= chunk;
			sector += chunk / SECTOR_SIZE;
		}

		kunmap_local(kaddr);
	}

	bio_endio(bio);
}

static const struct block_device_operations ramdisk_fops = {
	.owner		= THIS_MODULE,
	.submit_bio	= ramdisk_submit_bio,
};

static int __init ramdisk_init(void)
{
	unsigned long disk_size_bytes;
	int err;

	if (disk_size_mb <= 0) {
		pr_err(DEVICE_NAME ": disk_size_mb must be > 0\n");
		return -EINVAL;
	}

	disk_size_bytes = (unsigned long)disk_size_mb * 1024 * 1024;
	n_pages = DIV_ROUND_UP(disk_size_bytes, PAGE_SIZE);

	pages = kvmalloc_array(n_pages, sizeof(*pages), GFP_KERNEL | __GFP_ZERO);
	if (!pages)
		return -ENOMEM;

	major = register_blkdev(0, DEVICE_NAME);
	if (major < 0) {
		err = major;
		pr_err(DEVICE_NAME ": register_blkdev failed: %d\n", err);
		goto err_pages;
	}

	ramdisk = blk_alloc_disk(NULL, NUMA_NO_NODE);
	if (IS_ERR(ramdisk)) {
		err = PTR_ERR(ramdisk);
		pr_err(DEVICE_NAME ": blk_alloc_disk failed: %d\n", err);
		goto err_blkdev;
	}

	ramdisk->major = major;
	ramdisk->first_minor = 0;
	ramdisk->minors = 1;
	ramdisk->fops = &ramdisk_fops;
	strscpy(ramdisk->disk_name, DEVICE_NAME, DISK_NAME_LEN);
	set_capacity(ramdisk, disk_size_bytes / SECTOR_SIZE);

	err = add_disk(ramdisk);
	if (err) {
		pr_err(DEVICE_NAME ": add_disk failed: %d\n", err);
		goto err_disk;
	}

	pr_info(DEVICE_NAME ": loaded, size=%d MB, pages=%lu\n", disk_size_mb, n_pages);
	return 0;

err_disk:
	put_disk(ramdisk);
err_blkdev:
	unregister_blkdev(major, DEVICE_NAME);
err_pages:
	kvfree(pages);
	return err;
}

static void __exit ramdisk_exit(void)
{
	unsigned long i;

	del_gendisk(ramdisk);
	put_disk(ramdisk);
	unregister_blkdev(major, DEVICE_NAME);

	for (i = 0; i < n_pages; i++) {
		if (pages[i])
			__free_page(pages[i]);
	}
	kvfree(pages);

	pr_info(DEVICE_NAME ": unloaded\n");
}

module_init(ramdisk_init);
module_exit(ramdisk_exit);
