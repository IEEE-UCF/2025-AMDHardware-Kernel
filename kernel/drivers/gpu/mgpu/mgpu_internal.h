#ifndef _MGPU_INTERNAL_H_
#define _MGPU_INTERNAL_H_

#include <linux/types.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/kref.h>

/* Forward declarations */
struct mgpu_device;
struct mgpu_bo;
struct mgpu_ring;
struct mgpu_fence_context;
struct mgpu_shader_mgr;

/* Main device structure, extended */
struct mgpu_device {
    struct device *dev;
    void __iomem *mmio_base;
    struct resource *mmio_res;
    int irq;
    
    /* Device capabilities */
    u32 version;
    u32 caps;
    u32 num_engines;
    u32 num_queues;
    
    /* Memory management */
    struct list_head bo_list;
    struct mutex bo_lock;
    
    /* Command submission */
    struct mgpu_ring *cmd_ring;
    spinlock_t cmd_lock;
    
    /* Fence context */
    struct mgpu_fence_context *fence_ctx;
    
    /* Shader manager */
    struct mgpu_shader_mgr *shader_mgr;
    
    /* Interrupt handling */
    struct tasklet_struct irq_tasklet;
    u32 irq_status;
    
    /* Debug */
    struct dentry *debugfs_root;
    
    /* Character device */
    struct cdev cdev;
    dev_t devno;
    struct class *class;
};

/* Memory Management (mgpu_gem.c) */

/* Buffer object functions */
int mgpu_bo_create(struct mgpu_device *mdev, struct mgpu_bo_create *args);
int mgpu_bo_destroy(struct mgpu_device *mdev, struct mgpu_bo_destroy *args);
int mgpu_bo_mmap(struct mgpu_device *mdev, struct mgpu_bo_mmap *args,
                struct file *filp);
int mgpu_mmap(struct file *filp, struct vm_area_struct *vma);
struct mgpu_bo *mgpu_bo_lookup(struct mgpu_device *mdev, u32 handle);
struct mgpu_bo *mgpu_bo_lookup_by_offset(u64 offset);
void mgpu_bo_put(struct mgpu_bo *bo);
void *mgpu_bo_vmap(struct mgpu_bo *bo);
void mgpu_bo_vunmap(struct mgpu_bo *bo, void *vaddr);
int mgpu_bo_cpu_prep(struct mgpu_bo *bo, bool write);
int mgpu_bo_cpu_fini(struct mgpu_bo *bo, bool write);
void mgpu_gem_cleanup(struct mgpu_device *mdev);

/* Command Queue (mgpu_cmdq.c) */

/* Ring buffer functions */
struct mgpu_ring *mgpu_ring_create(struct mgpu_device *mdev, size_t size, u32 queue_id);
void mgpu_ring_destroy(struct mgpu_ring *ring);
int mgpu_submit_commands(struct mgpu_device *mdev, struct mgpu_submit *args);
int mgpu_cmdq_init(struct mgpu_device *mdev);
void mgpu_cmdq_fini(struct mgpu_device *mdev);

/* Fence Management (mgpu_fence.c) */

/* Fence functions */
int mgpu_fence_init(struct mgpu_device *mdev);
void mgpu_fence_fini(struct mgpu_device *mdev);
u32 mgpu_fence_next(struct mgpu_device *mdev);
bool mgpu_fence_signaled(struct mgpu_device *mdev, u64 fence_addr, u32 fence_value);
void mgpu_fence_process(struct mgpu_device *mdev);
int mgpu_wait_fence(struct mgpu_device *mdev, struct mgpu_wait_fence *args);
int mgpu_fence_emit(struct mgpu_device *mdev, u64 fence_addr, u32 fence_value);

/* Shader Management (mgpu_shader.c) */

/* Shader functions */
int mgpu_shader_init(struct mgpu_device *mdev);
void mgpu_shader_fini(struct mgpu_device *mdev);
int mgpu_load_shader(struct mgpu_device *mdev, struct mgpu_load_shader *args);
int mgpu_shader_bind(struct mgpu_device *mdev, u32 slot, u32 type);
int mgpu_shader_get_info(struct mgpu_device *mdev, u32 slot,
                        size_t *size, u32 *type);

/* Reset and Recovery (mgpu_reset.c) */

/* Reset functions */
int mgpu_reset_hw(struct mgpu_device *mdev);
int mgpu_reset_engine(struct mgpu_device *mdev, u32 engine);
void mgpu_reset_on_error(struct mgpu_device *mdev);

/* Health Monitoring (mgpu_health.c) */

/* Health check functions */
int mgpu_health_check(struct mgpu_device *mdev);
int mgpu_run_selftest(struct mgpu_device *mdev);
void mgpu_dump_state(struct mgpu_device *mdev);

/* DebugFS (mgpu_debugfs.c) */

/* DebugFS functions */
int mgpu_debugfs_init(struct mgpu_device *mdev);
void mgpu_debugfs_fini(struct mgpu_device *mdev);

/* Helper Macros */

/* Register access helpers */
static inline u32 mgpu_read(struct mgpu_device *mdev, u32 offset)
{
    return ioread32(mdev->mmio_base + offset);
}

static inline void mgpu_write(struct mgpu_device *mdev, u32 offset, u32 value)
{
    iowrite32(value, mdev->mmio_base + offset);
}

/* Logging helpers */
#define mgpu_err(mdev, fmt, ...) \
    dev_err((mdev)->dev, fmt, ##__VA_ARGS__)

#define mgpu_warn(mdev, fmt, ...) \
    dev_warn((mdev)->dev, fmt, ##__VA_ARGS__)

#define mgpu_info(mdev, fmt, ...) \
    dev_info((mdev)->dev, fmt, ##__VA_ARGS__)

#define mgpu_dbg(mdev, fmt, ...) \
    dev_dbg((mdev)->dev, fmt, ##__VA_ARGS__)

#endif /* _MGPU_INTERNAL_H_ */
