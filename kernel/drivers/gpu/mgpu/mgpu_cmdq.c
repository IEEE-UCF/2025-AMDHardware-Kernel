#include <linux/module.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>

#include "mgpu_drm.h"
#include "mgpu_regs.h"
#include "mgpu_cmdq.h"

/* Command ring structure */
struct mgpu_ring {
    struct mgpu_device *mdev;
    
    /* Ring buffer memory */
    void *vaddr;
    dma_addr_t dma_addr;
    size_t size;
    
    /* Ring pointers */
    u32 head;  /* Where GPU reads from */
    u32 tail;  /* Where CPU writes to */
    
    /* Queue ID */
    u32 queue_id;
    
    /* Stats */
    u64 submitted_cmds;
    u64 completed_cmds;
};

/* Allocate and initialize a command ring */
struct mgpu_ring *mgpu_ring_create(struct mgpu_device *mdev, size_t size, u32 queue_id)
{
    struct mgpu_ring *ring;
    
    /* Validate size */
    if (size < MGPU_RING_SIZE_MIN || size > MGPU_RING_SIZE_MAX) {
        dev_err(mdev->dev, "Invalid ring size: %zu\n", size);
        return NULL;
    }
    
    /* Ensure size is power of 2 */
    if (!is_power_of_2(size)) {
        size = roundup_pow_of_two(size);
        dev_warn(mdev->dev, "Rounding ring size to %zu\n", size);
    }
    
    ring = kzalloc(sizeof(*ring), GFP_KERNEL);
    if (!ring)
        return NULL;
    
    ring->mdev = mdev;
    ring->size = size;
    ring->queue_id = queue_id;
    
    /* Allocate ring buffer memory */
    ring->vaddr = dma_alloc_coherent(mdev->dev, size,
                                     &ring->dma_addr, GFP_KERNEL);
    if (!ring->vaddr) {
        dev_err(mdev->dev, "Failed to allocate ring buffer\n");
        kfree(ring);
        return NULL;
    }
    
    /* Clear ring buffer */
    memset(ring->vaddr, 0, size);
    
    /* Initialize hardware registers */
    mgpu_write(mdev, MGPU_REG_CMD_BASE + (queue_id * 0x10), 
               lower_32_bits(ring->dma_addr));
    mgpu_write(mdev, MGPU_REG_CMD_SIZE + (queue_id * 0x10), size);
    mgpu_write(mdev, MGPU_REG_CMD_HEAD + (queue_id * 0x10), 0);
    mgpu_write(mdev, MGPU_REG_CMD_TAIL + (queue_id * 0x10), 0);
    
    dev_info(mdev->dev, "Created ring %u, size %zu at 0x%pad\n",
             queue_id, size, &ring->dma_addr);
    
    return ring;
}

/* Destroy a command ring */
void mgpu_ring_destroy(struct mgpu_ring *ring)
{
    struct mgpu_device *mdev;
    
    if (!ring)
        return;
    
    mdev = ring->mdev;
    
    /* Disable ring in hardware */
    mgpu_write(mdev, MGPU_REG_CMD_BASE + (ring->queue_id * 0x10), 0);
    mgpu_write(mdev, MGPU_REG_CMD_SIZE + (ring->queue_id * 0x10), 0);
    
    /* Free ring memory */
    if (ring->vaddr)
        dma_free_coherent(mdev->dev, ring->size,
                         ring->vaddr, ring->dma_addr);
    
    kfree(ring);
}

/* Get available space in ring */
static u32 mgpu_ring_space(struct mgpu_ring *ring)
{
    u32 head, tail, space;
    
    /* Read head from hardware */
    head = mgpu_read(ring->mdev, MGPU_REG_CMD_HEAD + (ring->queue_id * 0x10));
    tail = ring->tail;
    
    if (head <= tail)
        space = ring->size - (tail - head) - 1;
    else
        space = head - tail - 1;
    
    return space;
}

