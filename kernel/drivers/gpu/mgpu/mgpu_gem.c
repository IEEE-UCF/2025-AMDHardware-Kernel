#include <linux/module.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/sched/mm.h>
#include <linux/file.h>
#include <linux/anon_inodes.h>

#include "mgpu_drm.h"
#include "mgpu_gem.h"

/* Forward declarations */
static void mgpu_bo_vm_open(struct vm_area_struct *vma);
static void mgpu_bo_vm_close(struct vm_area_struct *vma);
static const struct vm_operations_struct mgpu_bo_vm_ops;

/* Buffer object structure */
struct mgpu_bo {
    struct mgpu_device *mdev;
    struct list_head list;
    
    /* Memory info */
    void *vaddr;           /* CPU virtual address */
    dma_addr_t dma_addr;   /* DMA/GPU address */
    size_t size;           /* Size in bytes */
    u32 flags;             /* Creation flags */
    
    /* Reference counting */
    struct kref refcount;
    
    /* Mapping info */
    struct page **pages;
    int nr_pages;
    struct sg_table *sgt;
    
    /* Handle for userspace */
    u32 handle;
    
    /* mmap offset management */
    struct drm_vma_offset_node vma_node;
    struct rb_node node;       /* For mmap offset tree */
    u64 mmap_offset;          /* Unique offset for mmap */
    
    /* File association */
    struct file *filp;         /* File that created this BO */
    
    /* Sync */
    bool cached;
    bool dirty;
};

/* Global IDR for handle management */
static DEFINE_IDR(mgpu_bo_idr);
static DEFINE_SPINLOCK(mgpu_bo_idr_lock);

/* Global mmap offset manager */
static DEFINE_MUTEX(mgpu_mmap_lock);
static struct rb_root mgpu_mmap_root = RB_ROOT;
static u64 mgpu_mmap_offset_counter = 0x10000;  /* Start at 64KB offset */

/* Allocate a unique mmap offset for the BO */
static int mgpu_bo_alloc_mmap_offset(struct mgpu_bo *bo)
{
    struct rb_node **new, *parent = NULL;
    u64 offset;
    
    mutex_lock(&mgpu_mmap_lock);
    
    /* Allocate a unique offset */
    offset = mgpu_mmap_offset_counter;
    mgpu_mmap_offset_counter += bo->size;
    mgpu_mmap_offset_counter = ALIGN(mgpu_mmap_offset_counter, PAGE_SIZE);
    
    bo->mmap_offset = offset;
    
    /* Insert into RB tree */
    new = &mgpu_mmap_root.rb_node;
    while (*new) {
        struct mgpu_bo *this = rb_entry(*new, struct mgpu_bo, node);
        
        parent = *new;
        if (offset < this->mmap_offset)
            new = &((*new)->rb_left);
        else if (offset > this->mmap_offset)
            new = &((*new)->rb_right);
        else {
            mutex_unlock(&mgpu_mmap_lock);
            return -EEXIST;  /* Should never happen */
        }
    }
    
    /* Add new node and rebalance tree */
    rb_link_node(&bo->node, parent, new);
    rb_insert_color(&bo->node, &mgpu_mmap_root);
    
    mutex_unlock(&mgpu_mmap_lock);
    
    return 0;
}

/* Free the mmap offset */
static void mgpu_bo_free_mmap_offset(struct mgpu_bo *bo)
{
    mutex_lock(&mgpu_mmap_lock);
    if (!RB_EMPTY_NODE(&bo->node)) {
        rb_erase(&bo->node, &mgpu_mmap_root);
        RB_CLEAR_NODE(&bo->node);
    }
    mutex_unlock(&mgpu_mmap_lock);
}

/* Look up BO by mmap offset */
struct mgpu_bo *mgpu_bo_lookup_by_offset(u64 offset)
{
    struct rb_node *node;
    struct mgpu_bo *bo = NULL;
    
    mutex_lock(&mgpu_mmap_lock);
    
    node = mgpu_mmap_root.rb_node;
    while (node) {
        struct mgpu_bo *this = rb_entry(node, struct mgpu_bo, node);
        
        if (offset < this->mmap_offset) {
            node = node->rb_left;
        } else if (offset >= this->mmap_offset + this->size) {
            node = node->rb_right;
        } else {
            bo = this;
            kref_get(&bo->refcount);
            break;
        }
    }
    
    mutex_unlock(&mgpu_mmap_lock);
    
    return bo;
}

/* Allocate a unique handle for the BO */
static int mgpu_bo_alloc_handle(struct mgpu_bo *bo)
{
    int ret;
    
    idr_preload(GFP_KERNEL);
    spin_lock(&mgpu_bo_idr_lock);
    ret = idr_alloc(&mgpu_bo_idr, bo, 1, 0, GFP_NOWAIT);
    spin_unlock(&mgpu_bo_idr_lock);
    idr_preload_end();
    
    if (ret < 0)
        return ret;
    
    bo->handle = ret;
    return 0;
}

