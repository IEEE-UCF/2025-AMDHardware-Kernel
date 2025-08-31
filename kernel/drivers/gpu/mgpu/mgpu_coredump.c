/*
 * MGPU Coredump - Captures GPU state on errors/hangs
 * Based on actual hardware registers and state
 *
 * Copyright (C) 2024
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/devcoredump.h>
#include <linux/scatterlist.h>
#include <linux/string.h>
#include <linux/time.h>
#include <linux/timekeeping.h>

#include "mgpu_drm.h"
#include "mgpu_regs.h"
#include "mgpu_internal.h"

/* Coredump header version */
#define MGPU_COREDUMP_VERSION 1
#define MGPU_COREDUMP_MAGIC   0x4D475055  /* 'MGPU' */

/* Section types in coredump */
enum mgpu_coredump_section {
    MGPU_DUMP_HEADER = 0,
    MGPU_DUMP_REGISTERS,
    MGPU_DUMP_COMMAND_RING,
    MGPU_DUMP_SHADER_STATE,
    MGPU_DUMP_VERTEX_STATE,
    MGPU_DUMP_RASTER_STATE,
    MGPU_DUMP_INSTR_MEM,
    MGPU_DUMP_ERROR_INFO,
    MGPU_DUMP_BO_LIST,
    MGPU_DUMP_END
};

/* Coredump header */
struct mgpu_coredump_header {
    u32 magic;
    u32 version;
    u64 timestamp;
    u32 gpu_version;
    u32 gpu_caps;
    u32 error_status;
    u32 error_code;
    u32 reset_count;
    u32 num_sections;
    u32 total_size;
    char kernel_version[64];
    char driver_version[32];
} __packed;

/* Section header */
struct mgpu_coredump_section_header {
    u32 type;
    u32 size;
    u32 offset;
    u32 flags;
    char name[32];
} __packed;

/* Register dump */
struct mgpu_register_dump {
    struct {
        u32 version;
        u32 caps;
        u32 control;
        u32 status;
        u32 scratch;
    } base;
    
    struct {
        u32 irq_status;
        u32 irq_enable;
    } interrupt;
    
    struct {
        u32 cmd_base;
        u32 cmd_size;
        u32 cmd_head;
        u32 cmd_tail;
    } command_queue[MGPU_MAX_QUEUES];
    
    struct {
        u32 fence_addr;
        u32 fence_value;
    } fence;
    
    struct {
        u32 vertex_base;
        u32 vertex_count;
        u32 vertex_stride;
    } vertex;
    
    struct {
        u32 shader_pc;
        u32 shader_addr;
        u32 shader_data;
        u32 shader_ctrl;
    } shader;
} __packed;

/* Command ring dump */
struct mgpu_cmdring_dump {
    u32 queue_id;
    u32 size;
    u32 head;
    u32 tail;
    u32 num_commands;
    u8 data[];  /* Flexible array for ring contents */
} __packed;

/* Shader state dump */
struct mgpu_shader_dump {
    u32 active_slots;
    u32 pc_values[16];  /* PC for each slot */
    struct {
        u32 slot;
        u32 type;
        u32 size;
        u32 loaded;
    } slots[16];
} __packed;

/* Error information */
struct mgpu_error_dump {
    u32 error_code;
    u32 error_address;
    u32 error_data;
    u32 hang_detected;
    u32 last_fence;
    u32 last_cmd_head;
    u64 timestamp;
    char description[256];
} __packed;

/* Coredump state */
struct mgpu_coredump_state {
    struct mgpu_device *mdev;
    void *data;
    size_t size;
    size_t offset;
    
    /* Sections */
    struct mgpu_coredump_header header;
    struct mgpu_register_dump regs;
    struct mgpu_shader_dump shaders;
    struct mgpu_error_dump error;
};

/* Helper to append data to coredump */
static void mgpu_coredump_append(struct mgpu_coredump_state *state,
                                 const void *data, size_t len)
{
    if (state->offset + len <= state->size) {
        memcpy(state->data + state->offset, data, len);
        state->offset += len;
    }
}

