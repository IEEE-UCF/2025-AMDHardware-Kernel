#include <linux/module.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/jiffies.h>
#include <linux/dma-mapping.h>

#include "mgpu_drm.h"
#include "mgpu_regs.h"

/* Fence context structure */
struct mgpu_fence_context {
    struct mgpu_device *mdev;
    
    /* Fence memory (shared with GPU) */
    u32 *cpu_addr;
    dma_addr_t dma_addr;
    
    /* Current fence value */
    atomic_t seqno;
    
    /* Wait queue for fence waits */
    wait_queue_head_t wait_queue;
    
    /* List of pending waits */
    struct list_head wait_list;
    spinlock_t wait_lock;
};

/* Fence wait entry */
struct mgpu_fence_wait {
    struct list_head list;
    u32 value;
    bool signaled;
};

/* Initialize fence subsystem */
int mgpu_fence_init(struct mgpu_device *mdev)
{
    struct mgpu_fence_context *ctx;
    
    ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
    if (!ctx)
        return -ENOMEM;
    
    ctx->mdev = mdev;
    atomic_set(&ctx->seqno, 0);
    init_waitqueue_head(&ctx->wait_queue);
    INIT_LIST_HEAD(&ctx->wait_list);
    spin_lock_init(&ctx->wait_lock);
    
    /* Allocate fence memory (single page for fence values) */
    ctx->cpu_addr = dma_alloc_coherent(mdev->dev, PAGE_SIZE,
                                       &ctx->dma_addr, GFP_KERNEL);
    if (!ctx->cpu_addr) {
        kfree(ctx);
        return -ENOMEM;
    }
    
    /* Clear fence memory */
    memset(ctx->cpu_addr, 0, PAGE_SIZE);
    
    /* Program fence base address in hardware */
    mgpu_write(mdev, MGPU_REG_FENCE_ADDR, lower_32_bits(ctx->dma_addr));
    
    mdev->fence_ctx = ctx;
    
    dev_info(mdev->dev, "Fence context initialized at 0x%pad\n", &ctx->dma_addr);
    
    return 0;
}

/* Clean up fence subsystem */
void mgpu_fence_fini(struct mgpu_device *mdev)
{
    struct mgpu_fence_context *ctx = mdev->fence_ctx;
    struct mgpu_fence_wait *wait, *tmp;
    
    if (!ctx)
        return;
    
    /* Clear fence address in hardware */
    mgpu_write(mdev, MGPU_REG_FENCE_ADDR, 0);
    
    /* Wake up any waiters */
    wake_up_all(&ctx->wait_queue);
    
    /* Clean up wait list */
    spin_lock(&ctx->wait_lock);
    list_for_each_entry_safe(wait, tmp, &ctx->wait_list, list) {
        list_del(&wait->list);
        kfree(wait);
    }
    spin_unlock(&ctx->wait_lock);
    
    /* Free fence memory */
    if (ctx->cpu_addr)
        dma_free_coherent(mdev->dev, PAGE_SIZE,
                         ctx->cpu_addr, ctx->dma_addr);
    
    kfree(ctx);
    mdev->fence_ctx = NULL;
}

/* Get next fence value */
u32 mgpu_fence_next(struct mgpu_device *mdev)
{
    struct mgpu_fence_context *ctx = mdev->fence_ctx;
    
    if (!ctx)
        return 0;
    
    return atomic_inc_return(&ctx->seqno);
}

/* Check if fence is signaled */
bool mgpu_fence_signaled(struct mgpu_device *mdev, u64 fence_addr, u32 fence_value)
{
    struct mgpu_fence_context *ctx = mdev->fence_ctx;
    u32 *fence_ptr;
    u32 current_value;
    
    if (!ctx)
        return true;
    
    /* Calculate offset into fence memory */
    if (fence_addr < ctx->dma_addr || 
        fence_addr >= ctx->dma_addr + PAGE_SIZE)
        return true;  /* Invalid address, consider signaled */
    
    fence_ptr = ctx->cpu_addr + ((fence_addr - ctx->dma_addr) / sizeof(u32));
    
    /* Read current fence value */
    current_value = READ_ONCE(*fence_ptr);
    
    /* Fence is signaled if current value >= expected value */
    return current_value >= fence_value;
}

/* Process fence interrupts */
void mgpu_fence_process(struct mgpu_device *mdev)
{
    struct mgpu_fence_context *ctx = mdev->fence_ctx;
    struct mgpu_fence_wait *wait, *tmp;
    bool wake = false;
    
    if (!ctx)
        return;
    
    /* Check all pending waits */
    spin_lock(&ctx->wait_lock);
    list_for_each_entry_safe(wait, tmp, &ctx->wait_list, list) {
        /* Check if this fence is now signaled */
        if (!wait->signaled) {
            /* Read hardware fence value */
            u32 hw_value = mgpu_read(mdev, MGPU_REG_FENCE_VALUE);
            if (hw_value >= wait->value) {
                wait->signaled = true;
                wake = true;
            }
        }
    }
    spin_unlock(&ctx->wait_lock);
    
    /* Wake up waiters if any fence was signaled */
    if (wake)
        wake_up_all(&ctx->wait_queue);
}

/* Wait for fence */
int mgpu_wait_fence(struct mgpu_device *mdev, struct mgpu_wait_fence *args)
{
    struct mgpu_fence_context *ctx = mdev->fence_ctx;
    struct mgpu_fence_wait wait_entry;
    unsigned long timeout;
    long ret;
    
    if (!ctx)
        return -ENODEV;
    
    /* Check if already signaled */
    if (mgpu_fence_signaled(mdev, args->fence_addr, args->fence_value))
        return 0;
    
    /* Set up wait entry */
    wait_entry.value = args->fence_value;
    wait_entry.signaled = false;
    INIT_LIST_HEAD(&wait_entry.list);
    
    /* Add to wait list */
    spin_lock(&ctx->wait_lock);
    list_add_tail(&wait_entry.list, &ctx->wait_list);
    spin_unlock(&ctx->wait_lock);
    
    /* Calculate timeout */
    if (args->timeout_ms == 0)
        timeout = MAX_SCHEDULE_TIMEOUT;
    else
        timeout = msecs_to_jiffies(args->timeout_ms);
    
    /* Wait for fence */
    ret = wait_event_interruptible_timeout(ctx->wait_queue,
                                          mgpu_fence_signaled(mdev, args->fence_addr, 
                                                            args->fence_value),
                                          timeout);
    
    /* Remove from wait list */
    spin_lock(&ctx->wait_lock);
    list_del(&wait_entry.list);
    spin_unlock(&ctx->wait_lock);
    
    if (ret == 0)
        return -ETIMEDOUT;
    else if (ret < 0)
        return ret;
    
    return 0;
}

/* Emit fence command */
int mgpu_fence_emit(struct mgpu_device *mdev, u64 fence_addr, u32 fence_value)
{
    struct mgpu_fence_context *ctx = mdev->fence_ctx;
    
    if (!ctx)
        return -ENODEV;
    
    /* Validate fence address */
    if (fence_addr < ctx->dma_addr || 
        fence_addr >= ctx->dma_addr + PAGE_SIZE)
        return -EINVAL;
    
    /* Ok, just for a note, the GPU will write the fence value when it processes the fence command */
    /* This is handled by the command submission code */
    
    return 0;
}