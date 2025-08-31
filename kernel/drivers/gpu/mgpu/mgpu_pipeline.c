/*
 * MGPU Pipeline State Management
 * Manages the GPU rendering pipeline based on GPU implementation
 *
 * Pipeline stages (from gpu_top.sv):
 * 1. Vertex Fetch (vertex_fetch.sv)
 * 2. Vertex Shader (shader_core.sv) 
 * 3. Rasterization (rasterizer.sv)
 * 4. Fragment Shader (fragment_shader.sv)
 * 5. Framebuffer (framebuffer.sv)
 *
 * Copyright (C) 2025
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/completion.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>

#include "mgpu_drm.h"
#include "mgpu_regs.h"
#include "mgpu_internal.h"

/* Pipeline states from gpu_top.sv */
enum mgpu_pipeline_state {
    PIPE_IDLE = 0,
    PIPE_FETCH_VERTEX,
    PIPE_EXECUTE_SHADER,
    PIPE_RASTERIZE,
    PIPE_FRAGMENT,
    PIPE_DONE
};

/* Pipeline stage configuration */
struct mgpu_pipeline_stage {
    const char *name;
    u32 status_bit;
    u32 control_bit;
    bool enabled;
    u64 processed_items;
    u64 stall_cycles;
    ktime_t last_active;
};

/* Vertex fetch state (from vertex_fetch.sv) */
struct mgpu_vertex_fetch_state {
    u32 base_addr;      /* Base address of vertex buffer */
    u32 vertex_count;   /* Number of vertices to fetch */
    u32 vertex_stride;  /* Bytes per vertex (default 44 for 11 attrs * 4 bytes) */
    u32 current_vertex; /* Current vertex being fetched */
    bool fetch_active;
};

/* Rasterizer state (from rasterizer.sv) */
struct mgpu_rasterizer_state {
    /* Triangle vertices */
    s32 v0_x, v0_y;
    s32 v1_x, v1_y;
    s32 v2_x, v2_y;
    
    /* Bounding box */
    s32 bbox_min_x, bbox_min_y;
    s32 bbox_max_x, bbox_max_y;
    
    /* Current scan position */
    s32 current_x, current_y;
    bool raster_active;
    
    /* Statistics */
    u32 triangles_processed;
    u32 fragments_generated;
};

/* Fragment shader state (from fragment_shader.sv) */
struct mgpu_fragment_state {
    bool texture_enabled;
    u32 texture_base;
    u32 fragments_processed;
    u32 pixels_written;
};

/* Shader execution state */
struct mgpu_shader_exec_state {
    u32 pc;             /* Program counter */
    u32 slot;           /* Active shader slot */
    u32 type;           /* Shader type (vertex/fragment/compute) */
    bool halted;
    u32 cycles_executed;
};

/* Pipeline configuration */
struct mgpu_pipeline_config {
    /* Render state */
    struct {
        u32 vertex_shader_slot;
        u32 fragment_shader_slot;
        u32 vertex_format;
        u32 primitive_type;
    } shaders;
    
    /* Rasterizer configuration */
    struct {
        bool cull_enable;
        u32 cull_mode;      /* Front/back/none */
        bool depth_test;
        bool depth_write;
        u32 depth_func;
    } raster;
    
    /* Fragment configuration */
    struct {
        bool alpha_blend;
        u32 blend_src;
        u32 blend_dst;
        bool texture_enable;
        u32 texture_slot;
    } fragment;
    
    /* Framebuffer configuration */
    struct {
        u32 width;          /* 640 from framebuffer.sv */
        u32 height;         /* 480 from framebuffer.sv */
        u32 format;         /* Color format */
        u32 base_addr;      /* Framebuffer base address */
    } framebuffer;
};

/* Main pipeline manager structure */
struct mgpu_pipeline_mgr {
    struct mgpu_device *mdev;
    
    /* Pipeline state */
    enum mgpu_pipeline_state state;
    struct mutex state_lock;
    
    /* Stage states */
    struct mgpu_vertex_fetch_state vertex_fetch;
    struct mgpu_rasterizer_state rasterizer;
    struct mgpu_fragment_state fragment;
    struct mgpu_shader_exec_state vertex_shader;
    struct mgpu_shader_exec_state fragment_shader;
    
    /* Pipeline configuration */
    struct mgpu_pipeline_config config;
    
    /* Stage information */
    struct mgpu_pipeline_stage stages[5];
    
    /* Synchronization */
    struct completion pipeline_complete;
    wait_queue_head_t stage_wait;
    
