#include <linux/module.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "mgpu_drm.h"
#include "mgpu_regs.h"

/* Shader slot information */
struct mgpu_shader_slot {
    u32 *code;
    size_t size;
    u32 type;
    bool loaded;
};

/* Shader manager */
struct mgpu_shader_mgr {
    struct mgpu_device *mdev;
    struct mgpu_shader_slot slots[16];  /* 16 shader slots */
    struct mutex lock;
};

/* Initialize shader manager */
int mgpu_shader_init(struct mgpu_device *mdev)
{
    struct mgpu_shader_mgr *mgr;
    
    mgr = kzalloc(sizeof(*mgr), GFP_KERNEL);
    if (!mgr)
        return -ENOMEM;
    
    mgr->mdev = mdev;
    mutex_init(&mgr->lock);
    
    mdev->shader_mgr = mgr;
    
    dev_info(mdev->dev, "Shader manager initialized\n");
    
    return 0;
}

/* Clean up shader manager */
void mgpu_shader_fini(struct mgpu_device *mdev)
{
    struct mgpu_shader_mgr *mgr = mdev->shader_mgr;
    int i;
    
    if (!mgr)
        return;
    
    /* Free all loaded shaders */
    for (i = 0; i < 16; i++) {
        if (mgr->slots[i].code) {
            kfree(mgr->slots[i].code);
            mgr->slots[i].code = NULL;
            mgr->slots[i].loaded = false;
        }
    }
    
    kfree(mgr);
    mdev->shader_mgr = NULL;
}

/* Validate shader binary */
static int mgpu_shader_validate(u32 *code, size_t size, u32 type)
{
    u32 magic, version;
    size_t min_size = 8;  /* Minimum shader size in bytes */
    
    if (size < min_size || size > MGPU_REG_INSTR_MEM_SIZE) {
        pr_err("Invalid shader size: %zu\n", size);
        return -EINVAL;
    }
    
    /* Check alignment */
    if (size & 3) {
        pr_err("Shader size not aligned to 4 bytes\n");
        return -EINVAL;
    }
    
    /* Simple validation - check for magic number */
    magic = code[0];
    version = code[1];
    
    /* Expected magic: 'MGPU' */
    if (magic != 0x5547504D) {  /* 'MGPU' in little-endian */
        pr_warn("Shader magic not found, assuming raw binary\n");
        /* Allow raw binaries for now */
    }
    
    /* Validate shader type */
    if (type > MGPU_SHADER_COMPUTE) {
        pr_err("Invalid shader type: %u\n", type);
        return -EINVAL;
    }
    
    /* TODO: More sophisticated validation */
    /* - Check instruction encoding */
    /* - Verify resource usage */
    /* - Validate branch targets */
    
    return 0;
}

/* Write shader to instruction memory */
static int mgpu_shader_write_to_hw(struct mgpu_device *mdev, u32 slot,
                                   u32 *code, size_t size)
{
    u32 instr_offset;
    u32 i, dwords;
    
    /* Calculate instruction memory offset for this slot */
    /* Each slot gets 256 dwords (1KB) */
    instr_offset = slot * 256;
    
    if (instr_offset * 4 + size > MGPU_REG_INSTR_MEM_SIZE) {
        dev_err(mdev->dev, "Shader too large for slot %u\n", slot);
        return -ENOSPC;
    }
    
    dwords = size / 4;
    
    /* Write shader to instruction memory */
    for (i = 0; i < dwords; i++) {
        mgpu_write(mdev, MGPU_REG_SHADER_ADDR, instr_offset + i);
        mgpu_write(mdev, MGPU_REG_SHADER_DATA, code[i]);
    }
    
    /* Set shader control register */
    mgpu_write(mdev, MGPU_REG_SHADER_CTRL, (slot << 16) | (dwords & 0xFFFF));
    
    dev_dbg(mdev->dev, "Wrote %zu bytes to shader slot %u\n", size, slot);
    
    return 0;
}

