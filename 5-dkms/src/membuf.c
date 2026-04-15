#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/rwsem.h>
#include <linux/mutex.h>
#include <linux/version.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Prokhor Arkhipov");
MODULE_DESCRIPTION("membuf - fixed size memory buffers");
MODULE_VERSION("0.1");

#define DEVICE_NAME  "membuf"
#define CLASS_NAME   "membuf_class"

#define MAX_DEVICES     256
#define DEFAULT_BUFSIZE 4096

struct membuf_device {
	char                *buf;
	size_t               size;
	struct rw_semaphore  rwsem;
	struct cdev          cdev;
	struct device       *device;
	atomic_t             open_count;
};

static struct membuf_device *devices[MAX_DEVICES];
static DEFINE_MUTEX(devices_mutex);
static dev_t dev_base;
static struct class *membuf_class;
static int num_devices = 1;
static int default_bufsize = DEFAULT_BUFSIZE;
static int leak_test = 0;
static bool initialized = false;

static int membuf_create_device(int index);
static void membuf_destroy_device(int index);

static int membuf_open(struct inode *inode, struct file *file)
{
	struct membuf_device *dev;

	dev = container_of(inode->i_cdev, struct membuf_device, cdev);
	file->private_data = dev;
	atomic_inc(&dev->open_count);
	return 0;
}

static int membuf_release(struct inode *inode, struct file *file)
{
	struct membuf_device *dev = file->private_data;

	atomic_dec(&dev->open_count);
	return 0;
}

static ssize_t membuf_read(struct file *file, char __user *ubuf,
			    size_t count, loff_t *ppos)
{
	struct membuf_device *dev = file->private_data;
	ssize_t ret;

	down_read(&dev->rwsem);
	if (*ppos >= dev->size) {
		ret = 0;
		goto out;
	}
	if (*ppos + count > dev->size)
		count = dev->size - *ppos;

	if (copy_to_user(ubuf, dev->buf + *ppos, count)) {
		ret = -EFAULT;
		goto out;
	}

	*ppos += count;
	ret = count;
out:
	up_read(&dev->rwsem);
	return ret;
}

static ssize_t membuf_write(struct file *file, const char __user *ubuf,
			     size_t count, loff_t *ppos)
{
	struct membuf_device *dev = file->private_data;
	ssize_t ret;

	down_write(&dev->rwsem);

	if (*ppos >= dev->size) {
		ret = -ENOSPC;
		goto out;
	}
	if (*ppos + count > dev->size)
		count = dev->size - *ppos;

	if (copy_from_user(dev->buf + *ppos, ubuf, count)) {
		ret = -EFAULT;
		goto out;
	}

	*ppos += count;
	ret = count;
out:
	up_write(&dev->rwsem);
	return ret;
}

static loff_t membuf_llseek(struct file *file, loff_t offset, int whence)
{
	struct membuf_device *dev = file->private_data;
	loff_t newpos;

	down_read(&dev->rwsem);

	switch (whence) {
	case SEEK_SET:
		newpos = offset;
		break;
	case SEEK_CUR:
		newpos = file->f_pos + offset;
		break;
	case SEEK_END:
		newpos = dev->size + offset;
		break;
	default:
		newpos = -EINVAL;
		goto out;
	}

	if (newpos < 0 || (size_t)newpos > dev->size) {
		newpos = -EINVAL;
		goto out;
	}

	file->f_pos = newpos;
out:
	up_read(&dev->rwsem);
	return newpos;
}

static const struct file_operations membuf_fops = {
	.owner          = THIS_MODULE,
	.open           = membuf_open,
	.release        = membuf_release,
	.read           = membuf_read,
	.write          = membuf_write,
	.llseek         = membuf_llseek,
};

static ssize_t bufsize_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct membuf_device *mdev = dev_get_drvdata(dev);
	size_t sz;

	down_read(&mdev->rwsem);
	sz = mdev->size;
	up_read(&mdev->rwsem);

	return sprintf(buf, "%zu\n", sz);
}

static ssize_t bufsize_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct membuf_device *mdev = dev_get_drvdata(dev);
	unsigned long new_size;
	char *new_buf;
	size_t copy_len;
	int ret;

	ret = kstrtoul(buf, 10, &new_size);
	if (ret)
		return ret;
	if (new_size == 0)
		return -EINVAL;

	new_buf = kvzalloc(new_size, GFP_KERNEL);
	if (!new_buf)
		return -ENOMEM;

	down_write(&mdev->rwsem);
	copy_len = min(new_size, (unsigned long)mdev->size);
	memcpy(new_buf, mdev->buf, copy_len);
	kvfree(mdev->buf);
	mdev->buf = new_buf;
	mdev->size = new_size;
	up_write(&mdev->rwsem);

	return count;
}

static DEVICE_ATTR_RW(bufsize);

static struct attribute *membuf_dev_attrs[] = {
	&dev_attr_bufsize.attr,
	NULL,
};

ATTRIBUTE_GROUPS(membuf_dev);

