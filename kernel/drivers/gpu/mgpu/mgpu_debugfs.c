/*
 * MGPU DebugFS Interface
 *
 * Copyright (C) 2024 
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/slab.h>

#include "mgpu_drm.h"
#include "mgpu_regs.h"
#include "mgpu_internal.h"

/* DebugFS root directory */
static struct dentry *mgpu_debugfs_root;

/* Register dump */
static int mgpu_regs_show(struct seq_file *m, void *unused)
{
    struct mgpu_device *mdev = m->private;
    int i;
    
    seq_printf(m, "=== MGPU Register Dump ===\n\n");
    
    /* Base registers */
    seq_printf(m, "Base Registers:\n");
    seq_printf(m, "  VERSION:       0x%08x\n", mgpu_read(mdev, MGPU_REG_VERSION));
    seq_printf(m, "  CAPS:          0x%08x\n", mgpu_read(mdev, MGPU_REG_CAPS));
    seq_printf(m, "  CONTROL:       0x%08x\n", mgpu_read(mdev, MGPU_REG_CONTROL));
    seq_printf(m, "  STATUS:        0x%08x\n", mgpu_read(mdev, MGPU_REG_STATUS));
    seq_printf(m, "  SCRATCH:       0x%08x\n", mgpu_read(mdev, MGPU_REG_SCRATCH));
    
    /* Interrupt registers */
    seq_printf(m, "\nInterrupt Registers:\n");
    seq_printf(m, "  IRQ_STATUS:    0x%08x\n", mgpu_read(mdev, MGPU_REG_IRQ_STATUS));
    seq_printf(m, "  IRQ_ENABLE:    0x%08x\n", mgpu_read(mdev, MGPU_REG_IRQ_ENABLE));
    
    /* Command queue registers */
    seq_printf(m, "\nCommand Queue Registers:\n");
    for (i = 0; i < mdev->num_queues; i++) {
        u32 base_offset = i * 0x10;
        seq_printf(m, "  Queue %d:\n", i);
        seq_printf(m, "    CMD_BASE:    0x%08x\n", 
                   mgpu_read(mdev, MGPU_REG_CMD_BASE + base_offset));
        seq_printf(m, "    CMD_SIZE:    0x%08x\n", 
                   mgpu_read(mdev, MGPU_REG_CMD_SIZE + base_offset));
        seq_printf(m, "    CMD_HEAD:    0x%08x\n", 
                   mgpu_read(mdev, MGPU_REG_CMD_HEAD + base_offset));
        seq_printf(m, "    CMD_TAIL:    0x%08x\n", 
                   mgpu_read(mdev, MGPU_REG_CMD_TAIL + base_offset));
    }
    
    /* Fence registers */
    seq_printf(m, "\nFence Registers:\n");
    seq_printf(m, "  FENCE_ADDR:    0x%08x\n", mgpu_read(mdev, MGPU_REG_FENCE_ADDR));
    seq_printf(m, "  FENCE_VALUE:   0x%08x\n", mgpu_read(mdev, MGPU_REG_FENCE_VALUE));
    
    /* Vertex registers */
    seq_printf(m, "\nVertex Registers:\n");
    seq_printf(m, "  VERTEX_BASE:   0x%08x\n", mgpu_read(mdev, MGPU_REG_VERTEX_BASE));
    seq_printf(m, "  VERTEX_COUNT:  0x%08x\n", mgpu_read(mdev, MGPU_REG_VERTEX_COUNT));
    seq_printf(m, "  VERTEX_STRIDE: 0x%08x\n", mgpu_read(mdev, MGPU_REG_VERTEX_STRIDE));
    
    /* Shader registers */
    seq_printf(m, "\nShader Registers:\n");
    seq_printf(m, "  SHADER_PC:     0x%08x\n", mgpu_read(mdev, MGPU_REG_SHADER_PC));
    seq_printf(m, "  SHADER_ADDR:   0x%08x\n", mgpu_read(mdev, MGPU_REG_SHADER_ADDR));
    seq_printf(m, "  SHADER_CTRL:   0x%08x\n", mgpu_read(mdev, MGPU_REG_SHADER_CTRL));
    
    return 0;
}

static int mgpu_regs_open(struct inode *inode, struct file *file)
{
    return single_open(file, mgpu_regs_show, inode->i_private);
}

