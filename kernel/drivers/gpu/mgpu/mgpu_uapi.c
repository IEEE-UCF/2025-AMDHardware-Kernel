#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/sync_file.h>
#include <linux/dma-fence.h>

#include "mgpu_drm.h"
#include "mgpu_regs.h"
#include "mgpu_internal.h"

/* Per-file context */
struct mgpu_file_context {
    struct mgpu_device *mdev;
    struct list_head bo_list;      /* Buffer objects created by this context */
    struct list_head fence_list;   /* Pending fences */
    struct mutex lock;
    
    /* Statistics */
    u64 submit_count;
    u64 bo_count;
    u64 fence_count;
};

/* Command validation structure */
struct mgpu_cmd_validator {
    u32 opcode;
    u32 min_size;  /* Minimum size in dwords */
    u32 max_size;  /* Maximum size in dwords */
    bool privileged;
    int (*validate)(struct mgpu_device *mdev, u32 *cmd, u32 size);
};

/* Forward declarations */
static int mgpu_validate_draw_cmd(struct mgpu_device *mdev, u32 *cmd, u32 size);
static int mgpu_validate_dma_cmd(struct mgpu_device *mdev, u32 *cmd, u32 size);
static int mgpu_validate_fence_cmd(struct mgpu_device *mdev, u32 *cmd, u32 size);

/* Command validators based on hardware opcodes from registers.yaml */
static const struct mgpu_cmd_validator cmd_validators[] = {
    { MGPU_CMD_NOP,       1,  1, false, NULL },
    { MGPU_CMD_DRAW,      5,  8, false, mgpu_validate_draw_cmd },
    { MGPU_CMD_COMPUTE,   4,  8, false, NULL },  /* TODO */
    { MGPU_CMD_DMA,       4,  5, false, mgpu_validate_dma_cmd },
    { MGPU_CMD_FENCE,     3,  3, false, mgpu_validate_fence_cmd },
    { MGPU_CMD_WAIT,      2,  3, false, NULL },
    { MGPU_CMD_REG_WRITE, 3,  3, true,  NULL },  /* Privileged */
    { MGPU_CMD_REG_READ,  2,  3, true,  NULL },  /* Privileged */
};

/* Validate DRAW command */
static int mgpu_validate_draw_cmd(struct mgpu_device *mdev, u32 *cmd, u32 size)
{
    struct mgpu_cmd_draw *draw = (struct mgpu_cmd_draw *)cmd;
    
    /* Validate vertex count (hardware supports up to vertex_fetch module limits) */
    if (draw->vertex_count == 0 || draw->vertex_count > 65536) {
        dev_err(mdev->dev, "Invalid vertex count: %u\n", draw->vertex_count);
        return -EINVAL;
    }
    
    /* Instance count must be non-zero */
    if (draw->instance_count == 0) {
        dev_err(mdev->dev, "Invalid instance count: %u\n", draw->instance_count);
        return -EINVAL;
    }
    
    /* Check if vertex data is configured */
    if (mgpu_read(mdev, MGPU_REG_VERTEX_BASE) == 0) {
        dev_err(mdev->dev, "No vertex buffer configured\n");
        return -EINVAL;
    }
    
    return 0;
}

/* Validate DMA command */
static int mgpu_validate_dma_cmd(struct mgpu_device *mdev, u32 *cmd, u32 size)
{
    struct mgpu_cmd_dma *dma = (struct mgpu_cmd_dma *)cmd;
    
    /* Validate DMA size */
    if (dma->size == 0 || dma->size > (16 * 1024 * 1024)) {
        dev_err(mdev->dev, "Invalid DMA size: %u\n", dma->size);
        return -EINVAL;
    }
    
    /* Check alignment */
    if ((dma->src_addr & 3) || (dma->dst_addr & 3) || (dma->size & 3)) {
        dev_err(mdev->dev, "DMA addresses/size must be 4-byte aligned\n");
        return -EINVAL;
    }
    
    /* TODO: Validate addresses against BO ranges */
    
    return 0;
}