    /* Work for async pipeline operations */
    struct work_struct pipeline_work;
    struct delayed_work monitor_work;
    
    /* Statistics */
    u64 frames_rendered;
    u64 total_vertices;
    u64 total_primitives;
    u64 total_fragments;
    u64 total_pixels;
    ktime_t frame_start_time;
    ktime_t frame_end_time;
    
    /* Error tracking */
    u32 pipeline_errors;
    u32 last_error_stage;
    
    /* Performance */
    bool profiling_enabled;
    u32 perf_counters[16];
};

/* Forward declarations */
static int mgpu_pipeline_execute_stage(struct mgpu_pipeline_mgr *mgr, 
                                       enum mgpu_pipeline_state stage);
static void mgpu_pipeline_work_handler(struct work_struct *work);
static void mgpu_pipeline_monitor_work(struct work_struct *work);

/* Initialize pipeline stages */
static void mgpu_pipeline_init_stages(struct mgpu_pipeline_mgr *mgr)
{
    /* Initialize stage descriptors */
    mgr->stages[0] = (struct mgpu_pipeline_stage){
        .name = "Vertex Fetch",
        .status_bit = MGPU_STATUS_BUSY,
        .control_bit = MGPU_CTRL_ENABLE,
        .enabled = true,
    };
    
    mgr->stages[1] = (struct mgpu_pipeline_stage){
        .name = "Vertex Shader",
        .status_bit = MGPU_STATUS_BUSY,
        .control_bit = MGPU_CTRL_ENABLE,
        .enabled = true,
    };
    
    mgr->stages[2] = (struct mgpu_pipeline_stage){
        .name = "Rasterizer",
        .status_bit = MGPU_STATUS_BUSY,
        .control_bit = MGPU_CTRL_ENABLE,
        .enabled = true,
    };
    
    mgr->stages[3] = (struct mgpu_pipeline_stage){
        .name = "Fragment Shader",
        .status_bit = MGPU_STATUS_BUSY,
        .control_bit = MGPU_CTRL_ENABLE,
        .enabled = true,
    };
    
    mgr->stages[4] = (struct mgpu_pipeline_stage){
        .name = "Framebuffer",
        .status_bit = MGPU_STATUS_BUSY,
        .control_bit = MGPU_CTRL_ENABLE,
        .enabled = true,
    };
}

/* Configure vertex fetch stage */
static int mgpu_pipeline_config_vertex_fetch(struct mgpu_pipeline_mgr *mgr,
                                            u32 base_addr, u32 vertex_count,
                                            u32 vertex_stride)
{
    struct mgpu_device *mdev = mgr->mdev;
    
    /* Validate parameters */
    if (!base_addr || !vertex_count) {
        dev_err(mdev->dev, "Invalid vertex fetch parameters\n");
        return -EINVAL;
    }
    
    /* Default stride from vertex_fetch.sv: 11 attributes * 4 bytes = 44 */
    if (!vertex_stride)
        vertex_stride = 44;
    
    /* Update state */
    mgr->vertex_fetch.base_addr = base_addr;
    mgr->vertex_fetch.vertex_count = vertex_count;
    mgr->vertex_fetch.vertex_stride = vertex_stride;
    mgr->vertex_fetch.current_vertex = 0;
    
    /* Program hardware registers */
    mgpu_write(mdev, MGPU_REG_VERTEX_BASE, base_addr);
    mgpu_write(mdev, MGPU_REG_VERTEX_COUNT, vertex_count);
    mgpu_write(mdev, MGPU_REG_VERTEX_STRIDE, vertex_stride);
    
    dev_dbg(mdev->dev, "Configured vertex fetch: base=0x%08x, count=%u, stride=%u\n",
            base_addr, vertex_count, vertex_stride);
    
    return 0;
}

/* Configure rasterizer */
static int mgpu_pipeline_config_rasterizer(struct mgpu_pipeline_mgr *mgr,
                                          bool cull_enable, u32 cull_mode)
{
    struct mgpu_device *mdev = mgr->mdev;
    
    mgr->config.raster.cull_enable = cull_enable;
    mgr->config.raster.cull_mode = cull_mode;
    
    /* Note: Current hardware rasterizer.sv is fixed-function with no config regs */
    dev_dbg(mdev->dev, "Rasterizer config: cull=%d, mode=%u\n", 
            cull_enable, cull_mode);
    
    return 0;
}