/* Free the BO handle */
static void mgpu_bo_free_handle(struct mgpu_bo *bo)
{
    spin_lock(&mgpu_bo_idr_lock);
    idr_remove(&mgpu_bo_idr, bo->handle);
    spin_unlock(&mgpu_bo_idr_lock);
}

/* Look up BO by handle */
struct mgpu_bo *mgpu_bo_lookup(struct mgpu_device *mdev, u32 handle)
{
    struct mgpu_bo *bo;
    
    spin_lock(&mgpu_bo_idr_lock);
    bo = idr_find(&mgpu_bo_idr, handle);
    if (bo && bo->mdev == mdev)
        kref_get(&bo->refcount);
    else
        bo = NULL;
    spin_unlock(&mgpu_bo_idr_lock);
    
    return bo;
}

/* Free the actual BO memory and structure */
static void mgpu_bo_free(struct kref *ref)
{
    struct mgpu_bo *bo = container_of(ref, struct mgpu_bo, refcount);
    struct mgpu_device *mdev = bo->mdev;
    
    dev_dbg(mdev->dev, "Freeing BO handle %u, size %zu\n", bo->handle, bo->size);
    
    /* Remove from device list */
    mutex_lock(&mdev->bo_lock);
    list_del(&bo->list);
    mutex_unlock(&mdev->bo_lock);
    
    /* Free mmap offset */
    mgpu_bo_free_mmap_offset(bo);
    
    /* Free the memory based on allocation type */
    if (bo->vaddr) {
        if (bo->flags & MGPU_BO_FLAGS_COHERENT) {
            dma_free_coherent(mdev->dev, bo->size, bo->vaddr, bo->dma_addr);
        } else {
            dma_free_attrs(mdev->dev, bo->size, bo->vaddr, bo->dma_addr,
                          DMA_ATTR_WRITE_COMBINE);
        }
    }
    
    /* Free scatter-gather table if exists */
    if (bo->sgt) {
        sg_free_table(bo->sgt);
        kfree(bo->sgt);
    }
    
    /* Free pages array */
    kfree(bo->pages);
    
    /* Free handle */
    mgpu_bo_free_handle(bo);
    
    /* Free the BO structure */
    kfree(bo);
}

/* Release BO reference */
void mgpu_bo_put(struct mgpu_bo *bo)
{
    if (bo)
        kref_put(&bo->refcount, mgpu_bo_free);
}

/* Create a new buffer object */
int mgpu_bo_create(struct mgpu_device *mdev, struct mgpu_bo_create *args)
{
    struct mgpu_bo *bo;
    int ret;
    
    /* Validate size */
    if (!args->size || args->size > (256 * 1024 * 1024)) {
        dev_err(mdev->dev, "Invalid BO size: %u\n", args->size);
        return -EINVAL;
    }
    
    /* Align size to page boundary */
    args->size = PAGE_ALIGN(args->size);
    
    /* Allocate BO structure */
    bo = kzalloc(sizeof(*bo), GFP_KERNEL);
    if (!bo)
        return -ENOMEM;
    
    bo->mdev = mdev;
    bo->size = args->size;
    bo->flags = args->flags;
    kref_init(&bo->refcount);
    INIT_LIST_HEAD(&bo->list);
    RB_CLEAR_NODE(&bo->node);
    
    /* Allocate memory based on flags */
    if (args->flags & MGPU_BO_FLAGS_COHERENT) {
        /* Coherent DMA memory */
        bo->vaddr = dma_alloc_coherent(mdev->dev, bo->size,
                                       &bo->dma_addr, GFP_KERNEL);
        bo->cached = false;
    } else if (args->flags & MGPU_BO_FLAGS_WRITE_COMBINE) {
        /* Write-combined memory */
        bo->vaddr = dma_alloc_attrs(mdev->dev, bo->size,
                                   &bo->dma_addr, GFP_KERNEL,
                                   DMA_ATTR_WRITE_COMBINE);
        bo->cached = false;
    } else {
        /* Default cached memory */
        bo->vaddr = dma_alloc_coherent(mdev->dev, bo->size,
                                       &bo->dma_addr, GFP_KERNEL);
        bo->cached = true;
    }
    
    if (!bo->vaddr) {
        dev_err(mdev->dev, "Failed to allocate %zu bytes\n", bo->size);
        ret = -ENOMEM;
        goto err_free_bo;
    }
    
    /* Clear the memory */
    memset(bo->vaddr, 0, bo->size);
    
    /* Allocate handle */
    ret = mgpu_bo_alloc_handle(bo);
    if (ret) {
        dev_err(mdev->dev, "Failed to allocate handle\n");
        goto err_free_mem;
    }
    
    /* Allocate mmap offset */
    ret = mgpu_bo_alloc_mmap_offset(bo);
    if (ret) {
        dev_err(mdev->dev, "Failed to allocate mmap offset\n");
        goto err_free_handle;
    }
    
    /* Add to device list */
    mutex_lock(&mdev->bo_lock);
    list_add(&bo->list, &mdev->bo_list);
    mutex_unlock(&mdev->bo_lock);
    
    /* Return info to userspace */
    args->handle = bo->handle;
    args->gpu_addr = (u32)bo->dma_addr;  /* Truncate for 32-bit GPU */
    
    dev_dbg(mdev->dev, "Created BO handle %u, size %zu, gpu_addr 0x%08x, mmap_offset 0x%llx\n",
            bo->handle, bo->size, args->gpu_addr, bo->mmap_offset);
    
    return 0;
    
err_free_handle:
    mgpu_bo_free_handle(bo);
err_free_mem:
    if (bo->flags & MGPU_BO_FLAGS_COHERENT) {
        dma_free_coherent(mdev->dev, bo->size, bo->vaddr, bo->dma_addr);
    } else {
        dma_free_attrs(mdev->dev, bo->size, bo->vaddr, bo->dma_addr,
                      DMA_ATTR_WRITE_COMBINE);
    }
err_free_bo:
    kfree(bo);
    return ret;
}