/* Capture register state */
static void mgpu_coredump_capture_registers(struct mgpu_coredump_state *state)
{
    struct mgpu_device *mdev = state->mdev;
    struct mgpu_register_dump *regs = &state->regs;
    int i;
    
    /* Base registers */
    regs->base.version = mgpu_read(mdev, MGPU_REG_VERSION);
    regs->base.caps = mgpu_read(mdev, MGPU_REG_CAPS);
    regs->base.control = mgpu_read(mdev, MGPU_REG_CONTROL);
    regs->base.status = mgpu_read(mdev, MGPU_REG_STATUS);
    regs->base.scratch = mgpu_read(mdev, MGPU_REG_SCRATCH);
    
    /* Interrupt registers */
    regs->interrupt.irq_status = mgpu_read(mdev, MGPU_REG_IRQ_STATUS);
    regs->interrupt.irq_enable = mgpu_read(mdev, MGPU_REG_IRQ_ENABLE);
    
    /* Command queue registers for each queue */
    for (i = 0; i < MGPU_MAX_QUEUES && i < mdev->num_queues; i++) {
        u32 offset = i * 0x10;
        regs->command_queue[i].cmd_base = mgpu_read(mdev, MGPU_REG_CMD_BASE + offset);
        regs->command_queue[i].cmd_size = mgpu_read(mdev, MGPU_REG_CMD_SIZE + offset);
        regs->command_queue[i].cmd_head = mgpu_read(mdev, MGPU_REG_CMD_HEAD + offset);
        regs->command_queue[i].cmd_tail = mgpu_read(mdev, MGPU_REG_CMD_TAIL + offset);
    }
    
    /* Fence registers */
    regs->fence.fence_addr = mgpu_read(mdev, MGPU_REG_FENCE_ADDR);
    regs->fence.fence_value = mgpu_read(mdev, MGPU_REG_FENCE_VALUE);
    
    /* Vertex registers */
    regs->vertex.vertex_base = mgpu_read(mdev, MGPU_REG_VERTEX_BASE);
    regs->vertex.vertex_count = mgpu_read(mdev, MGPU_REG_VERTEX_COUNT);
    regs->vertex.vertex_stride = mgpu_read(mdev, MGPU_REG_VERTEX_STRIDE);
    
    /* Shader registers */
    regs->shader.shader_pc = mgpu_read(mdev, MGPU_REG_SHADER_PC);
    regs->shader.shader_addr = mgpu_read(mdev, MGPU_REG_SHADER_ADDR);
    regs->shader.shader_data = mgpu_read(mdev, MGPU_REG_SHADER_DATA);
    regs->shader.shader_ctrl = mgpu_read(mdev, MGPU_REG_SHADER_CTRL);
}

/* Capture command ring contents */
static size_t mgpu_coredump_capture_cmdring(struct mgpu_coredump_state *state,
                                            void *buffer, u32 queue_id)
{
    struct mgpu_device *mdev = state->mdev;
    struct mgpu_ring *ring = mdev->cmd_ring;  /* TODO: Support multiple rings */
    struct mgpu_cmdring_dump *dump = buffer;
    size_t dump_size;
    u32 ring_size;
    
    if (!ring || queue_id != 0)
        return 0;
    
    /* Get ring state */
    dump->queue_id = queue_id;
    dump->size = ring->size;
    dump->head = mgpu_read(mdev, MGPU_REG_CMD_HEAD + (queue_id * 0x10));
    dump->tail = mgpu_read(mdev, MGPU_REG_CMD_TAIL + (queue_id * 0x10));
    
    /* Calculate how much data to dump (last 4KB or entire ring if smaller) */
    ring_size = min(ring->size, 4096U);
    dump_size = sizeof(*dump) + ring_size;
    
    /* Copy ring contents if we have space */
    if (buffer && ring->vaddr) {
        memcpy(dump->data, ring->vaddr, ring_size);
        
        /* Count commands in ring */
        u32 *cmds = (u32 *)dump->data;
        u32 count = 0;
        u32 offset = 0;
        
        while (offset < ring_size / 4) {
            struct mgpu_cmd_header *hdr = (struct mgpu_cmd_header *)&cmds[offset];
            if (hdr->opcode == 0 || hdr->size == 0)
                break;
            count++;
            offset += hdr->size;
        }
        dump->num_commands = count;
    }
    
    return dump_size;
}

