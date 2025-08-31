#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/component.h>
#include <linux/of_graph.h>
#include <linux/dma-buf.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_of.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_vblank.h>
#include <drm/drm_panel.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_file.h>

#include "mgpu_drm.h"
#include "mgpu_regs.h"
#include "mgpu_internal.h"

/* DRM driver info */
#define DRIVER_NAME    "mgpu"
#define DRIVER_DESC    "DRM driver for FPGA GPU"
#define DRIVER_DATE    "20241220"
#define DRIVER_MAJOR   1
#define DRIVER_MINOR   0

/* Fixed framebuffer size from hardware (framebuffer.sv) */
#define MGPU_FB_WIDTH  640
#define MGPU_FB_HEIGHT 480
#define MGPU_FB_BPP    32

/* DRM device structure */
struct mgpu_drm_device {
    struct drm_device drm;
    struct mgpu_device *mdev;  /* Core device */
    
    /* Display pipeline */
    struct drm_simple_display_pipe pipe;
    struct drm_connector connector;
    struct drm_panel *panel;
    struct drm_bridge *bridge;
    
    /* Framebuffer */
    void __iomem *fb_base;
    dma_addr_t fb_dma_addr;
    size_t fb_size;
    
    /* Mode */
    struct drm_display_mode mode;
    
    /* CRTC state */
    bool crtc_enabled;
    
    /* Render state */
    struct {
        u32 vertex_base;
        u32 vertex_count;
        u32 vertex_stride;
        u32 shader_pc;
    } render_state;
};

#define drm_to_mgpu(x) container_of(x, struct mgpu_drm_device, drm)

/* Get device from DRM file */
static struct mgpu_device *mgpu_get_device(struct drm_file *file)
{
    struct mgpu_drm_device *drm_dev = drm_to_mgpu(file->minor->dev);
    return drm_dev->mdev;
}

/* --- Framebuffer Functions --- */

static void mgpu_fb_dirty(struct drm_framebuffer *fb, struct drm_rect *rect)
{
    struct drm_gem_cma_object *cma_obj = drm_fb_cma_get_gem_obj(fb, 0);
    struct mgpu_drm_device *mgpu = drm_to_mgpu(fb->dev);
    struct mgpu_device *mdev = mgpu->mdev;
    void *src = cma_obj->vaddr;
    u32 cpp = fb->format->cpp[0];
    size_t len = drm_rect_width(rect) * cpp;
    unsigned int y;
    
    /* Copy damaged region to hardware framebuffer */
    for (y = rect->y1; y < rect->y2; y++) {
        void *dst = mgpu->fb_base + (y * MGPU_FB_WIDTH + rect->x1) * cpp;
        void *line_src = src + (y * fb->pitches[0] + rect->x1 * cpp);
        memcpy_toio(dst, line_src, len);
    }
}

/* --- Display Pipe Functions --- */

static void mgpu_pipe_enable(struct drm_simple_display_pipe *pipe,
                             struct drm_crtc_state *crtc_state,
                             struct drm_plane_state *plane_state)
{
    struct mgpu_drm_device *mgpu = drm_to_mgpu(pipe->crtc.dev);
    struct mgpu_device *mdev = mgpu->mdev;
    struct drm_framebuffer *fb = plane_state->fb;
    
    if (!fb)
        return;
    
    dev_info(mdev->dev, "Enabling display pipe\n");
    
    /* Configure framebuffer base in hardware */
    /* Note: The hardware expects vertex data, not a framebuffer address directly */
    /* For display-only mode, we'd need different hardware */
    
    mgpu->crtc_enabled = true;
    
    /* Start the GPU rendering pipeline */
    mgpu_write(mdev, MGPU_REG_CONTROL, MGPU_CTRL_ENABLE);
}