/* Configure fragment stage */
static int mgpu_pipeline_config_fragment(struct mgpu_pipeline_mgr *mgr,
                                        bool texture_enable, u32 texture_slot)
{
    struct mgpu_device *mdev = mgr->mdev;
    
    mgr->config.fragment.texture_enable = texture_enable;
    mgr->config.fragment.texture_slot = texture_slot;
    mgr->fragment.texture_enabled = texture_enable;
    
    /* Texture unit has fixed 256x256 size from texture_unit.sv */
    if (texture_enable) {
        dev_dbg(mdev->dev, "Fragment config: texture enabled, slot=%u\n",
                texture_slot);
    }
    
    return 0;
}

/* Configure framebuffer */
static int mgpu_pipeline_config_framebuffer(struct mgpu_pipeline_mgr *mgr,
                                           u32 base_addr)
{
    struct mgpu_device *mdev = mgr->mdev;
    
    /* Framebuffer dimensions are fixed in hardware: 640x480 */
    mgr->config.framebuffer.width = 640;
    mgr->config.framebuffer.height = 480;
    mgr->config.framebuffer.format = 0x8888; /* XRGB8888 */
    mgr->config.framebuffer.base_addr = base_addr;
    
    /* Note: Hardware framebuffer.sv doesn't have a base addr register,
     * it writes directly through the memory interconnect */
    
    dev_dbg(mdev->dev, "Framebuffer config: %ux%u at 0x%08x\n",
            mgr->config.framebuffer.width,
            mgr->config.framebuffer.height,
            base_addr);
    
    return 0;
}

/* Execute vertex fetch stage */
static int mgpu_pipeline_fetch_vertices(struct mgpu_pipeline_mgr *mgr)
{
    struct mgpu_device *mdev = mgr->mdev;
    u32 status;
    int timeout = 100;
    
    if (!mgr->vertex_fetch.vertex_count)
        return 0;
    
    mgr->vertex_fetch.fetch_active = true;
    
    /* Trigger vertex fetch by starting pipeline */
    status = mgpu_read(mdev, MGPU_REG_CONTROL);
    mgpu_write(mdev, MGPU_REG_CONTROL, status | MGPU_CTRL_ENABLE);
    
    /* Wait for fetch to complete */
    while (timeout-- > 0) {
        status = mgpu_read(mdev, MGPU_REG_STATUS);
        if (!(status & MGPU_STATUS_BUSY))
            break;
        udelay(10);
    }
    
    mgr->vertex_fetch.fetch_active = false;
    mgr->vertex_fetch.current_vertex = mgr->vertex_fetch.vertex_count;
    
    /* Update statistics */
    mgr->total_vertices += mgr->vertex_fetch.vertex_count;
    mgr->stages[0].processed_items += mgr->vertex_fetch.vertex_count;
    
    return (timeout > 0) ? 0 : -ETIMEDOUT;
}

/* Execute shader stage */
static int mgpu_pipeline_execute_shader(struct mgpu_pipeline_mgr *mgr,
                                       struct mgpu_shader_exec_state *shader)
{
    struct mgpu_device *mdev = mgr->mdev;
    u32 pc_offset;
    
    /* Calculate PC offset for shader slot (256 dwords per slot) */
    pc_offset = shader->slot * 256;
    
    /* Set shader PC */
    mgpu_write(mdev, MGPU_REG_SHADER_PC, pc_offset);
    
    /* Execute shader (simplified - real implementation would step through) */
    shader->cycles_executed++;
    
    /* Check for halt condition */
    if (mgpu_read(mdev, MGPU_REG_STATUS) & MGPU_STATUS_HALTED) {
        shader->halted = true;
        return -EIO;
    }
    
    mgr->stages[1].processed_items++;
    
    return 0;
}

/* Execute rasterization stage */
static int mgpu_pipeline_rasterize(struct mgpu_pipeline_mgr *mgr)
{
    struct mgpu_rasterizer_state *rast = &mgr->rasterizer;
    struct mgpu_device *mdev = mgr->mdev;
    u32 triangles_per_batch = mgr->vertex_fetch.vertex_count / 3;
    
    rast->raster_active = true;
    
    /* Process triangles */
    rast->triangles_processed += triangles_per_batch;
    
    /* Estimate fragments (simplified) */
    rast->fragments_generated += triangles_per_batch * 100; /* Rough estimate */
    
    /* Update statistics */
    mgr->total_primitives += triangles_per_batch;
    mgr->total_fragments += rast->fragments_generated;
    mgr->stages[2].processed_items += triangles_per_batch;
    
    rast->raster_active = false;
    
    return 0;
}