static const struct file_operations mgpu_regs_fops = {
    .owner = THIS_MODULE,
    .open = mgpu_regs_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

/* Status information */
static int mgpu_status_show(struct seq_file *m, void *unused)
{
    struct mgpu_device *mdev = m->private;
    u32 status, control;
    
    status = mgpu_read(mdev, MGPU_REG_STATUS);
    control = mgpu_read(mdev, MGPU_REG_CONTROL);
    
    seq_printf(m, "=== MGPU Status ===\n\n");
    
    seq_printf(m, "Hardware State:\n");
    seq_printf(m, "  Idle:          %s\n", (status & MGPU_STATUS_IDLE) ? "Yes" : "No");
    seq_printf(m, "  Busy:          %s\n", (status & MGPU_STATUS_BUSY) ? "Yes" : "No");
    seq_printf(m, "  Error:         %s\n", (status & MGPU_STATUS_ERROR) ? "Yes" : "No");
    seq_printf(m, "  Halted:        %s\n", (status & MGPU_STATUS_HALTED) ? "Yes" : "No");
    seq_printf(m, "  Fence Done:    %s\n", (status & MGPU_STATUS_FENCE_DONE) ? "Yes" : "No");
    seq_printf(m, "  Cmd Empty:     %s\n", (status & MGPU_STATUS_CMD_EMPTY) ? "Yes" : "No");
    seq_printf(m, "  Cmd Full:      %s\n", (status & MGPU_STATUS_CMD_FULL) ? "Yes" : "No");
    
    seq_printf(m, "\nControl State:\n");
    seq_printf(m, "  Enabled:       %s\n", (control & MGPU_CTRL_ENABLE) ? "Yes" : "No");
    seq_printf(m, "  Reset:         %s\n", (control & MGPU_CTRL_RESET) ? "Yes" : "No");
    seq_printf(m, "  Paused:        %s\n", (control & MGPU_CTRL_PAUSE) ? "Yes" : "No");
    seq_printf(m, "  Single Step:   %s\n", (control & MGPU_CTRL_SINGLE_STEP) ? "Yes" : "No");
    seq_printf(m, "  Perf Counter:  %s\n", (control & MGPU_CTRL_PERF_COUNTER) ? "Yes" : "No");
    
    if (mdev->cmd_ring) {
        struct mgpu_ring *ring = mdev->cmd_ring;
        seq_printf(m, "\nCommand Ring:\n");
        seq_printf(m, "  Head:          %u\n", mgpu_read(mdev, MGPU_REG_CMD_HEAD));
        seq_printf(m, "  Tail:          %u\n", mgpu_read(mdev, MGPU_REG_CMD_TAIL));
        seq_printf(m, "  Submitted:     %llu\n", ring->submitted_cmds);
        seq_printf(m, "  Completed:     %llu\n", ring->completed_cmds);
    }
    
    return 0;
}

static int mgpu_status_open(struct inode *inode, struct file *file)
{
    return single_open(file, mgpu_status_show, inode->i_private);
}

static const struct file_operations mgpu_status_fops = {
    .owner = THIS_MODULE,
    .open = mgpu_status_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

/* Capabilities */
static int mgpu_caps_show(struct seq_file *m, void *unused)
{
    struct mgpu_device *mdev = m->private;
    u32 caps = mdev->caps;
    u32 version = mdev->version;
    
    seq_printf(m, "=== MGPU Capabilities ===\n\n");
    
    seq_printf(m, "Version: %d.%d.%d (build %d)\n",
               MGPU_VERSION_MAJOR(version),
               MGPU_VERSION_MINOR(version),
               MGPU_VERSION_PATCH(version),
               MGPU_VERSION_BUILD(version));
    
    seq_printf(m, "\nCapabilities (0x%08x):\n", caps);
    seq_printf(m, "  Vertex Shader:    %s\n", (caps & MGPU_CAP_VERTEX_SHADER) ? "Yes" : "No");
    seq_printf(m, "  Fragment Shader:  %s\n", (caps & MGPU_CAP_FRAGMENT_SHADER) ? "Yes" : "No");
    seq_printf(m, "  Texture:          %s\n", (caps & MGPU_CAP_TEXTURE) ? "Yes" : "No");
    seq_printf(m, "  Float16:          %s\n", (caps & MGPU_CAP_FLOAT16) ? "Yes" : "No");
    seq_printf(m, "  Float32:          %s\n", (caps & MGPU_CAP_FLOAT32) ? "Yes" : "No");
    seq_printf(m, "  Int32:            %s\n", (caps & MGPU_CAP_INT32) ? "Yes" : "No");
    seq_printf(m, "  Atomic:           %s\n", (caps & MGPU_CAP_ATOMIC) ? "Yes" : "No");
    seq_printf(m, "  Fence:            %s\n", (caps & MGPU_CAP_FENCE) ? "Yes" : "No");
    seq_printf(m, "  Multi Queue:      %s\n", (caps & MGPU_CAP_MULTI_QUEUE) ? "Yes" : "No");
    seq_printf(m, "  Preemption:       %s\n", (caps & MGPU_CAP_PREEMPTION) ? "Yes" : "No");
    
    seq_printf(m, "\nLimits:\n");
    seq_printf(m, "  Queues:           %u\n", mdev->num_queues);
    seq_printf(m, "  Engines:          %u\n", mdev->num_engines);
    seq_printf(m, "  Instruction Mem:  %u KB\n", MGPU_REG_INSTR_MEM_SIZE / 1024);
    seq_printf(m, "  Max Ring Size:    %u KB\n", MGPU_RING_SIZE_MAX / 1024);
    
    return 0;
}

static int mgpu_caps_open(struct inode *inode, struct file *file)
{
    return single_open(file, mgpu_caps_show, inode->i_private);
}

static const struct file_operations mgpu_caps_fops = {
    .owner = THIS_MODULE,
    .open = mgpu_caps_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

/* Buffer objects list */
static int mgpu_bo_list_show(struct seq_file *m, void *unused)
{
    struct mgpu_device *mdev = m->private;
    struct mgpu_bo *bo;
    int count = 0;
    size_t total_size = 0;
    
    seq_printf(m, "=== Buffer Objects ===\n\n");
    seq_printf(m, "Handle    Size        DMA Addr    Flags      Refs\n");
    seq_printf(m, "------------------------------------------------------\n");
    
    mutex_lock(&mdev->bo_lock);
    list_for_each_entry(bo, &mdev->bo_list, list) {
        seq_printf(m, "%-8u  %-10zu  0x%08llx  0x%08x  %d\n",
                   bo->handle,
                   bo->size,
                   (u64)bo->dma_addr,
                   bo->flags,
                   kref_read(&bo->refcount));
        count++;
        total_size += bo->size;
    }
    mutex_unlock(&mdev->bo_lock);
    
    seq_printf(m, "\nTotal: %d objects, %zu bytes\n", count, total_size);
    
    return 0;
}

static int mgpu_bo_list_open(struct inode *inode, struct file *file)
{
    return single_open(file, mgpu_bo_list_show, inode->i_private);
}

static const struct file_operations mgpu_bo_list_fops = {
    .owner = THIS_MODULE,
    .open = mgpu_bo_list_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

/* Shader info */
static int mgpu_shaders_show(struct seq_file *m, void *unused)
{
    struct mgpu_device *mdev = m->private;
    struct mgpu_shader_mgr *mgr = mdev->shader_mgr;
    int i;
    
    if (!mgr) {
        seq_printf(m, "No shader manager initialized\n");
        return 0;
    }
    
    seq_printf(m, "=== Loaded Shaders ===\n\n");
    seq_printf(m, "Slot  Type      Size     Loaded\n");
    seq_printf(m, "--------------------------------\n");
    
    mutex_lock(&mgr->lock);
    for (i = 0; i < MGPU_MAX_SHADER_SLOTS; i++) {
        if (mgr->slots[i].loaded) {
            const char *type_str = "Unknown";
            switch (mgr->slots[i].type) {
            case MGPU_SHADER_VERTEX:
                type_str = "Vertex";
                break;
            case MGPU_SHADER_FRAGMENT:
                type_str = "Fragment";
                break;
            case MGPU_SHADER_COMPUTE:
                type_str = "Compute";
                break;
            }
            
            seq_printf(m, "%-4d  %-8s  %-7zu  Yes\n",
                       i, type_str, mgr->slots[i].size);
        }
    }
    mutex_unlock(&mgr->lock);
    
    return 0;
}

static int mgpu_shaders_open(struct inode *inode, struct file *file)
{
    return single_open(file, mgpu_shaders_show, inode->i_private);
}

static const struct file_operations mgpu_shaders_fops = {
    .owner = THIS_MODULE,
    .open = mgpu_shaders_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

/* Interrupt statistics */
static int mgpu_irq_stats_show(struct seq_file *m, void *unused)
{
    struct mgpu_device *mdev = m->private;
    
    seq_printf(m, "=== Interrupt Statistics ===\n\n");
    
    /* Get interrupt line from /proc/interrupts */
    seq_printf(m, "IRQ Line: %d\n", mdev->irq);
    seq_printf(m, "IRQ Status: 0x%08x\n", mgpu_read(mdev, MGPU_REG_IRQ_STATUS));
    seq_printf(m, "IRQ Enable: 0x%08x\n", mgpu_read(mdev, MGPU_REG_IRQ_ENABLE));
    
    /* TODO: Add actual interrupt counters when available */
    
    return 0;
}

static int mgpu_irq_stats_open(struct inode *inode, struct file *file)
{
    return single_open(file, mgpu_irq_stats_show, inode->i_private);
}

static const struct file_operations mgpu_irq_stats_fops = {
    .owner = THIS_MODULE,
    .open = mgpu_irq_stats_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

/* Test register read/write */
static ssize_t mgpu_test_reg_write(struct file *file, const char __user *buf,
                                   size_t count, loff_t *ppos)
{
    struct mgpu_device *mdev = file->private_data;
    char kbuf[64];
    u32 offset, value;
    int ret;
    
    if (count >= sizeof(kbuf))
        return -EINVAL;
    
    if (copy_from_user(kbuf, buf, count))
        return -EFAULT;
    
    kbuf[count] = '\0';
    
    /* Parse "offset value" format */
    ret = sscanf(kbuf, "%x %x", &offset, &value);
    if (ret != 2)
        return -EINVAL;
    
    /* Validate offset */
    if (offset >= resource_size(mdev->mmio_res))
        return -EINVAL;
    
    /* Write register */
    mgpu_write(mdev, offset, value);
    
    dev_info(mdev->dev, "Wrote 0x%08x to register 0x%04x\n", value, offset);
    
    return count;
}

static ssize_t mgpu_test_reg_read(struct file *file, char __user *buf,
                                  size_t count, loff_t *ppos)
{
    struct mgpu_device *mdev = file->private_data;
    char kbuf[256];
    int len;
    
    if (*ppos != 0)
        return 0;
    
    /* Show scratch register value as example */
    len = snprintf(kbuf, sizeof(kbuf),
                   "Usage: echo \"offset value\" > test_reg\n"
                   "Scratch register (0x10): 0x%08x\n",
                   mgpu_read(mdev, MGPU_REG_SCRATCH));
    
    if (len > count)
        len = count;
    
    if (copy_to_user(buf, kbuf, len))
        return -EFAULT;
    
    *ppos += len;
    return len;
}

static const struct file_operations mgpu_test_reg_fops = {
    .owner = THIS_MODULE,
    .open = simple_open,
    .write = mgpu_test_reg_write,
    .read = mgpu_test_reg_read,
};

/* GPU reset trigger */
static ssize_t mgpu_reset_write(struct file *file, const char __user *buf,
                                size_t count, loff_t *ppos)
{
    struct mgpu_device *mdev = file->private_data;
    char kbuf[16];
    
    if (count >= sizeof(kbuf))
        return -EINVAL;
    
    if (copy_from_user(kbuf, buf, count))
        return -EFAULT;
    
    kbuf[count] = '\0';
    
    /* Any write triggers reset */
    dev_info(mdev->dev, "Manual GPU reset triggered via debugfs\n");
    mgpu_reset_schedule(mdev);
    
    return count;
}

static const struct file_operations mgpu_reset_fops = {
    .owner = THIS_MODULE,
    .open = simple_open,
    .write = mgpu_reset_write,
};

/* Initialize debugfs */
int mgpu_debugfs_init(struct mgpu_device *mdev)
{
    struct dentry *root;
    
    /* Create root directory */
    root = debugfs_create_dir(DRIVER_NAME, NULL);
    if (IS_ERR(root))
        return PTR_ERR(root);
    
    mdev->debugfs_root = root;
    
    /* Create debugfs files */
    debugfs_create_file("regs", 0444, root, mdev, &mgpu_regs_fops);
    debugfs_create_file("status", 0444, root, mdev, &mgpu_status_fops);
    debugfs_create_file("caps", 0444, root, mdev, &mgpu_caps_fops);
    debugfs_create_file("bo_list", 0444, root, mdev, &mgpu_bo_list_fops);
    debugfs_create_file("shaders", 0444, root, mdev, &mgpu_shaders_fops);
    debugfs_create_file("irq_stats", 0444, root, mdev, &mgpu_irq_stats_fops);
    debugfs_create_file("test_reg", 0644, root, mdev, &mgpu_test_reg_fops);
    debugfs_create_file("reset", 0200, root, mdev, &mgpu_reset_fops);
    
    /* Direct register access */
    debugfs_create_x32("version", 0444, root, &mdev->version);
    debugfs_create_x32("caps_raw", 0444, root, &mdev->caps);
    debugfs_create_u32("num_queues", 0444, root, &mdev->num_queues);
    debugfs_create_u32("num_engines", 0444, root, &mdev->num_engines);
    
    dev_info(mdev->dev, "DebugFS interface initialized at /sys/kernel/debug/%s\n",
             DRIVER_NAME);
    
    return 0;
}

/* Cleanup debugfs */
void mgpu_debugfs_fini(struct mgpu_device *mdev)
{
    if (mdev->debugfs_root) {
        debugfs_remove_recursive(mdev->debugfs_root);
        mdev->debugfs_root = NULL;
    }
}

MODULE_DESCRIPTION("MGPU DebugFS Interface");
MODULE_AUTHOR("Rafeed Khan");
MODULE_LICENSE("GPL v2");