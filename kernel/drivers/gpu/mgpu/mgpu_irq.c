#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>

#include "mgpu_drm.h"
#include "mgpu_regs.h"
#include "mgpu_internal.h"

/* IRQ handler - top half */
static irqreturn_t mgpu_irq_handler(int irq, void *arg)
{
    struct mgpu_device *mdev = arg;
    u32 status;
    
    /* Read interrupt status */
    status = mgpu_read(mdev, MGPU_REG_IRQ_STATUS);
    
    /* No interrupts pending */
    if (!status)
        return IRQ_NONE;
    
    /* Acknowledge interrupts immediately */
    mgpu_write(mdev, MGPU_REG_IRQ_ACK, status);
    
    /* Save status for bottom half */
    mdev->irq_status |= status;
    
    /* Schedule tasklet for bottom half processing */
    tasklet_schedule(&mdev->irq_tasklet);
    
    return IRQ_HANDLED;
}

/* IRQ tasklet - bottom half */
static void mgpu_irq_tasklet_func(unsigned long data)
{
    struct mgpu_device *mdev = (struct mgpu_device *)data;
    u32 status;
    unsigned long flags;
    
    /* Get and clear saved status */
    spin_lock_irqsave(&mdev->irq_lock, flags);
    status = mdev->irq_status;
    mdev->irq_status = 0;
    spin_unlock_irqrestore(&mdev->irq_lock, flags);
    
    /* Process each interrupt type */
    if (status & MGPU_IRQ_CMD_COMPLETE) {
        dev_dbg(mdev->dev, "Command complete IRQ\n");
        mgpu_cmdq_irq_handler(mdev);
    }
    
    if (status & MGPU_IRQ_ERROR) {
        dev_err(mdev->dev, "GPU error IRQ\n");
        mgpu_core_handle_error(mdev);
    }
    
    if (status & MGPU_IRQ_FENCE) {
        dev_dbg(mdev->dev, "Fence IRQ\n");
        mgpu_fence_process(mdev);
    }
    
    if (status & MGPU_IRQ_QUEUE_EMPTY) {
        dev_dbg(mdev->dev, "Queue empty IRQ\n");
        /* Wake up any threads waiting for queue space */
        wake_up(&mdev->queue_wait);
    }
    
    if (status & MGPU_IRQ_SHADER_HALT) {
        dev_warn(mdev->dev, "Shader halt IRQ\n");
        mgpu_shader_handle_halt(mdev);
    }
    
    if (status & MGPU_IRQ_PERF_COUNTER) {
        dev_dbg(mdev->dev, "Performance counter IRQ\n");
        mgpu_pm_handle_perf_irq(mdev);
    }
}

/* Initialize IRQ subsystem */
int mgpu_irq_init(struct mgpu_device *mdev)
{
    int ret;
    
    dev_info(mdev->dev, "Initializing IRQ subsystem\n");
    
    /* Initialize IRQ state */
    spin_lock_init(&mdev->irq_lock);
    mdev->irq_status = 0;
    
    /* Initialize tasklet */
    tasklet_init(&mdev->irq_tasklet, mgpu_irq_tasklet_func,
                (unsigned long)mdev);
    
    /* Initialize wait queues */
    init_waitqueue_head(&mdev->queue_wait);
    init_waitqueue_head(&mdev->fence_wait);
    
    /* Request IRQ */
    ret = request_irq(mdev->irq, mgpu_irq_handler,
                     IRQF_SHARED, "mgpu", mdev);
    if (ret) {
        dev_err(mdev->dev, "Failed to request IRQ %d: %d\n", mdev->irq, ret);
        return ret;
    }
    
    /* Enable interrupts in hardware */
    mgpu_irq_enable(mdev);
    
    dev_info(mdev->dev, "IRQ %d registered\n", mdev->irq);
    
    return 0;
}