/* Validate FENCE command */
static int mgpu_validate_fence_cmd(struct mgpu_device *mdev, u32 *cmd, u32 size)
{
    struct mgpu_cmd_fence *fence = (struct mgpu_cmd_fence *)cmd;
    
    /* Check fence address alignment */
    if (fence->addr & 3) {
        dev_err(mdev->dev, "Fence address must be 4-byte aligned\n");
        return -EINVAL;
    }
    
    /* Fence value of 0 is typically invalid */
    if (fence->value == 0) {
        dev_warn(mdev->dev, "Fence value of 0 may cause issues\n");
    }
    
    return 0;
}

/* Validate command buffer */
static int mgpu_validate_commands(struct mgpu_device *mdev, u32 *cmds, u32 size_bytes)
{
    u32 *ptr = cmds;
    u32 *end = cmds + (size_bytes / 4);
    int ret = 0;
    
    while (ptr < end) {
        struct mgpu_cmd_header *hdr = (struct mgpu_cmd_header *)ptr;
        const struct mgpu_cmd_validator *validator = NULL;
        u32 cmd_size;
        int i;
        
        /* Find validator for this opcode */
        for (i = 0; i < ARRAY_SIZE(cmd_validators); i++) {
            if (cmd_validators[i].opcode == hdr->opcode) {
                validator = &cmd_validators[i];
                break;
            }
        }
        
        if (!validator) {
            dev_err(mdev->dev, "Invalid opcode: 0x%02x\n", hdr->opcode);
            return -EINVAL;
        }
        
        /* Check command size */
        cmd_size = hdr->size;
        if (cmd_size < validator->min_size || cmd_size > validator->max_size) {
            dev_err(mdev->dev, "Invalid size for opcode 0x%02x: %u\n",
                    hdr->opcode, cmd_size);
            return -EINVAL;
        }
        
        /* Check if we have enough data */
        if (ptr + cmd_size > end) {
            dev_err(mdev->dev, "Command buffer truncated\n");
            return -EINVAL;
        }
        
        /* Run specific validator if available */
        if (validator->validate) {
            ret = validator->validate(mdev, ptr, cmd_size);
            if (ret)
                return ret;
        }
        
        /* Check privilege */
        if (validator->privileged) {
            /* TODO: Check if context has privilege */
            dev_warn(mdev->dev, "Privileged command 0x%02x in user buffer\n",
                     hdr->opcode);
            /* For now, skip privileged commands */
            hdr->opcode = MGPU_CMD_NOP;
        }
        
        ptr += cmd_size;
    }
    
    return 0;
}

/* Create file context */
struct mgpu_file_context *mgpu_create_context(struct mgpu_device *mdev)
{
    struct mgpu_file_context *ctx;
    
    ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
    if (!ctx)
        return NULL;
    
    ctx->mdev = mdev;
    INIT_LIST_HEAD(&ctx->bo_list);
    INIT_LIST_HEAD(&ctx->fence_list);
    mutex_init(&ctx->lock);
    
    return ctx;
}

/* Destroy file context */
void mgpu_destroy_context(struct mgpu_file_context *ctx)
{
    struct mgpu_bo *bo, *bo_tmp;
    
    if (!ctx)
        return;
    
    /* Clean up buffer objects */
    mutex_lock(&ctx->lock);
    list_for_each_entry_safe(bo, bo_tmp, &ctx->bo_list, list) {
        list_del(&bo->list);
        mgpu_bo_put(bo);
    }
    mutex_unlock(&ctx->lock);
    
    kfree(ctx);
}

/* Submit command buffer with validation */
int mgpu_submit_validated(struct mgpu_device *mdev, struct mgpu_submit *args,
                          struct mgpu_file_context *ctx)
{
    u32 *cmd_copy = NULL;
    int ret;
    