/* Wait for space in ring */
static int mgpu_ring_wait_space(struct mgpu_ring *ring, u32 needed)
{
    int timeout = 1000;  /* 1 second timeout */
    
    while (mgpu_ring_space(ring) < needed) {
        if (timeout-- <= 0) {
            dev_err(ring->mdev->dev, "Ring %u timeout waiting for space\n",
                    ring->queue_id);
            return -ETIMEDOUT;
        }
        msleep(1);
    }
    
    return 0;
}

/* Write data to ring */
static void mgpu_ring_write(struct mgpu_ring *ring, const u32 *data, u32 dwords)
{
    u32 *ring_ptr = ring->vaddr;
    u32 tail = ring->tail;
    u32 ring_size_dw = ring->size / 4;
    u32 i;
    
    for (i = 0; i < dwords; i++) {
        ring_ptr[tail] = data[i];
        tail = (tail + 1) & (ring_size_dw - 1);
    }
    
    /* Memory barrier to ensure writes complete */
    wmb();
    
    ring->tail = tail;
}

/* Kick the ring (doorbell) */
static void mgpu_ring_kick(struct mgpu_ring *ring)
{
    struct mgpu_device *mdev = ring->mdev;
    
    /* Update tail pointer in hardware */
    mgpu_write(mdev, MGPU_REG_CMD_TAIL + (ring->queue_id * 0x10), ring->tail);
    
    /* Ring doorbell */
    mgpu_write(mdev, MGPU_REG_DOORBELL(ring->queue_id), 1);
    
    ring->submitted_cmds++;
}

/* Submit commands to ring */
int mgpu_submit_commands(struct mgpu_device *mdev, struct mgpu_submit *args)
{
    struct mgpu_ring *ring;
    u32 *cmds;
    u32 cmd_dwords;
    int ret;
    
    /* Validate arguments */
    if (!args->commands || !args->cmd_size) {
        dev_err(mdev->dev, "Invalid command buffer\n");
        return -EINVAL;
    }
    
    if (args->queue_id >= MGPU_MAX_QUEUES) {
        dev_err(mdev->dev, "Invalid queue ID %u\n", args->queue_id);
        return -EINVAL;
    }
    
    /* Get or create ring for this queue */
    ring = mdev->cmd_ring;
    if (!ring) {
        ring = mgpu_ring_create(mdev, MGPU_RING_SIZE_MIN, args->queue_id);
        if (!ring)
            return -ENOMEM;
        mdev->cmd_ring = ring;
    }
    
    /* Copy commands from userspace */
    cmd_dwords = args->cmd_size / 4;
    cmds = kmalloc(args->cmd_size, GFP_KERNEL);
    if (!cmds)
        return -ENOMEM;
    
    if (copy_from_user(cmds, (void __user *)args->commands, args->cmd_size)) {
        kfree(cmds);
        return -EFAULT;
    }
    
    /* Lock the ring */
    spin_lock(&mdev->cmd_lock);
    
    /* Wait for space */
    ret = mgpu_ring_wait_space(ring, cmd_dwords);
    if (ret) {
        spin_unlock(&mdev->cmd_lock);
        kfree(cmds);
        return ret;
    }
    
    /* Write commands to ring */
    mgpu_ring_write(ring, cmds, cmd_dwords);
    
    /* Add fence command if requested */
    if (args->flags & MGPU_SUBMIT_FLAGS_FENCE) {
        struct mgpu_cmd_fence fence_cmd = {
            .header = {
                .opcode = MGPU_CMD_FENCE,
                .size = sizeof(fence_cmd) / 4,
                .flags = 0,
            },
            .addr = args->fence_addr,
            .value = args->fence_value,
        };
        
        ret = mgpu_ring_wait_space(ring, sizeof(fence_cmd) / 4);
        if (ret) {
            spin_unlock(&mdev->cmd_lock);
            kfree(cmds);
            return ret;
        }
        
        mgpu_ring_write(ring, (u32 *)&fence_cmd, sizeof(fence_cmd) / 4);
    }
    
    /* Kick the ring */
    mgpu_ring_kick(ring);
    
    spin_unlock(&mdev->cmd_lock);
    
    kfree(cmds);
    
    dev_dbg(mdev->dev, "Submitted %u bytes to queue %u\n",
            args->cmd_size, args->queue_id);
    
    /* Synchronous wait if requested */
    if (args->flags & MGPU_SUBMIT_FLAGS_SYNC) {
        /* Simple polling wait for now */
        int timeout = 1000;
        u32 head, tail;
        
        do {
            head = mgpu_read(mdev, MGPU_REG_CMD_HEAD + (ring->queue_id * 0x10));
            tail = mgpu_read(mdev, MGPU_REG_CMD_TAIL + (ring->queue_id * 0x10));
            if (head == tail)
                break;
            msleep(1);
        } while (timeout-- > 0);
        
        if (timeout <= 0) {
            dev_warn(mdev->dev, "Sync submit timeout\n");
            return -ETIMEDOUT;
        }
    }
    
    return 0;
}