/* Execute fragment stage */
static int mgpu_pipeline_process_fragments(struct mgpu_pipeline_mgr *mgr)
{
    struct mgpu_fragment_state *frag = &mgr->fragment;
    
    /* Process fragments from rasterizer */
    frag->fragments_processed = mgr->rasterizer.fragments_generated;
    frag->pixels_written = frag->fragments_processed; /* 1:1 for now */
    
    /* Update statistics */
    mgr->total_pixels += frag->pixels_written;
    mgr->stages[3].processed_items += frag->fragments_processed;
    mgr->stages[4].processed_items += frag->pixels_written;
    
    return 0;
}

/* Execute a pipeline stage */
static int mgpu_pipeline_execute_stage(struct mgpu_pipeline_mgr *mgr,
                                       enum mgpu_pipeline_state stage)
{
    struct mgpu_device *mdev = mgr->mdev;
    int ret = 0;
    
    dev_dbg(mdev->dev, "Executing pipeline stage: %d\n", stage);
    
    switch (stage) {
    case PIPE_FETCH_VERTEX:
        ret = mgpu_pipeline_fetch_vertices(mgr);
        break;
        
    case PIPE_EXECUTE_SHADER:
        ret = mgpu_pipeline_execute_shader(mgr, &mgr->vertex_shader);
        if (!ret && mgr->config.shaders.fragment_shader_slot < 16) {
            ret = mgpu_pipeline_execute_shader(mgr, &mgr->fragment_shader);
        }
        break;
        
    case PIPE_RASTERIZE:
        ret = mgpu_pipeline_rasterize(mgr);
        break;
        
    case PIPE_FRAGMENT:
        ret = mgpu_pipeline_process_fragments(mgr);
        break;
        
    case PIPE_DONE:
        complete(&mgr->pipeline_complete);
        break;
        
    default:
        break;
    }
    
    if (ret) {
        dev_err(mdev->dev, "Pipeline stage %d failed: %d\n", stage, ret);
        mgr->pipeline_errors++;
        mgr->last_error_stage = stage;
    }
    
    return ret;
}

/* Main pipeline execution */
int mgpu_pipeline_execute(struct mgpu_pipeline_mgr *mgr)
{
    struct mgpu_device *mdev = mgr->mdev;
    enum mgpu_pipeline_state next_state;
    int ret = 0;
    
    mutex_lock(&mgr->state_lock);
    
    if (mgr->state != PIPE_IDLE) {
        dev_warn(mdev->dev, "Pipeline already running\n");
        mutex_unlock(&mgr->state_lock);
        return -EBUSY;
    }
    
    mgr->frame_start_time = ktime_get();
    mgr->state = PIPE_FETCH_VERTEX;
    
    /* Execute pipeline stages in sequence */
    while (mgr->state != PIPE_IDLE && mgr->state != PIPE_DONE) {
        ret = mgpu_pipeline_execute_stage(mgr, mgr->state);
        if (ret)
            break;
        
        /* Advance to next stage */
        switch (mgr->state) {
        case PIPE_FETCH_VERTEX:
            next_state = PIPE_EXECUTE_SHADER;
            break;
        case PIPE_EXECUTE_SHADER:
            next_state = PIPE_RASTERIZE;
            break;
        case PIPE_RASTERIZE:
            next_state = PIPE_FRAGMENT;
            break;
        case PIPE_FRAGMENT:
            next_state = PIPE_DONE;
            break;
        default:
            next_state = PIPE_IDLE;
            break;
        }
        
        mgr->state = next_state;
        
        /* Allow other tasks to run */
        if (mgr->state != PIPE_DONE) {
            mutex_unlock(&mgr->state_lock);
            cond_resched();
            mutex_lock(&mgr->state_lock);
        }
    }
    
    mgr->frame_end_time = ktime_get();
    mgr->frames_rendered++;
    mgr->state = PIPE_IDLE;
    
    mutex_unlock(&mgr->state_lock);
    
    if (!ret) {
        u64 frame_time_ns = ktime_to_ns(ktime_sub(mgr->frame_end_time,
                                                  mgr->frame_start_time));
        dev_dbg(mdev->dev, "Frame %llu completed in %llu ns\n",
                mgr->frames_rendered, frame_time_ns);
    }
    
    return ret;
}