    /* Sanity checks */
    if (!args->commands || !args->cmd_size) {
        dev_err(mdev->dev, "Invalid command buffer\n");
        return -EINVAL;
    }
    
    if (args->cmd_size > MGPU_RING_SIZE_MAX) {
        dev_err(mdev->dev, "Command buffer too large: %u\n", args->cmd_size);
        return -EINVAL;
    }
    
    if (args->cmd_size & 3) {
        dev_err(mdev->dev, "Command size must be 4-byte aligned\n");
        return -EINVAL;
    }
    
    /* Allocate kernel buffer for commands */
    cmd_copy = kmalloc(args->cmd_size, GFP_KERNEL);
    if (!cmd_copy)
        return -ENOMEM;
    
    /* Copy from userspace */
    if (copy_from_user(cmd_copy, (void __user *)args->commands, args->cmd_size)) {
        ret = -EFAULT;
        goto out;
    }
    
    /* Validate commands */
    ret = mgpu_validate_commands(mdev, cmd_copy, args->cmd_size);
    if (ret) {
        dev_err(mdev->dev, "Command validation failed\n");
        goto out;
    }
    
    /* Update args to point to validated copy */
    args->commands = (uintptr_t)cmd_copy;
    
    /* Submit to hardware */
    ret = mgpu_submit_commands(mdev, args);
    
    /* Update statistics */
    if (!ret && ctx) {
        ctx->submit_count++;
    }
    
out:
    kfree(cmd_copy);
    return ret;
}

/* Execute render job (3D pipeline) */
int mgpu_execute_render(struct mgpu_device *mdev, struct mgpu_render_job *job)
{
    int ret;
    u32 status;
    
    /* Validate render state */
    if (!job->vertex_count || !job->vertex_buffer_handle) {
        dev_err(mdev->dev, "Invalid render job parameters\n");
        return -EINVAL;
    }
    
    /* Look up vertex buffer */
    struct mgpu_bo *vbo = mgpu_bo_lookup(mdev, job->vertex_buffer_handle);
    if (!vbo) {
        dev_err(mdev->dev, "Invalid vertex buffer handle\n");
        return -EINVAL;
    }
    
    /* Wait for GPU idle */
    ret = mgpu_core_wait_idle(mdev, 100);
    if (ret) {
        dev_warn(mdev->dev, "GPU busy, queuing render job\n");
        /* Could implement a job queue here */
    }
    
    /* Configure vertex fetch unit (matches vertex_fetch.sv) */
    mgpu_write(mdev, MGPU_REG_VERTEX_BASE, vbo->dma_addr);
    mgpu_write(mdev, MGPU_REG_VERTEX_COUNT, job->vertex_count);
    mgpu_write(mdev, MGPU_REG_VERTEX_STRIDE, job->vertex_stride ?: 44);
    
    /* Configure shaders if specified */
    if (job->vertex_shader_handle) {
        /* The hardware expects shader code in instruction memory */
        mgpu_write(mdev, MGPU_REG_SHADER_PC, job->vertex_shader_pc);
    }
    
    /* Start rendering pipeline (matches controller.sv logic) */
    mgpu_write(mdev, MGPU_REG_CONTROL, 
               mgpu_read(mdev, MGPU_REG_CONTROL) | MGPU_CTRL_ENABLE);
    
    /* If synchronous, wait for completion */
    if (job->flags & MGPU_RENDER_FLAGS_SYNC) {
        int timeout = 1000; /* 1 second */
        
        while (timeout--) {
            status = mgpu_read(mdev, MGPU_REG_STATUS);
            if (status & MGPU_STATUS_IDLE)
                break;
            if (status & MGPU_STATUS_ERROR) {
                dev_err(mdev->dev, "Render error detected\n");
                ret = -EIO;
                break;
            }
            msleep(1);
        }
        
        if (timeout <= 0) {
            dev_err(mdev->dev, "Render timeout\n");
            ret = -ETIMEDOUT;
        }
    }
    
    mgpu_bo_put(vbo);
    
    return ret;
}