/* Capture shader state */
static void mgpu_coredump_capture_shaders(struct mgpu_coredump_state *state)
{
    struct mgpu_device *mdev = state->mdev;
    struct mgpu_shader_dump *dump = &state->shaders;
    struct mgpu_shader_mgr *mgr = mdev->shader_mgr;
    int i;
    
    if (!mgr)
        return;
    
    dump->active_slots = 0;
    
    mutex_lock(&mgr->lock);
    for (i = 0; i < 16; i++) {
        if (mgr->slots[i].loaded) {
            dump->slots[i].slot = i;
            dump->slots[i].type = mgr->slots[i].type;
            dump->slots[i].size = mgr->slots[i].size;
            dump->slots[i].loaded = 1;
            dump->active_slots |= (1 << i);
            
            /* Capture PC value for this slot */
            dump->pc_values[i] = i * 256;  /* Each slot starts at offset i*256 */
        }
    }
    mutex_unlock(&mgr->lock);
}

/* Capture instruction memory */
static size_t mgpu_coredump_capture_instrmem(struct mgpu_coredump_state *state,
                                             void *buffer)
{
    struct mgpu_device *mdev = state->mdev;
    u32 *instr_mem = buffer;
    u32 i;
    
    if (!buffer)
        return MGPU_REG_INSTR_MEM_SIZE;
    
    /* Read instruction memory through shader data port */
    for (i = 0; i < MGPU_REG_INSTR_MEM_SIZE / 4; i++) {
        mgpu_write(mdev, MGPU_REG_SHADER_ADDR, i);
        instr_mem[i] = mgpu_read(mdev, MGPU_REG_SHADER_DATA);
    }
    
    return MGPU_REG_INSTR_MEM_SIZE;
}

/* Capture error information */
static void mgpu_coredump_capture_error(struct mgpu_coredump_state *state)
{
    struct mgpu_device *mdev = state->mdev;
    struct mgpu_error_dump *error = &state->error;
    u32 status = mgpu_read(mdev, MGPU_REG_STATUS);
    
    error->error_code = 0;
    error->error_address = 0;
    error->error_data = 0;
    error->timestamp = ktime_get_real_ns();
    
    /* Check error status */
    if (status & MGPU_STATUS_ERROR) {
        /* Extract error code (would need extended registers in real hardware) */
        error->error_code = MGPU_ERROR_INVALID_CMD;  /* Example */
        snprintf(error->description, sizeof(error->description),
                 "GPU error detected: status=0x%08x", status);
    }
    
    if (status & MGPU_STATUS_HALTED) {
        error->hang_detected = 1;
        error->last_fence = mgpu_read(mdev, MGPU_REG_FENCE_VALUE);
        error->last_cmd_head = mgpu_read(mdev, MGPU_REG_CMD_HEAD);
        strncat(error->description, " GPU halted/hung.", 
                sizeof(error->description) - strlen(error->description) - 1);
    }
    
    if (status & MGPU_STATUS_CMD_FULL) {
        strncat(error->description, " Command queue full.", 
                sizeof(error->description) - strlen(error->description) - 1);
    }
}