static void mgpu_pipe_disable(struct drm_simple_display_pipe *pipe)
{
    struct mgpu_drm_device *mgpu = drm_to_mgpu(pipe->crtc.dev);
    struct mgpu_device *mdev = mgpu->mdev;
    
    dev_info(mdev->dev, "Disabling display pipe\n");
    
    /* Stop GPU rendering */
    mgpu_write(mdev, MGPU_REG_CONTROL, 0);
    
    mgpu->crtc_enabled = false;
}

static void mgpu_pipe_update(struct drm_simple_display_pipe *pipe,
                             struct drm_plane_state *old_state)
{
    struct drm_plane_state *state = pipe->plane.state;
    struct drm_framebuffer *fb = state->fb;
    struct drm_rect rect;
    
    if (!fb)
        return;
    
    /* Calculate damage rect */
    if (drm_atomic_helper_damage_merged(old_state, state, &rect))
        mgpu_fb_dirty(fb, &rect);
}

static int mgpu_pipe_check(struct drm_simple_display_pipe *pipe,
                           struct drm_plane_state *plane_state,
                           struct drm_crtc_state *crtc_state)
{
    struct drm_framebuffer *fb = plane_state->fb;
    
    if (!fb)
        return 0;
    
    /* Only support our fixed resolution */
    if (fb->width != MGPU_FB_WIDTH || fb->height != MGPU_FB_HEIGHT) {
        DRM_ERROR("Invalid framebuffer size %dx%d (expected %dx%d)\n",
                  fb->width, fb->height, MGPU_FB_WIDTH, MGPU_FB_HEIGHT);
        return -EINVAL;
    }
    
    return 0;
}

static const struct drm_simple_display_pipe_funcs mgpu_pipe_funcs = {
    .enable = mgpu_pipe_enable,
    .disable = mgpu_pipe_disable,
    .update = mgpu_pipe_update,
    .check = mgpu_pipe_check,
};

/* --- Connector Functions --- */

static int mgpu_connector_get_modes(struct drm_connector *connector)
{
    struct mgpu_drm_device *mgpu = drm_to_mgpu(connector->dev);
    struct drm_display_mode *mode;
    
    /* Add our fixed mode */
    mode = drm_mode_duplicate(connector->dev, &mgpu->mode);
    if (!mode)
        return 0;
    
    mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
    drm_mode_probed_add(connector, mode);
    
    return 1;
}

static const struct drm_connector_helper_funcs mgpu_connector_helper_funcs = {
    .get_modes = mgpu_connector_get_modes,
};