/* Query GPU capabilities (based on actual hardware) */
int mgpu_query_caps(struct mgpu_device *mdev, struct mgpu_caps_query *query)
{
    u32 caps = mgpu_read(mdev, MGPU_REG_CAPS);
    u32 version = mgpu_read(mdev, MGPU_REG_VERSION);
    
    /* Fill in capability structure based on hardware */
    query->version_major = MGPU_VERSION_MAJOR(version);
    query->version_minor = MGPU_VERSION_MINOR(version);
    query->version_patch = MGPU_VERSION_PATCH(version);
    query->version_build = MGPU_VERSION_BUILD(version);
    
    /* Capabilities from hardware */
    query->has_vertex_shader = !!(caps & MGPU_CAP_VERTEX_SHADER);
    query->has_fragment_shader = !!(caps & MGPU_CAP_FRAGMENT_SHADER);
    query->has_texture = !!(caps & MGPU_CAP_TEXTURE);
    query->has_float16 = !!(caps & MGPU_CAP_FLOAT16);
    query->has_float32 = !!(caps & MGPU_CAP_FLOAT32);
    query->has_int32 = !!(caps & MGPU_CAP_INT32);
    query->has_atomic = !!(caps & MGPU_CAP_ATOMIC);
    query->has_fence = !!(caps & MGPU_CAP_FENCE);
    query->has_multi_queue = !!(caps & MGPU_CAP_MULTI_QUEUE);
    query->has_preemption = !!(caps & MGPU_CAP_PREEMPTION);
    
    /* Fixed limits from hardware implementation */
    query->max_texture_size = 256;  /* From texture_unit.sv */
    query->max_vertex_count = 65536;
    query->max_shader_size = MGPU_REG_INSTR_MEM_SIZE;
    query->num_shader_cores = 1;  /* Single shader_core.sv */
    query->num_texture_units = 1;  /* Single texture_unit.sv */
    query->num_raster_units = 1;  /* Single rasterizer.sv */
    
    /* Queue configuration */
    if (caps & MGPU_CAP_MULTI_QUEUE) {
        query->num_queues = MGPU_MAX_QUEUES;
    } else {
        query->num_queues = 1;
    }
    
    /* Memory configuration */
    query->instruction_mem_size = MGPU_REG_INSTR_MEM_SIZE;
    query->max_bo_size = 256 * 1024 * 1024;  /* 256MB max per BO */
    
    /* Display configuration from hardware */
    query->display_width = 640;   /* From framebuffer.sv */
    query->display_height = 480;
    query->display_formats = DRM_FORMAT_XRGB8888;  /* 32-bit color */
    
    return 0;
}

/* Performance counter management */
int mgpu_perf_counter_enable(struct mgpu_device *mdev, u32 counter_mask)
{
    u32 control;
    
    /* Enable performance counters in hardware */
    control = mgpu_read(mdev, MGPU_REG_CONTROL);
    control |= MGPU_CTRL_PERF_COUNTER;
    mgpu_write(mdev, MGPU_REG_CONTROL, control);
    
    /* Enable performance counter interrupt */
    mgpu_write(mdev, MGPU_REG_IRQ_ENABLE,
               mgpu_read(mdev, MGPU_REG_IRQ_ENABLE) | MGPU_IRQ_PERF_COUNTER);
    
    /* Note: The hardware only has control bits, not actual counter registers */
    dev_info(mdev->dev, "Performance counters enabled (mask: 0x%08x)\n", counter_mask);
    
    return 0;
}

int mgpu_perf_counter_disable(struct mgpu_device *mdev)
{
    u32 control;
    
    /* Disable performance counters */
    control = mgpu_read(mdev, MGPU_REG_CONTROL);
    control &= ~MGPU_CTRL_PERF_COUNTER;
    mgpu_write(mdev, MGPU_REG_CONTROL, control);
    
    /* Disable interrupt */
    mgpu_write(mdev, MGPU_REG_IRQ_ENABLE,
               mgpu_read(mdev, MGPU_REG_IRQ_ENABLE) & ~MGPU_IRQ_PERF_COUNTER);
    
    return 0;
}