/* Pipeline work handler for async execution */
static void mgpu_pipeline_work_handler(struct work_struct *work)
{
    struct mgpu_pipeline_mgr *mgr = container_of(work,
                                                struct mgpu_pipeline_mgr,
                                                pipeline_work);
    mgpu_pipeline_execute(mgr);
}

/* Pipeline monitor work */
static void mgpu_pipeline_monitor_work(struct work_struct *work)
{
    struct mgpu_pipeline_mgr *mgr = container_of(work,
                                                struct mgpu_pipeline_mgr,
                                                monitor_work.work);
    struct mgpu_device *mdev = mgr->mdev;
    u32 status;
    
    /* Check pipeline health */
    status = mgpu_read(mdev, MGPU_REG_STATUS);
    
    if (status & MGPU_STATUS_ERROR) {
        dev_err(mdev->dev, "Pipeline error detected: 0x%08x\n", status);
        mgr->pipeline_errors++;
    }
    
    if (status & MGPU_STATUS_HALTED) {
        dev_err(mdev->dev, "Pipeline halted\n");
        /* Trigger recovery */
        mgpu_reset_schedule(mdev);
    }
    
    /* Log statistics periodically */
    if (mgr->frames_rendered % 100 == 0 && mgr->frames_rendered > 0) {
        dev_info(mdev->dev, "Pipeline stats: %llu frames, %llu vertices, "
                 "%llu fragments, %llu pixels\n",
                 mgr->frames_rendered, mgr->total_vertices,
                 mgr->total_fragments, mgr->total_pixels);
    }
    
    /* Reschedule monitor */
    if (mgr->state != PIPE_IDLE) {
        schedule_delayed_work(&mgr->monitor_work, HZ / 10); /* 100ms */
    }
}

/* Flush pipeline */
int mgpu_pipeline_flush(struct mgpu_pipeline_mgr *mgr)
{
    struct mgpu_device *mdev = mgr->mdev;
    u32 control;
    int timeout = 1000;
    
    dev_dbg(mdev->dev, "Flushing pipeline\n");
    
    /* Set flush bit */
    control = mgpu_read(mdev, MGPU_REG_CONTROL);
    mgpu_write(mdev, MGPU_REG_CONTROL, control | MGPU_CTRL_FLUSH_CACHE);
    
    /* Wait for flush to complete */
    while (timeout-- > 0) {
        if (mgpu_read(mdev, MGPU_REG_STATUS) & MGPU_STATUS_IDLE)
            break;
        udelay(10);
    }
    
    /* Clear flush bit */
    mgpu_write(mdev, MGPU_REG_CONTROL, control);
    
    return (timeout > 0) ? 0 : -ETIMEDOUT;
}

/* Stall pipeline */
int mgpu_pipeline_stall(struct mgpu_pipeline_mgr *mgr)
{
    struct mgpu_device *mdev = mgr->mdev;
    u32 control;
    
    dev_dbg(mdev->dev, "Stalling pipeline\n");
    
    /* Set pause bit */
    control = mgpu_read(mdev, MGPU_REG_CONTROL);
    mgpu_write(mdev, MGPU_REG_CONTROL, control | MGPU_CTRL_PAUSE);
    
    return 0;
}

/* Resume pipeline */
int mgpu_pipeline_resume(struct mgpu_pipeline_mgr *mgr)
{
    struct mgpu_device *mdev = mgr->mdev;
    u32 control;
    
    dev_dbg(mdev->dev, "Resuming pipeline\n");
    
    /* Clear pause bit */
    control = mgpu_read(mdev, MGPU_REG_CONTROL);
    mgpu_write(mdev, MGPU_REG_CONTROL, control & ~MGPU_CTRL_PAUSE);
    
    /* Wake up any waiting stages */
    wake_up_all(&mgr->stage_wait);
    
    return 0;
}

/* Get pipeline statistics */
int mgpu_pipeline_get_stats(struct mgpu_pipeline_mgr *mgr,
                           struct mgpu_pipeline_stats *stats)
{
    int i;
    
    if (!stats)
        return -EINVAL;
    
    mutex_lock(&mgr->state_lock);
    
    stats->frames_rendered = mgr->frames_rendered;
    stats->total_vertices = mgr->total_vertices;
    stats->total_primitives = mgr->total_primitives;
    stats->total_fragments = mgr->total_fragments;
    stats->total_pixels = mgr->total_pixels;
    stats->pipeline_errors = mgr->pipeline_errors;
    