/* Cleanup IRQ subsystem */
void mgpu_irq_fini(struct mgpu_device *mdev)
{
    dev_info(mdev->dev, "Cleaning up IRQ subsystem\n");
    
    /* Disable all interrupts */
    mgpu_irq_disable(mdev);
    
    /* Free IRQ */
    if (mdev->irq >= 0)
        free_irq(mdev->irq, mdev);
    
    /* Kill tasklet */
    tasklet_kill(&mdev->irq_tasklet);
}

/* Enable interrupts */
void mgpu_irq_enable(struct mgpu_device *mdev)
{
    u32 mask = MGPU_IRQ_CMD_COMPLETE |
               MGPU_IRQ_ERROR |
               MGPU_IRQ_FENCE |
               MGPU_IRQ_QUEUE_EMPTY;
    
    /* Enable shader halt IRQ only if debugging */
#ifdef CONFIG_MGPU_DEBUG
    mask |= MGPU_IRQ_SHADER_HALT;
#endif
    
    /* Enable performance counter IRQ if profiling */
    if (mdev->profiling_enabled)
        mask |= MGPU_IRQ_PERF_COUNTER;
    
    mgpu_write(mdev, MGPU_REG_IRQ_ENABLE, mask);
    
    dev_dbg(mdev->dev, "Enabled IRQs: 0x%08x\n", mask);
}

/* Disable interrupts */
void mgpu_irq_disable(struct mgpu_device *mdev)
{
    /* Disable all interrupts */
    mgpu_write(mdev, MGPU_REG_IRQ_ENABLE, 0);
    
    /* Clear any pending interrupts */
    mgpu_write(mdev, MGPU_REG_IRQ_ACK, 0xFFFFFFFF);
}

/* Suspend IRQ handling */
int mgpu_irq_suspend(struct mgpu_device *mdev)
{
    /* Disable interrupts */
    mgpu_irq_disable(mdev);
    
    /* Synchronize IRQ */
    synchronize_irq(mdev->irq);
    
    /* Ensure tasklet is not running */
    tasklet_disable(&mdev->irq_tasklet);
    
    return 0;
}

/* Resume IRQ handling */
int mgpu_irq_resume(struct mgpu_device *mdev)
{
    /* Clear any stale interrupts */
    mgpu_write(mdev, MGPU_REG_IRQ_ACK, 0xFFFFFFFF);
    mdev->irq_status = 0;
    
    /* Re-enable tasklet */
    tasklet_enable(&mdev->irq_tasklet);
    
    /* Re-enable interrupts */
    mgpu_irq_enable(mdev);
    
    return 0;
}

/* Force an interrupt (for testing) */
void mgpu_irq_force(struct mgpu_device *mdev, u32 irq_mask)
{
    unsigned long flags;
    
    spin_lock_irqsave(&mdev->irq_lock, flags);
    mdev->irq_status |= irq_mask;
    spin_unlock_irqrestore(&mdev->irq_lock, flags);
    
    tasklet_schedule(&mdev->irq_tasklet);
}

/* Wait for specific interrupt */
int mgpu_irq_wait(struct mgpu_device *mdev, u32 irq_mask, unsigned long timeout_ms)
{
    DEFINE_WAIT(wait);
    unsigned long timeout = jiffies + msecs_to_jiffies(timeout_ms);
    int ret = 0;
    
    while (time_before(jiffies, timeout)) {
        prepare_to_wait(&mdev->fence_wait, &wait, TASK_INTERRUPTIBLE);
        
        if (mdev->last_irq & irq_mask)
            break;
        
        if (signal_pending(current)) {
            ret = -ERESTARTSYS;
            break;
        }
        
        schedule_timeout(HZ / 100);  /* 10ms */
    }
    
    finish_wait(&mdev->fence_wait, &wait);
    
    if (ret == 0 && !(mdev->last_irq & irq_mask))
        ret = -ETIMEDOUT;
    
    return ret;
}