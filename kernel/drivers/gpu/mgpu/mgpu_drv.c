#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include "mgpu_mmio.h"

#define MGPU_NAME "mgpu"

static long mgpu_unlocked_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    /* Dispatch to mgpu_uapi in real driver */
    return -ENOTTY;
}

static const struct file_operations mgpu_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = mgpu_unlocked_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl   = mgpu_unlocked_ioctl,
#endif
};

static struct miscdevice mgpu_misc = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = "mgpu0",
    .fops  = &mgpu_fops,
};

static int __init mgpu_init(void)
{
    pr_info("mgpu: init\n");
    return misc_register(&mgpu_misc);
}

static void __exit mgpu_exit(void)
{
    misc_deregister(&mgpu_misc);
    pr_info("mgpu: exit\n");
}

module_init(mgpu_init);
module_exit(mgpu_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Minimal FPGA GPU driver (mgpu) - GEN1 skeleton");
MODULE_AUTHOR("Rafeed + ChatGPT");
