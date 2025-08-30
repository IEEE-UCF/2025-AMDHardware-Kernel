#include "mgpu_drm.h"
#include "mgpu_regs.h"
#include "mgpu_internal.h"

/* Reset work handler */
static void mgpu_reset_work(struct work_struct *work)
{
    struct mgpu_device *mdev = container_of(work, struct mgpu_device,
                                           reset_work.work);
    int ret;
    
    dev_warn(mdev->dev, "GPU reset initiated\n");
    
    /* Set reset flag */
    atomic_set(&mdev->in_reset, 1);
    
    /* Stop all submissions */
    mgpu_cmdq_stop(mdev);
    
    /* Wait for GPU to idle or timeout */
    ret = mgpu_core_wait_idle(mdev, 1000);
    if (ret) {
        dev_err(mdev->dev, "GPU failed to idle before reset\n");
    }
    
    /* Save GPU state for debugging */
    mgpu_core_dump_state(mdev);
    
    /* Disable interrupts */
    mgpu_irq_disable(mdev);
    
    /* Perform hardware reset */
    ret = mgpu_reset_hw(mdev);
    if (ret) {
        dev_err(mdev->dev, "Hardware reset failed: %d\n", ret);
        goto out;
    }
    
    /* Reinitialize hardware */
    ret = mgpu_core_init(mdev);
    if (ret) {
        dev_err(mdev->dev, "Failed to reinitialize after reset: %d\n", ret);
        goto out;
    }
    
    /* Restore command queues */
    ret = mgpu_cmdq_resume(mdev);
    if (ret) {
        dev_err(mdev->dev, "Failed to resume command queues: %d\n", ret);
        goto out;
    }
    
    /* Re-enable interrupts */
    mgpu_irq_enable(mdev);
    
    dev_info(mdev->dev, "GPU reset completed successfully\n");
    
out:
    /* Clear reset flag */
    atomic_set(&mdev->in_reset, 0);
    
    /* Wake up any waiters */
    wake_up_all(&mdev->reset_wait);
}

/* Initialize reset handling */
int mgpu_reset_init(struct mgpu_device *mdev)
{
    /* Initialize reset work */
    INIT_DELAYED_WORK(&mdev->reset_work, mgpu_reset_work);
    
    /* Initialize reset state */
    atomic_set(&mdev->in_reset, 0);
    atomic_set(&mdev->reset_count, 0);
    init_waitqueue_head(&mdev->reset_wait);
    
    return 0;
}

/* Cleanup reset handling */
void mgpu_reset_fini(struct mgpu_device *mdev)
{
    /* Cancel any pending reset work */
    cancel_delayed_work_sync(&mdev->reset_work);
}

/* Schedule a GPU reset */
void mgpu_reset_schedule(struct mgpu_device *mdev)
{
    /* Check if reset already in progress */
    if (atomic_read(&mdev->in_reset)) {
        dev_dbg(mdev->dev, "Reset already in progress\n");
        return;
    }
    
    /* Increment reset counter */
    atomic_inc(&mdev->reset_count);
    
    /* Schedule reset work */
    schedule_delayed_work(&mdev->reset_work, 0);
}

/* Perform hardware reset */
int mgpu_reset_hw(struct mgpu_device *mdev)
{
    int timeout;
    u32 status;
    
    dev_info(mdev->dev, "Performing hardware reset\n");
    
    /* Save current state */
    u32 old_control = mgpu_read(mdev, MGPU_REG_CONTROL);
    
    /* Assert reset */
    mgpu_write(mdev, MGPU_REG_CONTROL, MGPU_CTRL_RESET);
    
    /* Hold reset for 100ms */
    msleep(100);
    
    /* Deassert reset */
    mgpu_write(mdev, MGPU_REG_CONTROL, 0);
    
    /* Wait for GPU to come out of reset */
    timeout = 1000;  /* 1 second timeout */
    while (timeout--) {
        status = mgpu_read(mdev, MGPU_REG_STATUS);
        if (status & MGPU_STATUS_IDLE)
            break;
        msleep(1);
    }
    
    if (timeout <= 0) {
        dev_err(mdev->dev, "GPU failed to come out of reset\n");
        return -ETIMEDOUT;
    }
    
    /* Clear all interrupts */
    mgpu_write(mdev, MGPU_REG_IRQ_ACK, 0xFFFFFFFF);
    
    /* Verify GPU is responsive */
    if (mgpu_core_test_alive(mdev)) {
        dev_err(mdev->dev, "GPU not responsive after reset\n");
        return -EIO;
    }
    
    dev_info(mdev->dev, "Hardware reset completed\n");
    
    return 0;
}

/* Reset a specific engine */
int mgpu_reset_engine(struct mgpu_device *mdev, u32 engine)
{
    /* For now, we only support full GPU reset */
    dev_warn(mdev->dev, "Engine-specific reset not supported, performing full reset\n");
    mgpu_reset_schedule(mdev);
    return 0;
}

/* Wait for reset to complete */
int mgpu_reset_wait(struct mgpu_device *mdev, unsigned long timeout_ms)
{
    int ret;
    
    ret = wait_event_interruptible_timeout(mdev->reset_wait,
                                          !atomic_read(&mdev->in_reset),
                                          msecs_to_jiffies(timeout_ms));
    
    if (ret == 0)
        return -ETIMEDOUT;
    else if (ret < 0)
        return ret;
    
    return 0;
}

/* Check if GPU needs reset */
bool mgpu_reset_needed(struct mgpu_device *mdev)
{
    u32 status = mgpu_read(mdev, MGPU_REG_STATUS);
    
    /* Check for error conditions */
    if (status & MGPU_STATUS_ERROR)
        return true;
    
    if (status & MGPU_STATUS_HALTED)
        return true;
    
    /* Check if GPU is hung (busy but not making progress) */
    if (status & MGPU_STATUS_BUSY) {
        static u32 last_fence = 0;
        u32 current_fence = mgpu_read(mdev, MGPU_REG_FENCE_VALUE);
        
        if (current_fence == last_fence) {
            /* No progress in fence value */
            return true;
        }
        last_fence = current_fence;
    }
    
    return false;
}

/* Reset on error detection */
void mgpu_reset_on_error(struct mgpu_device *mdev)
{
    u32 status = mgpu_read(mdev, MGPU_REG_STATUS);
    
    if (status & MGPU_STATUS_ERROR) {
        dev_err(mdev->dev, "GPU error detected (status: 0x%08x), triggering reset\n", 
                status);
        mgpu_reset_schedule(mdev);
    }
}

/* Get reset statistics */
void mgpu_reset_get_stats(struct mgpu_device *mdev, struct mgpu_reset_stats *stats)
{
    stats->reset_count = atomic_read(&mdev->reset_count);
    stats->in_reset = atomic_read(&mdev->in_reset);
    stats->last_reset_time = mdev->last_reset_time;
}