/* Create coredump */
static void *mgpu_coredump_create(struct mgpu_device *mdev, size_t *dump_size)
{
    struct mgpu_coredump_state state = {0};
    struct mgpu_coredump_section_header *sections;
    void *dump_data;
    size_t total_size;
    int num_sections = 0;
    
    state.mdev = mdev;
    
    /* Capture state */
    mgpu_coredump_capture_registers(&state);
    mgpu_coredump_capture_shaders(&state);
    mgpu_coredump_capture_error(&state);
    
    /* Calculate total size needed */
    total_size = sizeof(struct mgpu_coredump_header);
    total_size += sizeof(struct mgpu_coredump_section_header) * MGPU_DUMP_END;
    total_size += sizeof(struct mgpu_register_dump);
    total_size += mgpu_coredump_capture_cmdring(&state, NULL, 0);
    total_size += sizeof(struct mgpu_shader_dump);
    total_size += mgpu_coredump_capture_instrmem(&state, NULL);
    total_size += sizeof(struct mgpu_error_dump);
    
    /* Allocate dump buffer */
    dump_data = kvmalloc(total_size, GFP_KERNEL);
    if (!dump_data)
        return NULL;
    
    state.data = dump_data;
    state.size = total_size;
    state.offset = 0;
    
    /* Fill header */
    state.header.magic = MGPU_COREDUMP_MAGIC;
    state.header.version = MGPU_COREDUMP_VERSION;
    state.header.timestamp = ktime_get_real_ns();
    state.header.gpu_version = state.regs.base.version;
    state.header.gpu_caps = state.regs.base.caps;
    state.header.error_status = state.regs.base.status;
    state.header.error_code = state.error.error_code;
    state.header.reset_count = atomic_read(&mdev->reset_count);
    state.header.total_size = total_size;
    strncpy(state.header.kernel_version, utsname()->release,
            sizeof(state.header.kernel_version) - 1);
    snprintf(state.header.driver_version, sizeof(state.header.driver_version),
             "%d.%d", DRIVER_MAJOR, DRIVER_MINOR);
    
    /* Write header */
    mgpu_coredump_append(&state, &state.header, sizeof(state.header));
    
    /* Reserve space for section headers */
    sections = (struct mgpu_coredump_section_header *)(state.data + state.offset);
    state.offset += sizeof(struct mgpu_coredump_section_header) * MGPU_DUMP_END;
    
    /* Write sections */
    
    /* Register dump */
    sections[num_sections].type = MGPU_DUMP_REGISTERS;
    sections[num_sections].offset = state.offset;
    sections[num_sections].size = sizeof(struct mgpu_register_dump);
    strncpy(sections[num_sections].name, "registers", 31);
    mgpu_coredump_append(&state, &state.regs, sizeof(state.regs));
    num_sections++;
    
    /* Command ring dump */
    sections[num_sections].type = MGPU_DUMP_COMMAND_RING;
    sections[num_sections].offset = state.offset;
    sections[num_sections].size = mgpu_coredump_capture_cmdring(&state, 
                                                                 state.data + state.offset, 0);
    strncpy(sections[num_sections].name, "command_ring", 31);
    state.offset += sections[num_sections].size;
    num_sections++;
    
    /* Shader state */
    sections[num_sections].type = MGPU_DUMP_SHADER_STATE;
    sections[num_sections].offset = state.offset;
    sections[num_sections].size = sizeof(struct mgpu_shader_dump);
    strncpy(sections[num_sections].name, "shaders", 31);
    mgpu_coredump_append(&state, &state.shaders, sizeof(state.shaders));
    num_sections++;
    
    /* Instruction memory */
    sections[num_sections].type = MGPU_DUMP_INSTR_MEM;
    sections[num_sections].offset = state.offset;
    sections[num_sections].size = mgpu_coredump_capture_instrmem(&state,
                                                                  state.data + state.offset);
    strncpy(sections[num_sections].name, "instruction_memory", 31);
    state.offset += sections[num_sections].size;
    num_sections++;
    
    /* Error info */
    sections[num_sections].type = MGPU_DUMP_ERROR_INFO;
    sections[num_sections].offset = state.offset;
    sections[num_sections].size = sizeof(struct mgpu_error_dump);
    strncpy(sections[num_sections].name, "error_info", 31);
    mgpu_coredump_append(&state, &state.error, sizeof(state.error));
    num_sections++;
    
    /* Update header with section count */
    ((struct mgpu_coredump_header *)dump_data)->num_sections = num_sections;
    
    *dump_size = state.offset;
    
    return dump_data;
}

