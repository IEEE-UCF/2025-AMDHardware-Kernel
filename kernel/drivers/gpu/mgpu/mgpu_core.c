#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/pm_runtime.h>

#include "mgpu_drm.h"
#include "mgpu_regs.h"
#include "mgpu_internal.h"

/* Initialize GPU hardware */
int mgpu_core_init(struct mgpu_device *mdev)
{
    u32 version, caps;
    int ret;
    
    dev_info(mdev->dev, "Initializing GPU core\n");
    
    /* Soft reset the GPU */
    ret = mgpu_core_reset(mdev);
    if (ret) {
        dev_err(mdev->dev, "Failed to reset GPU\n");
        return ret;
    }
    
    /* Read version and capabilities */
    version = mgpu_read(mdev, MGPU_REG_VERSION);
    caps = mgpu_read(mdev, MGPU_REG_CAPS);
    
    mdev->version = version;
    mdev->caps = caps;
    
    dev_info(mdev->dev, "GPU version: %d.%d.%d.%d\n",
             MGPU_VERSION_MAJOR(version),
             MGPU_VERSION_MINOR(version),
             MGPU_VERSION_PATCH(version),
             MGPU_VERSION_BUILD(version));
    
    dev_info(mdev->dev, "GPU capabilities: 0x%08x\n", caps);
    
    /* Log detected features */
    if (caps & MGPU_CAP_VERTEX_SHADER)
        dev_info(mdev->dev, "  - Vertex shader support\n");
    if (caps & MGPU_CAP_FRAGMENT_SHADER)
        dev_info(mdev->dev, "  - Fragment shader support\n");
    if (caps & MGPU_CAP_TEXTURE)
        dev_info(mdev->dev, "  - Texture support\n");
    if (caps & MGPU_CAP_FENCE)
        dev_info(mdev->dev, "  - Fence support\n");
    if (caps & MGPU_CAP_MULTI_QUEUE)
        dev_info(mdev->dev, "  - Multi-queue support\n");
    
    /* Determine number of engines and queues */
    if (caps & MGPU_CAP_MULTI_QUEUE) {
        mdev->num_queues = MGPU_MAX_QUEUES;
        mdev->num_engines = MGPU_MAX_ENGINES;
    } else {
        mdev->num_queues = 1;
        mdev->num_engines = 1;
    }
    
    /* Initialize subcomponents */
    ret = mgpu_irq_init(mdev);
    if (ret) {
        dev_err(mdev->dev, "Failed to initialize IRQ\n");
        return ret;
    }
    
    /* Enable GPU */
    mgpu_write(mdev, MGPU_REG_CONTROL, MGPU_CTRL_ENABLE);
    
    /* Verify GPU is responsive */
    ret = mgpu_core_test_alive(mdev);
    if (ret) {
        dev_err(mdev->dev, "GPU not responding after init\n");
        mgpu_write(mdev, MGPU_REG_CONTROL, 0);
        return ret;
    }
    
    dev_info(mdev->dev, "GPU core initialized successfully\n");
    
    return 0;
}

/* Cleanup GPU core */
void mgpu_core_fini(struct mgpu_device *mdev)
{
    dev_info(mdev->dev, "Shutting down GPU core\n");
    
    /* Disable interrupts */
    mgpu_write(mdev, MGPU_REG_IRQ_ENABLE, 0);
    
    /* Stop GPU */
    mgpu_write(mdev, MGPU_REG_CONTROL, 0);
    
    /* Final reset */
    mgpu_core_reset(mdev);
    
    /* Cleanup IRQ */
    mgpu_irq_fini(mdev);
}

/* Reset GPU hardware */
int mgpu_core_reset(struct mgpu_device *mdev)
{
    int timeout;
    u32 status;
    
    dev_dbg(mdev->dev, "Resetting GPU\n");
    
    /* Assert reset */
    mgpu_write(mdev, MGPU_REG_CONTROL, MGPU_CTRL_RESET);
    
    /* Hold reset for at least 10ms */
    msleep(10);
    
    /* Deassert reset */
    mgpu_write(mdev, MGPU_REG_CONTROL, 0);
    
    /* Wait for GPU to become idle */
    timeout = 100;  /* 100ms timeout */
    while (timeout--) {
        status = mgpu_read(mdev, MGPU_REG_STATUS);
        if (status & MGPU_STATUS_IDLE)
            break;
        msleep(1);
    }
    
    if (timeout <= 0) {
        dev_err(mdev->dev, "GPU reset timeout\n");
        return -ETIMEDOUT;
    }
    
    /* Clear any pending interrupts */
    mgpu_write(mdev, MGPU_REG_IRQ_ACK, 0xFFFFFFFF);
    
    dev_dbg(mdev->dev, "GPU reset complete\n");
    
    return 0;
}

