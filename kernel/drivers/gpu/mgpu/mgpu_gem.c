#include <linux/module.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/sched/mm.h>
#include <linux/file.h>
#include <linux/anon_inodes.h>

#include "mgpu_drm.h"
#include "mgpu_gem.h"

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
    
    /* Sync */
    bool cached;
    bool dirty;
};

static DEFINE_IDR(mgpu_bo_idr);
static DEFINE_SPINLOCK(mgpu_bo_idr_lock);

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
    
    /* Add to device list */
    mutex_lock(&mdev->bo_lock);
    list_add(&bo->list, &mdev->bo_list);
    mutex_unlock(&mdev->bo_lock);
    
    /* Return info to userspace */
    args->handle = bo->handle;
    args->gpu_addr = (u32)bo->dma_addr;  /* Truncate for 32-bit GPU */
    
    dev_dbg(mdev->dev, "Created BO handle %u, size %zu, gpu_addr 0x%08x\n",
            bo->handle, bo->size, args->gpu_addr);
    
    return 0;
    
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
    
    /* Drop creation reference, this should free it */
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
    
    /* For simplicity, use handle as offset */
    args->offset = (u64)args->handle << PAGE_SHIFT;
    
    /* Store BO reference for later mmap() */
    /* Im simplyifying for this now ig, im going to add a robust method later */
    
    mgpu_bo_put(bo);
    return 0;
}

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
    
    mutex_lock(&mdev->bo_lock);
    list_for_each_entry_safe(bo, tmp, &mdev->bo_list, list) {
        list_del(&bo->list);
        /* Force free */
        while (kref_read(&bo->refcount) > 0)
            mgpu_bo_put(bo);
    }
    mutex_unlock(&mdev->bo_lock);
    
    /* Clean up IDR */
    idr_destroy(&mgpu_bo_idr);
}