/* Trigger coredump capture */
void mgpu_coredump_capture(struct mgpu_device *mdev, const char *reason)
{
    void *dump_data;
    size_t dump_size;
    
    dev_warn(mdev->dev, "Capturing GPU coredump: %s\n", reason ?: "unknown");
    
    /* Create coredump */
    dump_data = mgpu_coredump_create(mdev, &dump_size);
    if (!dump_data) {
        dev_err(mdev->dev, "Failed to create coredump\n");
        return;
    }
    
    /* Submit to devcoredump subsystem */
    dev_coredumpv(mdev->dev, dump_data, dump_size, GFP_KERNEL);
    
    /* Note: devcoredump will free dump_data */
    
    dev_info(mdev->dev, "GPU coredump saved (%zu bytes)\n", dump_size);
}

/* Parse and print coredump (for debugging) */
void mgpu_coredump_print(struct mgpu_device *mdev, const void *data, size_t size)
{
    const struct mgpu_coredump_header *header = data;
    const struct mgpu_coredump_section_header *sections;
    const struct mgpu_register_dump *regs;
    const struct mgpu_error_dump *error;
    int i;
    
    if (size < sizeof(*header) || header->magic != MGPU_COREDUMP_MAGIC) {
        dev_err(mdev->dev, "Invalid coredump data\n");
        return;
    }
    
    dev_info(mdev->dev, "=== GPU Coredump ===\n");
    dev_info(mdev->dev, "Version: %u\n", header->version);
    dev_info(mdev->dev, "Timestamp: %llu\n", header->timestamp);
    dev_info(mdev->dev, "GPU Version: 0x%08x\n", header->gpu_version);
    dev_info(mdev->dev, "GPU Caps: 0x%08x\n", header->gpu_caps);
    dev_info(mdev->dev, "Error Status: 0x%08x\n", header->error_status);
    dev_info(mdev->dev, "Reset Count: %u\n", header->reset_count);
    dev_info(mdev->dev, "Kernel: %s\n", header->kernel_version);
    dev_info(mdev->dev, "Driver: %s\n", header->driver_version);
    
    sections = (const struct mgpu_coredump_section_header *)(data + sizeof(*header));
    
    /* Find and print register dump */
    for (i = 0; i < header->num_sections; i++) {
        if (sections[i].type == MGPU_DUMP_REGISTERS) {
            regs = data + sections[i].offset;
            dev_info(mdev->dev, "\n=== Registers ===\n");
            dev_info(mdev->dev, "Control: 0x%08x\n", regs->base.control);
            dev_info(mdev->dev, "Status: 0x%08x\n", regs->base.status);
            dev_info(mdev->dev, "IRQ Status: 0x%08x\n", regs->interrupt.irq_status);
            dev_info(mdev->dev, "CMD Head: 0x%08x\n", regs->command_queue[0].cmd_head);
            dev_info(mdev->dev, "CMD Tail: 0x%08x\n", regs->command_queue[0].cmd_tail);
            dev_info(mdev->dev, "Fence Value: 0x%08x\n", regs->fence.fence_value);
        }
        
        if (sections[i].type == MGPU_DUMP_ERROR_INFO) {
            error = data + sections[i].offset;
            dev_info(mdev->dev, "\n=== Error Info ===\n");
            dev_info(mdev->dev, "Error Code: 0x%08x\n", error->error_code);
            dev_info(mdev->dev, "Hang Detected: %s\n", error->hang_detected ? "Yes" : "No");
            dev_info(mdev->dev, "Description: %s\n", error->description);
        }
    }
    
    dev_info(mdev->dev, "====================\n");
}

/* Initialize coredump subsystem */
int mgpu_coredump_init(struct mgpu_device *mdev)
{
    /* Register with devcoredump is automatic */
    dev_dbg(mdev->dev, "Coredump support initialized\n");
    return 0;
}

/* Cleanup coredump subsystem */
void mgpu_coredump_fini(struct mgpu_device *mdev)
{
    /* Nothing to clean up */
}

MODULE_DESCRIPTION("MGPU Coredump Support");
MODULE_AUTHOR("Rafeed Khan");
MODULE_LICENSE("GPL v2");