static int membuf_create_device(int index)
{
	struct membuf_device *dev;
	int ret;

	dev = kvzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->size = default_bufsize;
	dev->buf = kvzalloc(dev->size, GFP_KERNEL);
	if (!dev->buf) {
		kvfree(dev);
		return -ENOMEM;
	}

	atomic_set(&dev->open_count, 0);
	init_rwsem(&dev->rwsem);
	cdev_init(&dev->cdev, &membuf_fops);
	dev->cdev.owner = THIS_MODULE;
	ret = cdev_add(&dev->cdev, MKDEV(MAJOR(dev_base), index), 1);
	if (ret)
		goto err_free;

	membuf_class->dev_groups = membuf_dev_groups;
	dev->device = device_create(membuf_class, NULL,
				    MKDEV(MAJOR(dev_base), index),
				    dev, DEVICE_NAME "%d", index);

	if (IS_ERR(dev->device)) {
		ret = PTR_ERR(dev->device);
		goto err_cdev;
	}

	devices[index] = dev;
	return 0;

err_cdev:
	cdev_del(&dev->cdev);
err_free:
	kvfree(dev->buf);
	kvfree(dev);
	return ret;
}

static void membuf_destroy_device(int index)
{
	struct membuf_device *dev = devices[index];

	if (!dev)
		return;
	device_destroy(membuf_class, MKDEV(MAJOR(dev_base), index));
	cdev_del(&dev->cdev);
	kvfree(dev->buf);
	kvfree(dev);
	devices[index] = NULL;
}

static int set_num_devices(const char *val, const struct kernel_param *kp)
{
	int new_num, old_num, i, ret;

	ret = kstrtoint(val, 10, &new_num);
	if (ret)
		return ret;
	if (new_num < 0 || new_num > MAX_DEVICES)
		return -EINVAL;

	if (!initialized) {
		num_devices = new_num;
		return 0;
	}

	mutex_lock(&devices_mutex);
	old_num = num_devices;

	if (new_num > old_num) {
		for (i = old_num; i < new_num; i++) {
			ret = membuf_create_device(i);
			if (ret) {
				while (--i >= old_num)
					membuf_destroy_device(i);
				mutex_unlock(&devices_mutex);
				return ret;
			}
		}
	} else if (new_num < old_num) {
		for (i = old_num - 1; i >= new_num; i--) {
			if (devices[i] &&
			    atomic_read(&devices[i]->open_count) > 0) {
				mutex_unlock(&devices_mutex);
				return -EBUSY;
			}
		}
		for (i = old_num - 1; i >= new_num; i--)
			membuf_destroy_device(i);
	}

	num_devices = new_num;
	mutex_unlock(&devices_mutex);
	return 0;
}

static int get_num_devices(char *buf, const struct kernel_param *kp)
{
	return sprintf(buf, "%d", num_devices);
}

static const struct kernel_param_ops num_devices_ops = {
	.set = set_num_devices,
	.get = get_num_devices,
};

module_param_cb(num_devices, &num_devices_ops, &num_devices, 0644);
MODULE_PARM_DESC(num_devices, "Number of membuf devices");

module_param(default_bufsize, int, 0644);
MODULE_PARM_DESC(default_bufsize, "Default buffer size for new devices (bytes)");

module_param(leak_test, int, 0444);
MODULE_PARM_DESC(leak_test, "If non-zero, intentionally leak this many bytes at init (kmemleak verification only)");

static int __init membuf_init(void)
{
	int ret, i;

	if (num_devices < 0 || num_devices > MAX_DEVICES)
		return -EINVAL;
	if (default_bufsize <= 0)
		return -EINVAL;

	ret = alloc_chrdev_region(&dev_base, 0, MAX_DEVICES, DEVICE_NAME);
	if (ret)
		return ret;

	membuf_class = class_create(DEVICE_NAME);
	if (IS_ERR(membuf_class)) {
		unregister_chrdev_region(dev_base, MAX_DEVICES);
		return PTR_ERR(membuf_class);
	}

	membuf_class->dev_groups = membuf_dev_groups;

	mutex_lock(&devices_mutex);
	for (i = 0; i < num_devices; i++) {
		ret = membuf_create_device(i);
		if (ret) {
			while (--i >= 0)
				membuf_destroy_device(i);
			mutex_unlock(&devices_mutex);
			class_destroy(membuf_class);
			unregister_chrdev_region(dev_base, MAX_DEVICES);
			return ret;
		}
	}
	mutex_unlock(&devices_mutex);

	initialized = true;

	if (leak_test > 0) {
		void *leaked = kzalloc(leak_test, GFP_KERNEL);
		if (leaked)
			pr_warn("membuf: intentionally leaking %d bytes (kmemleak test)\n",
				leak_test);
		/* intentionally not freed — kmemleak should report this */
	}

	pr_info("membuf: loaded, %d device(s), default_bufsize=%d\n",
		num_devices, default_bufsize);
	return 0;
}

static void __exit membuf_exit(void)
{
	int i;

	mutex_lock(&devices_mutex);
	for (i = 0; i < MAX_DEVICES; i++) {
		if (devices[i])
			membuf_destroy_device(i);
	}
	mutex_unlock(&devices_mutex);

	class_destroy(membuf_class);
	unregister_chrdev_region(dev_base, MAX_DEVICES);
	pr_info("membuf: unloaded\n");
}

module_init(membuf_init);
module_exit(membuf_exit);