/* Sync object creation (fence-based) */
int mgpu_create_sync_object(struct mgpu_device *mdev, struct mgpu_sync_create *args)
{
    struct mgpu_bo *fence_bo;
    struct mgpu_bo_create bo_args = {0};
    int ret;
    
    /* Create a small BO for fence storage */
    bo_args.size = PAGE_SIZE;  /* One page for fences */
    bo_args.flags = MGPU_BO_FLAGS_COHERENT;  /* Must be coherent for CPU/GPU sync */
    
    ret = mgpu_bo_create(mdev, &bo_args);
    if (ret)
        return ret;
    
    /* Return handle to userspace */
    args->handle = bo_args.handle;
    args->gpu_addr = bo_args.gpu_addr;
    
    /* Initialize fence memory to 0 */
    fence_bo = mgpu_bo_lookup(mdev, bo_args.handle);
    if (fence_bo) {
        void *vaddr = mgpu_bo_vmap(fence_bo);
        if (vaddr) {
            memset(vaddr, 0, PAGE_SIZE);
            mgpu_bo_vunmap(fence_bo, vaddr);
        }
        mgpu_bo_put(fence_bo);
    }
    
    return 0;
}

/* Pipeline state management */
int mgpu_set_pipeline_state(struct mgpu_device *mdev, 
                            struct mgpu_pipeline_state *state)
{
    /* Validate state */
    if (state->vertex_shader_slot >= 16 ||
        state->fragment_shader_slot >= 16) {
        dev_err(mdev->dev, "Invalid shader slot\n");
        return -EINVAL;
    }
    
    /* Bind shaders */
    if (state->vertex_shader_slot < 16) {
        mgpu_shader_bind(mdev, state->vertex_shader_slot, MGPU_SHADER_VERTEX);
    }
    
    if (state->fragment_shader_slot < 16) {
        mgpu_shader_bind(mdev, state->fragment_shader_slot, MGPU_SHADER_FRAGMENT);
    }
    
    /* Configure rasterizer state */
    /* Note: The hardware rasterizer.sv is fixed-function, limited config */
    
    /* Configure blend state */
    /* Note: No blend hardware in current implementation */
    
    return 0;
}

/* Memory barrier for cache coherency */
void mgpu_memory_barrier(struct mgpu_device *mdev, u32 flags)
{
    u32 control;
    
    /* Issue cache flush if requested */
    if (flags & MGPU_BARRIER_CACHE_FLUSH) {
        control = mgpu_read(mdev, MGPU_REG_CONTROL);
        control |= MGPU_CTRL_FLUSH_CACHE;
        mgpu_write(mdev, MGPU_REG_CONTROL, control);
        
        /* Wait for flush to complete */
        udelay(10);
        
        /* Clear flush bit */
        control &= ~MGPU_CTRL_FLUSH_CACHE;
        mgpu_write(mdev, MGPU_REG_CONTROL, control);
    }
    
    /* Memory barrier */
    mb();
}

/* Debug marker insertion */
int mgpu_insert_debug_marker(struct mgpu_device *mdev, const char *marker)
{
    /* Write marker to scratch register for debugging */
    u32 hash = 0;
    int i;
    
    /* Simple hash of marker string */
    for (i = 0; marker[i] && i < 64; i++) {
        hash = hash * 31 + marker[i];
    }
    
    mgpu_write(mdev, MGPU_REG_SCRATCH, hash);
    
    dev_dbg(mdev->dev, "Debug marker: %s (0x%08x)\n", marker, hash);
    
    return 0;
}

MODULE_DESCRIPTION("MGPU User API Implementation");
MODULE_AUTHOR("Rafeed Khan");
MODULE_LICENSE("GPL v2");