/* Load shader */
int mgpu_load_shader(struct mgpu_device *mdev, struct mgpu_load_shader *args)
{
    struct mgpu_shader_mgr *mgr = mdev->shader_mgr;
    struct mgpu_shader_slot *slot;
    u32 *code;
    int ret;
    
    if (!mgr)
        return -ENODEV;
    
    /* Validate slot number */
    if (args->slot >= 16) {
        dev_err(mdev->dev, "Invalid shader slot: %u\n", args->slot);
        return -EINVAL;
    }
    
    /* Validate size */
    if (!args->size || args->size > MGPU_REG_INSTR_MEM_SIZE) {
        dev_err(mdev->dev, "Invalid shader size: %u\n", args->size);
        return -EINVAL;
    }
    
    /* Allocate buffer for shader code */
    code = kmalloc(args->size, GFP_KERNEL);
    if (!code)
        return -ENOMEM;
    
    /* Copy shader from userspace */
    if (copy_from_user(code, (void __user *)args->data, args->size)) {
        kfree(code);
        return -EFAULT;
    }
    
    /* Validate shader binary */
    ret = mgpu_shader_validate(code, args->size, args->type);
    if (ret) {
        kfree(code);
        return ret;
    }
    
    mutex_lock(&mgr->lock);
    
    slot = &mgr->slots[args->slot];
    
    /* Free previous shader if loaded */
    if (slot->code) {
        kfree(slot->code);
    }
    
    /* Store shader info */
    slot->code = code;
    slot->size = args->size;
    slot->type = args->type;
    slot->loaded = false;
    
    /* Write to hardware */
    ret = mgpu_shader_write_to_hw(mdev, args->slot, code, args->size);
    if (ret) {
        kfree(code);
        slot->code = NULL;
        mutex_unlock(&mgr->lock);
        return ret;
    }
    
    slot->loaded = true;
    
    mutex_unlock(&mgr->lock);
    
    dev_info(mdev->dev, "Loaded %s shader to slot %u (%u bytes)\n",
             args->type == MGPU_SHADER_VERTEX ? "vertex" :
             args->type == MGPU_SHADER_FRAGMENT ? "fragment" : "compute",
             args->slot, args->size);
    
    return 0;
}

/* Bind shader for execution */
int mgpu_shader_bind(struct mgpu_device *mdev, u32 slot, u32 type)
{
    struct mgpu_shader_mgr *mgr = mdev->shader_mgr;
    struct mgpu_shader_slot *shader;
    u32 pc_offset;
    
    if (!mgr)
        return -ENODEV;
    
    if (slot >= 16)
        return -EINVAL;
    
    mutex_lock(&mgr->lock);
    
    shader = &mgr->slots[slot];
    if (!shader->loaded) {
        mutex_unlock(&mgr->lock);
        return -ENOENT;
    }
    
    if (shader->type != type) {
        mutex_unlock(&mgr->lock);
        return -EINVAL;
    }
    
    /* Calculate PC offset for this slot */
    pc_offset = slot * 256;  /* Each slot is 256 dwords */
    
    /* Set shader PC based on type */
    switch (type) {
    case MGPU_SHADER_VERTEX:
        mgpu_write(mdev, MGPU_REG_SHADER_PC, pc_offset);
        break;
    case MGPU_SHADER_FRAGMENT:
        mgpu_write(mdev, MGPU_REG_SHADER_PC + 4, pc_offset);
        break;
    case MGPU_SHADER_COMPUTE:
        mgpu_write(mdev, MGPU_REG_SHADER_PC + 8, pc_offset);
        break;
    }
    
    mutex_unlock(&mgr->lock);
    
    return 0;
}

/* Get shader info */
int mgpu_shader_get_info(struct mgpu_device *mdev, u32 slot,
                        size_t *size, u32 *type)
{
    struct mgpu_shader_mgr *mgr = mdev->shader_mgr;
    struct mgpu_shader_slot *shader;
    
    if (!mgr)
        return -ENODEV;
    
    if (slot >= 16)
        return -EINVAL;
    
    mutex_lock(&mgr->lock);
    
    shader = &mgr->slots[slot];
    if (!shader->loaded) {
        mutex_unlock(&mgr->lock);
        return -ENOENT;
    }
    
    if (size)
        *size = shader->size;
    if (type)
        *type = shader->type;
    
    mutex_unlock(&mgr->lock);
    
    return 0;
}