/* Test if GPU is alive and responding */
int mgpu_core_test_alive(struct mgpu_device *mdev)
{
    u32 test_val = 0xDEADBEEF;
    u32 read_val;
    
    /* Write to scratch register */
    mgpu_write(mdev, MGPU_REG_SCRATCH, test_val);
    
    /* Read back */
    read_val = mgpu_read(mdev, MGPU_REG_SCRATCH);
    
    if (read_val != test_val) {
        dev_err(mdev->dev, "GPU scratch test failed: wrote 0x%08x, read 0x%08x\n",
                test_val, read_val);
        return -EIO;
    }
    
    /* Test with inverted pattern */
    test_val = ~test_val;
    mgpu_write(mdev, MGPU_REG_SCRATCH, test_val);
    read_val = mgpu_read(mdev, MGPU_REG_SCRATCH);
    
    if (read_val != test_val) {
        dev_err(mdev->dev, "GPU scratch test failed (inverted): wrote 0x%08x, read 0x%08x\n",
                test_val, read_val);
        return -EIO;
    }
    
    return 0;
}

/* Get GPU status */
u32 mgpu_core_get_status(struct mgpu_device *mdev)
{
    return mgpu_read(mdev, MGPU_REG_STATUS);
}

/* Check if GPU is idle */
bool mgpu_core_is_idle(struct mgpu_device *mdev)
{
    u32 status = mgpu_core_get_status(mdev);
    return (status & MGPU_STATUS_IDLE) && !(status & MGPU_STATUS_BUSY);
}

/* Wait for GPU to become idle */
int mgpu_core_wait_idle(struct mgpu_device *mdev, unsigned int timeout_ms)
{
    unsigned long timeout = jiffies + msecs_to_jiffies(timeout_ms);
    
    while (time_before(jiffies, timeout)) {
        if (mgpu_core_is_idle(mdev))
            return 0;
        
        /* Check for errors */
        if (mgpu_read(mdev, MGPU_REG_STATUS) & MGPU_STATUS_ERROR) {
            dev_err(mdev->dev, "GPU error detected while waiting for idle\n");
            return -EIO;
        }
        
        cpu_relax();
        usleep_range(100, 200);
    }
    
    dev_err(mdev->dev, "Timeout waiting for GPU idle\n");
    return -ETIMEDOUT;
}

/* Handle GPU errors */
void mgpu_core_handle_error(struct mgpu_device *mdev)
{
    u32 status;
    
    status = mgpu_read(mdev, MGPU_REG_STATUS);
    
    if (status & MGPU_STATUS_ERROR) {
        dev_err(mdev->dev, "GPU error detected, status: 0x%08x\n", status);
        
        /* Trigger GPU reset */
        mgpu_reset_schedule(mdev);
    }
}

/* Dump GPU state for debugging */
void mgpu_core_dump_state(struct mgpu_device *mdev)
{
    dev_info(mdev->dev, "=== GPU State Dump ===\n");
    dev_info(mdev->dev, "Version:  0x%08x\n", mgpu_read(mdev, MGPU_REG_VERSION));
    dev_info(mdev->dev, "Caps:     0x%08x\n", mgpu_read(mdev, MGPU_REG_CAPS));
    dev_info(mdev->dev, "Control:  0x%08x\n", mgpu_read(mdev, MGPU_REG_CONTROL));
    dev_info(mdev->dev, "Status:   0x%08x\n", mgpu_read(mdev, MGPU_REG_STATUS));
    dev_info(mdev->dev, "IRQ Status: 0x%08x\n", mgpu_read(mdev, MGPU_REG_IRQ_STATUS));
    dev_info(mdev->dev, "IRQ Enable: 0x%08x\n", mgpu_read(mdev, MGPU_REG_IRQ_ENABLE));
    dev_info(mdev->dev, "CMD Head: 0x%08x\n", mgpu_read(mdev, MGPU_REG_CMD_HEAD));
    dev_info(mdev->dev, "CMD Tail: 0x%08x\n", mgpu_read(mdev, MGPU_REG_CMD_TAIL));
    dev_info(mdev->dev, "Fence Value: 0x%08x\n", mgpu_read(mdev, MGPU_REG_FENCE_VALUE));
    dev_info(mdev->dev, "======================\n");
}