static const struct drm_connector_funcs mgpu_connector_funcs = {
    .fill_modes = drm_helper_probe_single_connector_modes,
    .destroy = drm_connector_cleanup,
    .reset = drm_atomic_helper_connector_reset,
    .atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
    .atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

/* --- GEM/Command Submission IOCTLs --- */

static int mgpu_ioctl_submit_3d(struct drm_device *dev, void *data,
                                struct drm_file *file)
{
    struct mgpu_device *mdev = mgpu_get_device(file);
    struct drm_mgpu_submit_3d *args = data;
    struct mgpu_submit submit = {0};
    int ret;
    
    /* Validate arguments */
    if (!args->vertex_bo || !args->vertex_count) {
        DRM_ERROR("Invalid 3D submit parameters\n");
        return -EINVAL;
    }
    
    /* Look up vertex buffer object */
    struct mgpu_bo *vbo = mgpu_bo_lookup(mdev, args->vertex_bo);
    if (!vbo) {
        DRM_ERROR("Invalid vertex buffer handle\n");
        return -EINVAL;
    }
    
    /* Program vertex state registers */
    mgpu_write(mdev, MGPU_REG_VERTEX_BASE, vbo->dma_addr);
    mgpu_write(mdev, MGPU_REG_VERTEX_COUNT, args->vertex_count);
    mgpu_write(mdev, MGPU_REG_VERTEX_STRIDE, args->vertex_stride ?: 44); /* Default 11 attrs * 4 bytes */
    
    /* Bind shaders if specified */
    if (args->vertex_shader_slot < 16) {
        mgpu_shader_bind(mdev, args->vertex_shader_slot, MGPU_SHADER_VERTEX);
    }
    if (args->fragment_shader_slot < 16) {
        mgpu_shader_bind(mdev, args->fragment_shader_slot, MGPU_SHADER_FRAGMENT);
    }
    
    /* Build DRAW command */
    struct mgpu_cmd_draw draw_cmd = {
        .header = {
            .opcode = MGPU_CMD_DRAW,
            .size = sizeof(draw_cmd) / 4,
            .flags = 0,
        },
        .vertex_count = args->vertex_count,
        .instance_count = 1,
        .first_vertex = 0,
        .first_instance = 0,
    };
    
    /* Submit command through command queue */
    submit.commands = (uintptr_t)&draw_cmd;
    submit.cmd_size = sizeof(draw_cmd);
    submit.queue_id = 0;  /* Use queue 0 for 3D */
    submit.flags = args->flags;
    
    /* Add fence if requested */
    if (args->fence_bo) {
        struct mgpu_bo *fence_bo = mgpu_bo_lookup(mdev, args->fence_bo);
        if (!fence_bo) {
            mgpu_bo_put(vbo);
            return -EINVAL;
        }
        submit.fence_addr = fence_bo->dma_addr + args->fence_offset;
        submit.fence_value = args->fence_value;
        submit.flags |= MGPU_SUBMIT_FLAGS_FENCE;
        mgpu_bo_put(fence_bo);
    }
    
    ret = mgpu_submit_commands(mdev, &submit);
    
    mgpu_bo_put(vbo);
    
    return ret;
}

static int mgpu_ioctl_wait_bo(struct drm_device *dev, void *data,
                              struct drm_file *file)
{
    struct mgpu_device *mdev = mgpu_get_device(file);
    struct drm_mgpu_wait_bo *args = data;
    struct mgpu_wait_fence wait = {0};
    struct mgpu_bo *bo;
    int ret;
    
    /* Look up buffer object */
    bo = mgpu_bo_lookup(mdev, args->handle);
    if (!bo)
        return -EINVAL;
    
    /* Set up fence wait */
    wait.fence_addr = bo->dma_addr + args->offset;
    wait.fence_value = args->value;
    wait.timeout_ms = args->timeout_ms;
    
    ret = mgpu_wait_fence(mdev, &wait);
    
    mgpu_bo_put(bo);
    
    return ret;
}

static int mgpu_ioctl_gem_info(struct drm_device *dev, void *data,
                               struct drm_file *file)
{
    struct mgpu_device *mdev = mgpu_get_device(file);
    struct drm_mgpu_gem_info *args = data;
    struct mgpu_bo *bo;
    
    bo = mgpu_bo_lookup(mdev, args->handle);
    if (!bo)
        return -EINVAL;
    
    args->size = bo->size;
    args->gpu_addr = bo->dma_addr;
    args->flags = bo->flags;
    
    mgpu_bo_put(bo);
    
    return 0;
}

/* Extended IOCTLs for DRM mode */
static const struct drm_ioctl_desc mgpu_drm_ioctls[] = {
    DRM_IOCTL_DEF_DRV(MGPU_SUBMIT_3D, mgpu_ioctl_submit_3d, DRM_RENDER_ALLOW),
    DRM_IOCTL_DEF_DRV(MGPU_WAIT_BO, mgpu_ioctl_wait_bo, DRM_RENDER_ALLOW),
    DRM_IOCTL_DEF_DRV(MGPU_GEM_INFO, mgpu_ioctl_gem_info, DRM_RENDER_ALLOW),
};

/* --- File Operations --- */

static int mgpu_drm_open(struct drm_device *dev, struct drm_file *file)
{
    struct mgpu_drm_device *mgpu = drm_to_mgpu(dev);
    struct mgpu_device *mdev = mgpu->mdev;
    
    /* Initialize per-file state if needed */
    file->driver_priv = NULL;  /* Could store per-context data */
    
    dev_dbg(mdev->dev, "DRM file opened\n");
    
    return 0;
}

static void mgpu_drm_postclose(struct drm_device *dev, struct drm_file *file)
{
    struct mgpu_drm_device *mgpu = drm_to_mgpu(dev);
    struct mgpu_device *mdev = mgpu->mdev;
    
    /* Clean up any per-file resources */
    
    dev_dbg(mdev->dev, "DRM file closed\n");
}

/* --- DRM Driver Structure --- */

static struct drm_driver mgpu_drm_driver = {
    .driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC |
                      DRIVER_RENDER,
    
    /* Version info */
    .name = DRIVER_NAME,
    .desc = DRIVER_DESC,
    .date = DRIVER_DATE,
    .major = DRIVER_MAJOR,
    .minor = DRIVER_MINOR,
    
    /* File ops */
    .open = mgpu_drm_open,
    .postclose = mgpu_drm_postclose,
    
    /* IOCTLs */
    .ioctls = mgpu_drm_ioctls,
    .num_ioctls = ARRAY_SIZE(mgpu_drm_ioctls),
    
    /* GEM ops - use CMA helpers */
    DRM_GEM_CMA_DRIVER_OPS,
    
    /* Prime ops */
    .prime_handle_to_fd = drm_gem_prime_handle_to_fd,
    .prime_fd_to_handle = drm_gem_prime_fd_to_handle,
    .gem_prime_import_sg_table = drm_gem_cma_prime_import_sg_table,
    .gem_prime_mmap = drm_gem_prime_mmap,
    
    /* Misc */
    .fops = &mgpu_drm_fops,
};

/* --- Initialization --- */

static int mgpu_drm_bind(struct device *dev, struct device *master, void *data)
{
    struct platform_device *pdev = to_platform_device(dev);
    struct mgpu_device *mdev = platform_get_drvdata(pdev);
    struct mgpu_drm_device *mgpu;
    struct drm_device *drm;
    int ret;
    
    dev_info(dev, "Binding MGPU DRM\n");
    
    /* Allocate DRM device */
    mgpu = devm_drm_dev_alloc(dev, &mgpu_drm_driver,
                              struct mgpu_drm_device, drm);
    if (IS_ERR(mgpu))
        return PTR_ERR(mgpu);
    
    drm = &mgpu->drm;
    mgpu->mdev = mdev;
    
    /* Store DRM device in platform data */
    platform_set_drvdata(pdev, mgpu);
    
    /* Initialize fixed display mode (640x480 from hardware) */
    drm_mode_set_name(&mgpu->mode);
    mgpu->mode.hdisplay = MGPU_FB_WIDTH;
    mgpu->mode.vdisplay = MGPU_FB_HEIGHT;
    mgpu->mode.hsync_start = MGPU_FB_WIDTH + 16;
    mgpu->mode.hsync_end = MGPU_FB_WIDTH + 16 + 96;
    mgpu->mode.htotal = MGPU_FB_WIDTH + 16 + 96 + 48;
    mgpu->mode.vsync_start = MGPU_FB_HEIGHT + 10;
    mgpu->mode.vsync_end = MGPU_FB_HEIGHT + 10 + 2;
    mgpu->mode.vtotal = MGPU_FB_HEIGHT + 10 + 2 + 33;
    mgpu->mode.clock = 25175;  /* 25.175 MHz pixel clock */
    mgpu->mode.vrefresh = drm_mode_vrefresh(&mgpu->mode);
    
    /* Allocate framebuffer memory */
    mgpu->fb_size = MGPU_FB_WIDTH * MGPU_FB_HEIGHT * (MGPU_FB_BPP / 8);
    mgpu->fb_base = dma_alloc_coherent(dev, mgpu->fb_size,
                                       &mgpu->fb_dma_addr, GFP_KERNEL);
    if (!mgpu->fb_base) {
        dev_err(dev, "Failed to allocate framebuffer\n");
        return -ENOMEM;
    }
    
    /* Initialize vblank */
    ret = drm_vblank_init(drm, 1);
    if (ret) {
        dev_err(dev, "Failed to initialize vblank\n");
        goto err_fb;
    }
    
    /* Initialize mode config */
    drm_mode_config_init(drm);
    drm->mode_config.min_width = MGPU_FB_WIDTH;
    drm->mode_config.max_width = MGPU_FB_WIDTH;
    drm->mode_config.min_height = MGPU_FB_HEIGHT;
    drm->mode_config.max_height = MGPU_FB_HEIGHT;
    drm->mode_config.funcs = &drm_atomic_helper_mode_config_funcs;
    drm->mode_config.prefer_shadow_fbdev = true;
    
    /* Initialize connector */
    ret = drm_connector_init(drm, &mgpu->connector,
                             &mgpu_connector_funcs,
                             DRM_MODE_CONNECTOR_VIRTUAL);
    if (ret) {
        dev_err(dev, "Failed to initialize connector\n");
        goto err_mode;
    }
    
    drm_connector_helper_add(&mgpu->connector,
                             &mgpu_connector_helper_funcs);
    
    /* Initialize simple display pipe */
    ret = drm_simple_display_pipe_init(drm, &mgpu->pipe,
                                       &mgpu_pipe_funcs,
                                       mgpu_formats,
                                       ARRAY_SIZE(mgpu_formats),
                                       NULL,
                                       &mgpu->connector);
    if (ret) {
        dev_err(dev, "Failed to initialize display pipe\n");
        goto err_mode;
    }
    
    /* Enable damage clips */
    drm_plane_enable_fb_damage_clips(&mgpu->pipe.plane);
    
    /* Reset mode config */
    drm_mode_config_reset(drm);
    
    /* Initialize fbdev (optional) */
    drm_fbdev_generic_setup(drm, 32);
    
    /* Register DRM device */
    ret = drm_dev_register(drm, 0);
    if (ret) {
        dev_err(dev, "Failed to register DRM device\n");
        goto err_mode;
    }
    
    dev_info(dev, "MGPU DRM initialized\n");
    
    return 0;
    
err_mode:
    drm_mode_config_cleanup(drm);
err_fb:
    dma_free_coherent(dev, mgpu->fb_size, mgpu->fb_base, mgpu->fb_dma_addr);
    
    return ret;
}

static void mgpu_drm_unbind(struct device *dev, struct device *master,
                            void *data)
{
    struct platform_device *pdev = to_platform_device(dev);
    struct mgpu_drm_device *mgpu = platform_get_drvdata(pdev);
    struct drm_device *drm = &mgpu->drm;
    
    dev_info(dev, "Unbinding MGPU DRM\n");
    
    drm_dev_unregister(drm);
    drm_atomic_helper_shutdown(drm);
    drm_mode_config_cleanup(drm);
    
    if (mgpu->fb_base)
        dma_free_coherent(dev, mgpu->fb_size, mgpu->fb_base,
                         mgpu->fb_dma_addr);
}

static const struct component_ops mgpu_drm_ops = {
    .bind = mgpu_drm_bind,
    .unbind = mgpu_drm_unbind,
};

/* --- Platform Driver Integration --- */

int mgpu_drm_init(struct platform_device *pdev)
{
    return component_add(&pdev->dev, &mgpu_drm_ops);
}

void mgpu_drm_fini(struct platform_device *pdev)
{
    component_del(&pdev->dev, &mgpu_drm_ops);
}

/* Supported pixel formats */
static const uint32_t mgpu_formats[] = {
    DRM_FORMAT_XRGB8888,
    DRM_FORMAT_ARGB8888,
    DRM_FORMAT_RGB888,
    DRM_FORMAT_RGB565,
};

MODULE_DESCRIPTION("MGPU DRM Interface");
MODULE_AUTHOR("Rafeed Khan");
MODULE_LICENSE("GPL v2");