/* Initialize command queue subsystem */
int mgpu_cmdq_init(struct mgpu_device *mdev)
{
    /* Create default ring */
    mdev->cmd_ring = mgpu_ring_create(mdev, MGPU_RING_SIZE_MIN, 0);
    if (!mdev->cmd_ring)
        return -ENOMEM;
    
    return 0;
}

/* Clean up command queue subsystem */
void mgpu_cmdq_fini(struct mgpu_device *mdev)
{
    if (mdev->cmd_ring) {
        mgpu_ring_destroy(mdev->cmd_ring);
        mdev->cmd_ring = NULL;
    }
}

/* Get available space in ring (in dwords) */
static inline u32 mgpu_ring_space(struct mgpu_ring *ring)
{
    u32 head, tail, space;
    
    /* Read head from hardware */
    head = mgpu_read(ring->mdev, MGPU_REG_CMD_HEAD + (ring->queue_id * 0x10));
    tail = ring->tail;
    
    if (head <= tail)
        space = (ring->size / 4) - (tail - head) - 1;  /* Convert to dwords */
    else
        space = head - tail - 1;
    
    return space;
}

/* Suspend command queue processing */
int mgpu_cmdq_suspend(struct mgpu_device *mdev)
{
    struct mgpu_ring *ring = mdev->cmd_ring;
    unsigned long timeout;
    u32 head, tail;
    
    if (!ring)
        return 0;
    
    dev_dbg(mdev->dev, "Suspending command queue\n");
    
    /* Stop accepting new commands */
    ring->enabled = false;
    
    /* Wait for pending commands to complete */
    timeout = jiffies + msecs_to_jiffies(1000);
    while (time_before(jiffies, timeout)) {
        head = mgpu_read(mdev, MGPU_REG_CMD_HEAD + (ring->queue_id * 0x10));
        tail = mgpu_read(mdev, MGPU_REG_CMD_TAIL + (ring->queue_id * 0x10));
        
        if (head == tail) {
            /* Queue is empty */
            break;
        }
        
        msleep(10);
    }
    
    if (head != tail) {
        dev_warn(mdev->dev, "Command queue not empty at suspend (head=%u, tail=%u)\n",
                 head, tail);
        /* Continue anyway */
    }
    
    /* Save queue state */
    ring->last_head = head;
    
    return 0;
}

/* Resume command queue processing */
int mgpu_cmdq_resume(struct mgpu_device *mdev)
{
    struct mgpu_ring *ring = mdev->cmd_ring;
    
    if (!ring)
        return 0;
    
    dev_dbg(mdev->dev, "Resuming command queue\n");
    
    /* Restore queue registers */
    mgpu_write(mdev, MGPU_REG_CMD_BASE + (ring->queue_id * 0x10),
               lower_32_bits(ring->dma_addr));
    mgpu_write(mdev, MGPU_REG_CMD_SIZE + (ring->queue_id * 0x10),
               ring->size);
    
    /* Restore head/tail pointers */
    mgpu_write(mdev, MGPU_REG_CMD_HEAD + (ring->queue_id * 0x10),
               ring->last_head);
    mgpu_write(mdev, MGPU_REG_CMD_TAIL + (ring->queue_id * 0x10),
               ring->tail);
    
    /* Re-enable queue */
    ring->enabled = true;
    
    /* Wake any waiters */
    wake_up_all(&ring->wait_space);
    
    return 0;
}