/* Destroy a buffer object */
int mgpu_bo_destroy(struct mgpu_device *mdev, struct mgpu_bo_destroy *args)
{
    struct mgpu_bo *bo;
    
    bo = mgpu_bo_lookup(mdev, args->handle);
    if (!bo) {
        dev_err(mdev->dev, "Invalid BO handle %u\n", args->handle);
        return -EINVAL;
    }
    
    /* Drop lookup reference */
    mgpu_bo_put(bo);
    
    /* Drop creation reference - this should free it */
    mgpu_bo_put(bo);
    
    return 0;
}

/* mmap() implementation for BOs */
static int mgpu_bo_mmap_obj(struct mgpu_bo *bo, struct vm_area_struct *vma)
{
    struct mgpu_device *mdev = bo->mdev;
    unsigned long pfn;
    int ret;
    
    /* Check size */
    if (vma->vm_end - vma->vm_start > bo->size)
        return -EINVAL;
    
    /* Set VM flags */
    vma->vm_flags |= VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP;
    vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
    
    if (bo->flags & MGPU_BO_FLAGS_WRITE_COMBINE)
        vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
    
    /* Map the pages */
    pfn = bo->dma_addr >> PAGE_SHIFT;
    ret = remap_pfn_range(vma, vma->vm_start, pfn,
                         vma->vm_end - vma->vm_start,
                         vma->vm_page_prot);
    
    if (ret)
        dev_err(mdev->dev, "Failed to map BO: %d\n", ret);
    
    return ret;
}

/* VM operations for BO mappings */
static void mgpu_bo_vm_open(struct vm_area_struct *vma)
{
    struct mgpu_bo *bo = vma->vm_private_data;
    if (bo)
        kref_get(&bo->refcount);
}

static void mgpu_bo_vm_close(struct vm_area_struct *vma)
{
    struct mgpu_bo *bo = vma->vm_private_data;
    if (bo)
        mgpu_bo_put(bo);
}

static const struct vm_operations_struct mgpu_bo_vm_ops = {
    .open = mgpu_bo_vm_open,
    .close = mgpu_bo_vm_close,
};

/* Get mmap offset for a BO */
int mgpu_bo_mmap(struct mgpu_device *mdev, struct mgpu_bo_mmap *args,
                struct file *filp)
{
    struct mgpu_bo *bo;
    
    bo = mgpu_bo_lookup(mdev, args->handle);
    if (!bo) {
        dev_err(mdev->dev, "Invalid BO handle %u\n", args->handle);
        return -EINVAL;
    }
    
    /* Store file pointer for permission checking */
    if (!bo->filp)
        bo->filp = filp;
    
    /* Return the mmap offset to userspace */
    args->offset = bo->mmap_offset;
    
    mgpu_bo_put(bo);
    return 0;
}