    /* Per-stage statistics */
    for (i = 0; i < 5; i++) {
        stats->stage_stats[i].name = mgr->stages[i].name;
        stats->stage_stats[i].processed_items = mgr->stages[i].processed_items;
        stats->stage_stats[i].stall_cycles = mgr->stages[i].stall_cycles;
        stats->stage_stats[i].enabled = mgr->stages[i].enabled;
    }
    
    mutex_unlock(&mgr->state_lock);
    
    return 0;
}

/* Reset pipeline statistics */
void mgpu_pipeline_reset_stats(struct mgpu_pipeline_mgr *mgr)
{
    int i;
    
    mutex_lock(&mgr->state_lock);
    
    mgr->frames_rendered = 0;
    mgr->total_vertices = 0;
    mgr->total_primitives = 0;
    mgr->total_fragments = 0;
    mgr->total_pixels = 0;
    mgr->pipeline_errors = 0;
    
    for (i = 0; i < 5; i++) {
        mgr->stages[i].processed_items = 0;
        mgr->stages[i].stall_cycles = 0;
    }
    
    mutex_unlock(&mgr->state_lock);
}

/* Submit draw call */
int mgpu_pipeline_draw(struct mgpu_device *mdev, struct mgpu_draw_call *draw)
{
    struct mgpu_pipeline_mgr *mgr = mdev->pipeline_mgr;
    int ret;
    
    if (!mgr)
        return -ENODEV;
    
    /* Configure pipeline for draw */
    ret = mgpu_pipeline_config_vertex_fetch(mgr, draw->vertex_buffer,
                                           draw->vertex_count,
                                           draw->vertex_stride);
    if (ret)
        return ret;
    
    /* Set shader slots */
    mgr->vertex_shader.slot = draw->vertex_shader_slot;
    mgr->vertex_shader.type = MGPU_SHADER_VERTEX;
    mgr->fragment_shader.slot = draw->fragment_shader_slot;
    mgr->fragment_shader.type = MGPU_SHADER_FRAGMENT;
    
    /* Configure other stages */
    mgpu_pipeline_config_rasterizer(mgr, draw->cull_enable, draw->cull_mode);
    mgpu_pipeline_config_fragment(mgr, draw->texture_enable, draw->texture_slot);
    mgpu_pipeline_config_framebuffer(mgr, draw->framebuffer_addr);
    
    /* Execute pipeline */
    if (draw->flags & MGPU_DRAW_ASYNC) {
        /* Async execution */
        schedule_work(&mgr->pipeline_work);
        return 0;
    } else {
        /* Synchronous execution */
        return mgpu_pipeline_execute(mgr);
    }
}

/* Initialize pipeline manager */
int mgpu_pipeline_init(struct mgpu_device *mdev)
{
    struct mgpu_pipeline_mgr *mgr;
    
    mgr = kzalloc(sizeof(*mgr), GFP_KERNEL);
    if (!mgr)
        return -ENOMEM;
    
    mgr->mdev = mdev;
    mgr->state = PIPE_IDLE;
    
    mutex_init(&mgr->state_lock);
    init_completion(&mgr->pipeline_complete);
    init_waitqueue_head(&mgr->stage_wait);
    
    /* Initialize work queues */
    INIT_WORK(&mgr->pipeline_work, mgpu_pipeline_work_handler);
    INIT_DELAYED_WORK(&mgr->monitor_work, mgpu_pipeline_monitor_work);
    
    /* Initialize stages */
    mgpu_pipeline_init_stages(mgr);
    
    /* Set default configuration */
    mgr->config.framebuffer.width = 640;
    mgr->config.framebuffer.height = 480;
    
    mdev->pipeline_mgr = mgr;
    
    dev_info(mdev->dev, "Pipeline manager initialized\n");
    
    return 0;
}

/* Cleanup pipeline manager */
void mgpu_pipeline_fini(struct mgpu_device *mdev)
{
    struct mgpu_pipeline_mgr *mgr = mdev->pipeline_mgr;
    
    if (!mgr)
        return;
    
    /* Cancel work */
    cancel_work_sync(&mgr->pipeline_work);
    cancel_delayed_work_sync(&mgr->monitor_work);
    
    /* Wait for pipeline to idle */
    if (mgr->state != PIPE_IDLE) {
        mgpu_pipeline_flush(mgr);
    }
    
    kfree(mgr);
    mdev->pipeline_mgr = NULL;
    
    dev_info(mdev->dev, "Pipeline manager shutdown\n");
}

MODULE_DESCRIPTION("MGPU Pipeline State Management");
MODULE_AUTHOR("Your Name");
MODULE_LICENSE("GPL v2");