/* Actual mmap implementation called from file_operations->mmap */
int mgpu_mmap(struct file *filp, struct vm_area_struct *vma)
{
    struct mgpu_device *mdev = filp->private_data;
    struct mgpu_bo *bo;
    unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
    unsigned long size = vma->vm_end - vma->vm_start;
    unsigned long pfn;
    int ret;
    
    /* Look up BO by mmap offset */
    bo = mgpu_bo_lookup_by_offset(offset);
    if (!bo) {
        dev_err(mdev->dev, "Invalid mmap offset 0x%lx\n", offset);
        return -EINVAL;
    }
    
    /* Check if this file has permission to map this BO */
    if (bo->filp && bo->filp != filp) {
        dev_err(mdev->dev, "Permission denied for BO mmap\n");
        mgpu_bo_put(bo);
        return -EPERM;
    }
    
    /* Check size */
    if (size > bo->size) {
        dev_err(mdev->dev, "mmap size %lu exceeds BO size %zu\n", size, bo->size);
        mgpu_bo_put(bo);
        return -EINVAL;
    }
    
    /* Set VM flags */
    vma->vm_flags |= VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP;
    
    /* Set page protection based on BO flags */
    if (bo->flags & MGPU_BO_FLAGS_CACHED) {
        vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);
    } else if (bo->flags & MGPU_BO_FLAGS_WRITE_COMBINE) {
        vma->vm_page_prot = pgprot_writecombine(vm_get_page_prot(vma->vm_flags));
    } else {
        vma->vm_page_prot = pgprot_noncached(vm_get_page_prot(vma->vm_flags));
    }
    
    /* Map the physical pages */
    pfn = bo->dma_addr >> PAGE_SHIFT;
    ret = remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot);
    
    if (ret) {
        dev_err(mdev->dev, "Failed to map BO: %d\n", ret);
        mgpu_bo_put(bo);
        return ret;
    }
    
    /* Store BO reference in VMA private data for unmapping */
    vma->vm_private_data = bo;
    vma->vm_ops = &mgpu_bo_vm_ops;
    
    dev_dbg(mdev->dev, "Mapped BO handle %u at offset 0x%lx, size %lu\n",
            bo->handle, offset, size);
    
    return 0;
}

/* VM operations for BO mappings */
static void mgpu_bo_vm_open(struct vm_area_struct *vma)
{
    struct mgpu_bo *bo = vma->vm_private_data;
    if (bo)
        kref_get(&bo->refcount);
}

static void mgpu_bo_vm_close(struct vm_area_struct *vma)
{
    struct mgpu_bo *bo = vma->vm_private_data;
    if (bo)
        mgpu_bo_put(bo);
}

static const struct vm_operations_struct mgpu_bo_vm_ops = {
    .open = mgpu_bo_vm_open,
    .close = mgpu_bo_vm_close,
};

/* CPU access to BO */
void *mgpu_bo_vmap(struct mgpu_bo *bo)
{
    if (!bo)
        return NULL;
    return bo->vaddr;
}

void mgpu_bo_vunmap(struct mgpu_bo *bo, void *vaddr)
{
    /* Nothing to do for our simple implementation */
}

/* Cache management */
int mgpu_bo_cpu_prep(struct mgpu_bo *bo, bool write)
{
    struct mgpu_device *mdev = bo->mdev;
    
    if (!bo->cached)
        return 0;
    
    /* Invalidate cache for reads */
    if (!write) {
        dma_sync_single_for_cpu(mdev->dev, bo->dma_addr,
                               bo->size, DMA_FROM_DEVICE);
    }
    
    return 0;
}

int mgpu_bo_cpu_fini(struct mgpu_bo *bo, bool write)
{
    struct mgpu_device *mdev = bo->mdev;
    
    if (!bo->cached)
        return 0;
    
    /* Flush cache for writes */
    if (write) {
        dma_sync_single_for_device(mdev->dev, bo->dma_addr,
                                   bo->size, DMA_TO_DEVICE);
        bo->dirty = true;
    }
    
    return 0;
}

/* Clean up all BOs on device removal */
void mgpu_gem_cleanup(struct mgpu_device *mdev)
{
    struct mgpu_bo *bo, *tmp;
    struct rb_node *node;
    
    mutex_lock(&mdev->bo_lock);
    list_for_each_entry_safe(bo, tmp, &mdev->bo_list, list) {
        list_del(&bo->list);
        /* Force free */
        while (kref_read(&bo->refcount) > 0)
            mgpu_bo_put(bo);
    }
    mutex_unlock(&mdev->bo_lock);
    
    /* Clean up mmap RB tree */
    mutex_lock(&mgpu_mmap_lock);
    while ((node = rb_first(&mgpu_mmap_root))) {
        bo = rb_entry(node, struct mgpu_bo, node);
        rb_erase(node, &mgpu_mmap_root);
        /* BO should already be freed, just clean up the tree */
    }
    mutex_unlock(&mgpu_mmap_lock);
    
    /* Clean up IDR */
    idr_destroy(&mgpu_bo_idr);
}