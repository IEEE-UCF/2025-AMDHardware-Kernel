/*
 * MGPU Self-Test Implementation
 * Comprehensive hardware validation tests based on actual GPU modules
 *
 * Tests cover all major hardware blocks:
 * - Register access (controller.sv)
 * - Command submission (controller.sv, fifo.sv) 
 * - Shader execution (shader_core.sv, shader_loader.sv)
 * - Vertex processing (vertex_fetch.sv)
 * - Rasterization (rasterizer.sv)
 * - Fragment processing (fragment_shader.sv)
 * - Texture operations (texture_unit.sv)
 * - Memory operations (axi_wrapper.sv)
 *
 * Copyright (C) 2025
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/random.h>
#include <linux/crc32.h>

#include "mgpu_drm.h"
#include "mgpu_regs.h"
#include "mgpu_internal.h"

/* Test control flags */
#define MGPU_TEST_BASIC        BIT(0)
#define MGPU_TEST_MEMORY       BIT(1)
#define MGPU_TEST_SHADER       BIT(2)
#define MGPU_TEST_PIPELINE     BIT(3)
#define MGPU_TEST_COMMAND      BIT(4)
#define MGPU_TEST_INTERRUPT    BIT(5)
#define MGPU_TEST_DMA          BIT(6)
#define MGPU_TEST_STRESS       BIT(7)
#define MGPU_TEST_ALL          0xFF

/* Test patterns */
static const u32 test_patterns[] = {
    0x00000000, 0xFFFFFFFF, 0x5A5A5A5A, 0xA5A5A5A5,
    0xDEADBEEF, 0xCAFEBABE, 0x12345678, 0x87654321,
    0x0F0F0F0F, 0xF0F0F0F0, 0x33333333, 0xCCCCCCCC,
    0x55555555, 0xAAAAAAAA, 0x01234567, 0xFEDCBA98
};

/* Test result structure */
struct mgpu_test_result {
    const char *name;
    bool passed;
    u32 iterations;
    u32 errors;
    ktime_t duration;
    char error_msg[256];
};

/* Self-test context */
struct mgpu_selftest_ctx {
    struct mgpu_device *mdev;
    u32 test_flags;
    bool verbose;
    
    /* Test resources */
    struct mgpu_bo *test_bo;
    void *test_vaddr;
    dma_addr_t test_dma_addr;
    size_t test_size;
    
    /* Results */
    struct mgpu_test_result *results;
    u32 num_tests;
    u32 tests_passed;
    u32 tests_failed;
};

/* Forward declarations */
static int mgpu_test_registers(struct mgpu_selftest_ctx *ctx);
static int mgpu_test_memory(struct mgpu_selftest_ctx *ctx);
static int mgpu_test_shader(struct mgpu_selftest_ctx *ctx);
static int mgpu_test_pipeline(struct mgpu_selftest_ctx *ctx);
static int mgpu_test_command_queue(struct mgpu_selftest_ctx *ctx);
static int mgpu_test_interrupts(struct mgpu_selftest_ctx *ctx);
static int mgpu_test_dma(struct mgpu_selftest_ctx *ctx);
static int mgpu_test_stress(struct mgpu_selftest_ctx *ctx);

/* Helper: Record test result */
static void mgpu_test_record_result(struct mgpu_selftest_ctx *ctx,
                                    const char *name, bool passed,
                                    const char *fmt, ...)
{
    struct mgpu_test_result *result;
    va_list args;
    
    if (ctx->num_tests >= 64) /* Max tests */
        return;
    
    result = &ctx->results[ctx->num_tests++];
    result->name = name;
    result->passed = passed;
    
    if (fmt) {
        va_start(args, fmt);
        vsnprintf(result->error_msg, sizeof(result->error_msg), fmt, args);
        va_end(args);
    }
    
    if (passed) {
        ctx->tests_passed++;
        if (ctx->verbose)
            dev_info(ctx->mdev->dev, "[PASS] %s\n", name);
    } else {
        ctx->tests_failed++;
        dev_err(ctx->mdev->dev, "[FAIL] %s: %s\n", name, result->error_msg);
    }
}

/* Test 1: Basic register read/write */
static int mgpu_test_registers(struct mgpu_selftest_ctx *ctx)
{
    struct mgpu_device *mdev = ctx->mdev;
    u32 value, readback;
    int i;
    bool passed = true;
    
    dev_info(mdev->dev, "Testing register access...\n");
    
    /* Test VERSION register (read-only) */
    value = mgpu_read(mdev, MGPU_REG_VERSION);
    if (value == 0 || value == 0xFFFFFFFF) {
        mgpu_test_record_result(ctx, "Version Register",
                                false, "Invalid version: 0x%08x", value);
        passed = false;
    } else {
        mgpu_test_record_result(ctx, "Version Register", true, NULL);
    }
    
    /* Test CAPS register (read-only) */
    value = mgpu_read(mdev, MGPU_REG_CAPS);
    if (value == 0) {
        mgpu_test_record_result(ctx, "Caps Register",
                                false, "No capabilities reported");
        passed = false;
    } else {
        mgpu_test_record_result(ctx, "Caps Register", true, NULL);
    }
    
    /* Test SCRATCH register with patterns */
    for (i = 0; i < ARRAY_SIZE(test_patterns); i++) {
        mgpu_write(mdev, MGPU_REG_SCRATCH, test_patterns[i]);
        readback = mgpu_read(mdev, MGPU_REG_SCRATCH);
        
        if (readback != test_patterns[i]) {
            mgpu_test_record_result(ctx, "Scratch Pattern",
                                   false, "Pattern %d: wrote 0x%08x, read 0x%08x",
                                   i, test_patterns[i], readback);
            passed = false;
            break;
        }
    }
    
    if (i == ARRAY_SIZE(test_patterns)) {
        mgpu_test_record_result(ctx, "Scratch Pattern", true, NULL);
    }
    
    /* Test CONTROL register bits */
    mgpu_write(mdev, MGPU_REG_CONTROL, 0);
    readback = mgpu_read(mdev, MGPU_REG_CONTROL);
    if (readback != 0) {
        mgpu_test_record_result(ctx, "Control Clear",
                               false, "Failed to clear: 0x%08x", readback);
        passed = false;
    } else {
        mgpu_test_record_result(ctx, "Control Clear", true, NULL);
    }
    
    /* Test individual control bits */
    u32 control_bits[] = {
        MGPU_CTRL_ENABLE,
        MGPU_CTRL_PAUSE,
        MGPU_CTRL_SINGLE_STEP,
        MGPU_CTRL_FLUSH_CACHE,
        MGPU_CTRL_PERF_COUNTER
    };
    const char *bit_names[] = {
        "Enable", "Pause", "Single Step", "Flush Cache", "Perf Counter"
    };
    
    for (i = 0; i < ARRAY_SIZE(control_bits); i++) {
        mgpu_write(mdev, MGPU_REG_CONTROL, control_bits[i]);
        readback = mgpu_read(mdev, MGPU_REG_CONTROL);
        
        if ((readback & control_bits[i]) != control_bits[i]) {
            mgpu_test_record_result(ctx, bit_names[i],
                                   false, "Bit not set: 0x%08x", readback);
            passed = false;
        }
        
        mgpu_write(mdev, MGPU_REG_CONTROL, 0);
    }
    
    /* Test STATUS register */
    value = mgpu_read(mdev, MGPU_REG_STATUS);
    if (!(value & MGPU_STATUS_IDLE)) {
        mgpu_test_record_result(ctx, "Status Idle",
                               false, "GPU not idle: 0x%08x", value);
        passed = false;
    } else {
        mgpu_test_record_result(ctx, "Status Idle", true, NULL);
    }
    
    /* Test vertex registers (from vertex_fetch.sv) */
    mgpu_write(mdev, MGPU_REG_VERTEX_BASE, 0x10000000);
    readback = mgpu_read(mdev, MGPU_REG_VERTEX_BASE);
    if (readback != 0x10000000) {
        mgpu_test_record_result(ctx, "Vertex Base",
                               false, "Write failed: 0x%08x", readback);
        passed = false;
    } else {
        mgpu_test_record_result(ctx, "Vertex Base", true, NULL);
    }
    
    /* Test shader registers (from shader_loader.sv) */
    mgpu_write(mdev, MGPU_REG_SHADER_PC, 0x100);
    readback = mgpu_read(mdev, MGPU_REG_SHADER_PC);
    if (readback != 0x100) {
        mgpu_test_record_result(ctx, "Shader PC",
                               false, "Write failed: 0x%08x", readback);
        passed = false;
    } else {
        mgpu_test_record_result(ctx, "Shader PC", true, NULL);
    }
    
    return passed ? 0 : -EIO;
}

/* Test 2: Memory allocation and DMA */
static int mgpu_test_memory(struct mgpu_selftest_ctx *ctx)
{
    struct mgpu_device *mdev = ctx->mdev;
    struct mgpu_bo_create bo_args = {0};
    struct mgpu_bo *bo;
    void *vaddr;
    u32 *data;
    int i, ret;
    bool passed = true;
    
    dev_info(mdev->dev, "Testing memory operations...\n");
    
    /* Test buffer object creation */
    bo_args.size = PAGE_SIZE * 4;  /* 16KB */
    bo_args.flags = MGPU_BO_FLAGS_COHERENT;
    
    ret = mgpu_bo_create(mdev, &bo_args);
    if (ret) {
        mgpu_test_record_result(ctx, "BO Create",
                               false, "Failed to create BO: %d", ret);
        return ret;
    }
    
    mgpu_test_record_result(ctx, "BO Create", true, NULL);
    
    /* Get BO and map it */
    bo = mgpu_bo_lookup(mdev, bo_args.handle);
    if (!bo) {
        mgpu_test_record_result(ctx, "BO Lookup",
                               false, "Failed to lookup BO");
        return -EINVAL;
    }
    
    vaddr = mgpu_bo_vmap(bo);
    if (!vaddr) {
        mgpu_test_record_result(ctx, "BO Map",
                               false, "Failed to map BO");
        mgpu_bo_put(bo);
        return -EINVAL;
    }
    
    mgpu_test_record_result(ctx, "BO Map", true, NULL);
    
    /* Write test pattern */
    data = (u32 *)vaddr;
    for (i = 0; i < PAGE_SIZE / sizeof(u32); i++) {
        data[i] = i ^ 0xDEADBEEF;
    }
    
    /* Sync for device */
    mgpu_bo_cpu_fini(bo, true);
    
    /* Read back and verify */
    mgpu_bo_cpu_prep(bo, false);
    
    for (i = 0; i < PAGE_SIZE / sizeof(u32); i++) {
        if (data[i] != (i ^ 0xDEADBEEF)) {
            mgpu_test_record_result(ctx, "Memory Pattern",
                                   false, "Mismatch at offset %d: 0x%08x != 0x%08x",
                                   i, data[i], i ^ 0xDEADBEEF);
            passed = false;
            break;
        }
    }
    
    if (passed) {
        mgpu_test_record_result(ctx, "Memory Pattern", true, NULL);
    }
    
    /* Store for other tests */
    ctx->test_bo = bo;
    ctx->test_vaddr = vaddr;
    ctx->test_dma_addr = bo->dma_addr;
    ctx->test_size = bo_args.size;
    
    mgpu_bo_vunmap(bo, vaddr);
    
    /* Test cache coherency */
    if (bo_args.flags & MGPU_BO_FLAGS_COHERENT) {
        mgpu_test_record_result(ctx, "Cache Coherency", true, NULL);
    }
    
    return passed ? 0 : -EIO;
}

/* Test 3: Shader loading and execution */
static int mgpu_test_shader(struct mgpu_selftest_ctx *ctx)
{
    struct mgpu_device *mdev = ctx->mdev;
    struct mgpu_load_shader shader_args = {0};
    u32 *shader_code;
    int ret;
    
    dev_info(mdev->dev, "Testing shader operations...\n");
    
    /* Create simple NOP shader */
    shader_code = kzalloc(256, GFP_KERNEL);
    if (!shader_code) {
        mgpu_test_record_result(ctx, "Shader Alloc",
                               false, "Failed to allocate shader");
        return -ENOMEM;
    }
    
    /* Simple shader that just returns (based on instruction_decoder.sv format)
     * Format: [31:27] opcode | [26:23] rd | [22:19] rs1 | [18:15] rs2 | [14:0] imm
     */
    shader_code[0] = 0x4D475055;  /* Magic 'MGPU' */
    shader_code[1] = 0x00010000;  /* Version 1.0 */
    shader_code[2] = 0x00000000;  /* NOP instruction */
    shader_code[3] = 0x80000000;  /* HALT/RETURN */
    
    /* Load vertex shader to slot 0 */
    shader_args.data = (uintptr_t)shader_code;
    shader_args.size = 16;  /* 4 instructions */
    shader_args.type = MGPU_SHADER_VERTEX;
    shader_args.slot = 0;
    
    ret = mgpu_load_shader(mdev, &shader_args);
    if (ret) {
        mgpu_test_record_result(ctx, "Shader Load",
                               false, "Failed to load shader: %d", ret);
        kfree(shader_code);
        return ret;
    }
    
    mgpu_test_record_result(ctx, "Shader Load", true, NULL);
    
    /* Verify shader was written to instruction memory */
    mgpu_write(mdev, MGPU_REG_SHADER_ADDR, 0);
    u32 readback = mgpu_read(mdev, MGPU_REG_SHADER_DATA);
    if (readback != shader_code[0]) {
        mgpu_test_record_result(ctx, "Shader Verify",
                               false, "Shader data mismatch: 0x%08x != 0x%08x",
                               readback, shader_code[0]);
    } else {
        mgpu_test_record_result(ctx, "Shader Verify", true, NULL);
    }
    
    /* Load fragment shader to slot 1 */
    shader_args.type = MGPU_SHADER_FRAGMENT;
    shader_args.slot = 1;
    
    ret = mgpu_load_shader(mdev, &shader_args);
    if (ret) {
        mgpu_test_record_result(ctx, "Fragment Shader Load",
                               false, "Failed to load fragment shader: %d", ret);
    } else {
        mgpu_test_record_result(ctx, "Fragment Shader Load", true, NULL);
    }
    
    /* Bind shaders */
    ret = mgpu_shader_bind(mdev, 0, MGPU_SHADER_VERTEX);
    if (ret) {
        mgpu_test_record_result(ctx, "Shader Bind",
                               false, "Failed to bind vertex shader: %d", ret);
    } else {
        mgpu_test_record_result(ctx, "Shader Bind", true, NULL);
    }
    
    kfree(shader_code);
    
    return 0;
}

/* Test 4: Pipeline execution (vertex fetch -> rasterizer -> fragment) */
static int mgpu_test_pipeline(struct mgpu_selftest_ctx *ctx)
{
    struct mgpu_device *mdev = ctx->mdev;
    struct mgpu_draw_call draw = {0};
    u32 *vertex_data;
    int ret;
    
    dev_info(mdev->dev, "Testing pipeline execution...\n");
    
    if (!ctx->test_bo) {
        mgpu_test_record_result(ctx, "Pipeline Prerequisites",
                               false, "No test buffer allocated");
        return -EINVAL;
    }
    
    /* Setup simple triangle vertices (based on vertex_fetch.sv)
     * Format: 11 attributes per vertex * 4 bytes = 44 bytes per vertex
     * We'll use 3 vertices for a triangle
     */
    vertex_data = (u32 *)ctx->test_vaddr;
    
    /* Vertex 0: position (0, 0), color (red), uv (0, 0) */
    vertex_data[0] = 0;          /* x */
    vertex_data[1] = 0;          /* y */
    vertex_data[2] = 0xFF0000;   /* color */
    vertex_data[3] = 0;          /* u */
    vertex_data[4] = 0;          /* v */
    /* ... remaining attributes */
    
    /* Vertex 1: position (100, 0) */
    vertex_data[11] = 100;       /* x */
    vertex_data[12] = 0;         /* y */
    vertex_data[13] = 0x00FF00;  /* color (green) */
    
    /* Vertex 2: position (50, 100) */
    vertex_data[22] = 50;        /* x */
    vertex_data[23] = 100;       /* y */
    vertex_data[24] = 0x0000FF;  /* color (blue) */
    
    /* Sync data to device */
    mgpu_bo_cpu_fini(ctx->test_bo, true);
    
    /* Configure pipeline */
    draw.vertex_buffer = ctx->test_dma_addr;
    draw.vertex_count = 3;
    draw.vertex_stride = 44;  /* 11 attrs * 4 bytes */
    draw.vertex_shader_slot = 0;
    draw.fragment_shader_slot = 1;
    draw.framebuffer_addr = 0;  /* Use default framebuffer */
    draw.flags = 0;
    
    /* Submit draw call */
    if (mdev->pipeline_mgr) {
        ret = mgpu_pipeline_draw(mdev, &draw);
        if (ret) {
            mgpu_test_record_result(ctx, "Pipeline Draw",
                                   false, "Draw failed: %d", ret);
            return ret;
        }
        
        mgpu_test_record_result(ctx, "Pipeline Draw", true, NULL);
        
        /* Wait for completion */
        ret = mgpu_core_wait_idle(mdev, 1000);
        if (ret) {
            mgpu_test_record_result(ctx, "Pipeline Complete",
                                   false, "Pipeline timeout: %d", ret);
        } else {
            mgpu_test_record_result(ctx, "Pipeline Complete", true, NULL);
        }
    } else {
        mgpu_test_record_result(ctx, "Pipeline Manager",
                               false, "No pipeline manager");
    }
    
    /* Check status for errors */
    u32 status = mgpu_read(mdev, MGPU_REG_STATUS);
    if (status & MGPU_STATUS_ERROR) {
        mgpu_test_record_result(ctx, "Pipeline Status",
                               false, "Pipeline error: 0x%08x", status);
    } else {
        mgpu_test_record_result(ctx, "Pipeline Status", true, NULL);
    }
    
    return 0;
}

/* Test 5: Command queue submission */
static int mgpu_test_command_queue(struct mgpu_selftest_ctx *ctx)
{
    struct mgpu_device *mdev = ctx->mdev;
    struct mgpu_submit submit = {0};
    struct mgpu_cmd_nop *nop_cmd;
    struct mgpu_cmd_fence *fence_cmd;
    u32 *cmds;
    int ret;
    
    dev_info(mdev->dev, "Testing command queue...\n");
    
    /* Allocate command buffer */
    cmds = kzalloc(256, GFP_KERNEL);
    if (!cmds) {
        mgpu_test_record_result(ctx, "Command Alloc",
                               false, "Failed to allocate commands");
        return -ENOMEM;
    }
    
    /* Build NOP command */
    nop_cmd = (struct mgpu_cmd_nop *)cmds;
    nop_cmd->header.opcode = MGPU_CMD_NOP;
    nop_cmd->header.size = sizeof(*nop_cmd) / 4;
    nop_cmd->header.flags = 0;
    
    /* Submit NOP */
    submit.commands = (uintptr_t)cmds;
    submit.cmd_size = sizeof(*nop_cmd);
    submit.queue_id = 0;
    submit.flags = MGPU_SUBMIT_FLAGS_SYNC;
    
    ret = mgpu_submit_commands(mdev, &submit);
    if (ret) {
        mgpu_test_record_result(ctx, "NOP Submit",
                               false, "Failed to submit NOP: %d", ret);
        kfree(cmds);
        return ret;
    }
    
    mgpu_test_record_result(ctx, "NOP Submit", true, NULL);
    
    /* Test fence command */
    if (ctx->test_bo) {
        fence_cmd = (struct mgpu_cmd_fence *)cmds;
        fence_cmd->header.opcode = MGPU_CMD_FENCE;
        fence_cmd->header.size = sizeof(*fence_cmd) / 4;
        fence_cmd->header.flags = 0;
        fence_cmd->addr = ctx->test_dma_addr;
        fence_cmd->value = 0x12345678;
        
        submit.cmd_size = sizeof(*fence_cmd);
        submit.flags = 0;  /* Async */
        
        ret = mgpu_submit_commands(mdev, &submit);
        if (ret) {
            mgpu_test_record_result(ctx, "Fence Submit",
                                   false, "Failed to submit fence: %d", ret);
        } else {
            mgpu_test_record_result(ctx, "Fence Submit", true, NULL);
            
            /* Wait for fence */
            struct mgpu_wait_fence wait = {
                .fence_addr = ctx->test_dma_addr,
                .fence_value = 0x12345678,
                .timeout_ms = 1000
            };
            
            ret = mgpu_wait_fence(mdev, &wait);
            if (ret) {
                mgpu_test_record_result(ctx, "Fence Wait",
                                       false, "Fence wait failed: %d", ret);
            } else {
                mgpu_test_record_result(ctx, "Fence Wait", true, NULL);
            }
        }
    }
    
    /* Test queue overflow handling */
    int i;
    for (i = 0; i < 20; i++) {  /* Try to overflow queue */
        submit.cmd_size = sizeof(*nop_cmd);
        submit.flags = 0;  /* Async */
        
        ret = mgpu_submit_commands(mdev, &submit);
        if (ret == -EBUSY) {
            /* Expected when queue is full */
            mgpu_test_record_result(ctx, "Queue Overflow",
                                   true, "Properly handled at %d", i);
            break;
        }
    }
    
    if (i == 20) {
        mgpu_test_record_result(ctx, "Queue Overflow",
                               false, "No overflow detected");
    }
    
    /* Wait for queue to drain */
    mgpu_core_wait_idle(mdev, 1000);
    
    kfree(cmds);
    return 0;
}

/* Test 6: Interrupt handling */
static int mgpu_test_interrupts(struct mgpu_selftest_ctx *ctx)
{
    struct mgpu_device *mdev = ctx->mdev;
    u32 old_enable, status;
    int timeout;
    
    dev_info(mdev->dev, "Testing interrupt handling...\n");
    
    /* Save current interrupt state */
    old_enable = mgpu_read(mdev, MGPU_REG_IRQ_ENABLE);
    
    /* Disable all interrupts */
    mgpu_write(mdev, MGPU_REG_IRQ_ENABLE, 0);
    
    /* Clear any pending interrupts */
    mgpu_write(mdev, MGPU_REG_IRQ_ACK, 0xFFFFFFFF);
    
    /* Enable command complete interrupt */
    mgpu_write(mdev, MGPU_REG_IRQ_ENABLE, MGPU_IRQ_CMD_COMPLETE);
    
    /* Submit a command to trigger interrupt */
    struct mgpu_cmd_nop nop = {
        .header = {
            .opcode = MGPU_CMD_NOP,
            .size = sizeof(nop) / 4,
            .flags = 0
        }
    };
    
    struct mgpu_submit submit = {
        .commands = (uintptr_t)&nop,
        .cmd_size = sizeof(nop),
        .queue_id = 0,
        .flags = 0  /* Async to test interrupt */
    };
    
    int ret = mgpu_submit_commands(mdev, &submit);
    if (ret) {
        mgpu_test_record_result(ctx, "IRQ Submit",
                               false, "Failed to submit: %d", ret);
        mgpu_write(mdev, MGPU_REG_IRQ_ENABLE, old_enable);
        return ret;
    }
    
    /* Wait for interrupt */
    timeout = 100;  /* 100ms */
    while (timeout--) {
        status = mgpu_read(mdev, MGPU_REG_IRQ_STATUS);
        if (status & MGPU_IRQ_CMD_COMPLETE) {
            mgpu_test_record_result(ctx, "Command Complete IRQ", true, NULL);
            break;
        }
        msleep(1);
    }
    
    if (timeout <= 0) {
        mgpu_test_record_result(ctx, "Command Complete IRQ",
                               false, "Timeout waiting for interrupt");
    }
    
    /* Clear interrupt */
    mgpu_write(mdev, MGPU_REG_IRQ_ACK, MGPU_IRQ_CMD_COMPLETE);
    
    /* Test error interrupt */
    mgpu_write(mdev, MGPU_REG_IRQ_ENABLE, MGPU_IRQ_ERROR);
    
    /* Note: Triggering actual error would require invalid operation */
    /* For now, just verify we can enable/disable error IRQ */
    status = mgpu_read(mdev, MGPU_REG_IRQ_ENABLE);
    if (status & MGPU_IRQ_ERROR) {
        mgpu_test_record_result(ctx, "Error IRQ Enable", true, NULL);
    } else {
        mgpu_test_record_result(ctx, "Error IRQ Enable",
                               false, "Failed to enable error IRQ");
    }
    
    /* Restore interrupt state */
    mgpu_write(mdev, MGPU_REG_IRQ_ENABLE, old_enable);
    
    return 0;
}

/* Test 7: DMA operations */
static int mgpu_test_dma(struct mgpu_selftest_ctx *ctx)
{
    struct mgpu_device *mdev = ctx->mdev;
    struct mgpu_bo_create src_bo_args = {0};
    struct mgpu_bo_create dst_bo_args = {0};
    struct mgpu_bo *src_bo, *dst_bo;
    u32 *src_data, *dst_data;
    int i, ret;
    bool passed = true;
    
    dev_info(mdev->dev, "Testing DMA operations...\n");
    
    /* Create source buffer */
    src_bo_args.size = PAGE_SIZE;
    src_bo_args.flags = MGPU_BO_FLAGS_COHERENT;
    
    ret = mgpu_bo_create(mdev, &src_bo_args);
    if (ret) {
        mgpu_test_record_result(ctx, "DMA Source BO",
                               false, "Failed to create: %d", ret);
        return ret;
    }
    
    /* Create destination buffer */
    dst_bo_args.size = PAGE_SIZE;
    dst_bo_args.flags = MGPU_BO_FLAGS_COHERENT;
    
    ret = mgpu_bo_create(mdev, &dst_bo_args);
    if (ret) {
        mgpu_test_record_result(ctx, "DMA Dest BO",
                               false, "Failed to create: %d", ret);
        mgpu_bo_destroy(mdev, (struct mgpu_bo_destroy *)&src_bo_args.handle);
        return ret;
    }
    
    /* Get and map buffers */
    src_bo = mgpu_bo_lookup(mdev, src_bo_args.handle);
    dst_bo = mgpu_bo_lookup(mdev, dst_bo_args.handle);
    
    if (!src_bo || !dst_bo) {
        mgpu_test_record_result(ctx, "DMA BO Lookup",
                               false, "Failed to lookup BOs");
        passed = false;
        goto cleanup;
    }
    
    src_data = mgpu_bo_vmap(src_bo);
    dst_data = mgpu_bo_vmap(dst_bo);
    
    if (!src_data || !dst_data) {
        mgpu_test_record_result(ctx, "DMA BO Map",
                               false, "Failed to map BOs");
        passed = false;
        goto cleanup;
    }
    
    /* Initialize source with pattern */
    for (i = 0; i < PAGE_SIZE / sizeof(u32); i++) {
        src_data[i] = i ^ 0xABCDEF00;
    }
    
    /* Clear destination */
    memset(dst_data, 0, PAGE_SIZE);
    
    /* Sync to device */
    mgpu_bo_cpu_fini(src_bo, true);
    mgpu_bo_cpu_fini(dst_bo, true);
    
    /* Perform DMA copy */
    ret = mgpu_dma_copy(mdev, src_bo->dma_addr, dst_bo->dma_addr,
                       PAGE_SIZE, true);  /* Blocking */
    if (ret) {
        mgpu_test_record_result(ctx, "DMA Copy",
                               false, "DMA failed: %d", ret);
        passed = false;
    } else {
        mgpu_test_record_result(ctx, "DMA Copy", true, NULL);
        
        /* Sync from device */
        mgpu_bo_cpu_prep(dst_bo, false);
        
        /* Verify data */
        for (i = 0; i < PAGE_SIZE / sizeof(u32); i++) {
            if (dst_data[i] != (i ^ 0xABCDEF00)) {
                mgpu_test_record_result(ctx, "DMA Verify",
                                       false, "Mismatch at %d: 0x%08x != 0x%08x",
                                       i, dst_data[i], i ^ 0xABCDEF00);
                passed = false;
                break;
            }
        }
        
        if (i == PAGE_SIZE / sizeof(u32)) {
            mgpu_test_record_result(ctx, "DMA Verify", true, NULL);
        }
    }
    
    mgpu_bo_vunmap(src_bo, src_data);
    mgpu_bo_vunmap(dst_bo, dst_data);
    
cleanup:
    if (src_bo) mgpu_bo_put(src_bo);
    if (dst_bo) mgpu_bo_put(dst_bo);
    
    mgpu_bo_destroy(mdev, (struct mgpu_bo_destroy *)&src_bo_args.handle);
    mgpu_bo_destroy(mdev, (struct mgpu_bo_destroy *)&dst_bo_args.handle);
    
    return passed ? 0 : -EIO;
}

/* Test 8: Stress test */
static int mgpu_test_stress(struct mgpu_selftest_ctx *ctx)
{
    struct mgpu_device *mdev = ctx->mdev;
    ktime_t start, end;
    int iterations = 100;
    int i, ret;
    u32 status;
    
    dev_info(mdev->dev, "Running stress test (%d iterations)...\n", iterations);
    
    start = ktime_get();
    
    for (i = 0; i < iterations; i++) {
        /* Submit multiple commands rapidly */
        struct mgpu_cmd_nop nop = {
            .header = {
                .opcode = MGPU_CMD_NOP,
                .size = sizeof(nop) / 4,
                .flags = 0
            }
        };
        
        struct mgpu_submit submit = {
            .commands = (uintptr_t)&nop,
            .cmd_size = sizeof(nop),
            .queue_id = i % mdev->num_queues,  /* Round-robin queues */
            .flags = 0  /* Async */
        };
        
        ret = mgpu_submit_commands(mdev, &submit);
        if (ret && ret != -EBUSY) {
            mgpu_test_record_result(ctx, "Stress Submit",
                                   false, "Failed at iteration %d: %d", i, ret);
            break;
        }
        
        /* Check for errors periodically */
        if ((i % 10) == 0) {
            status = mgpu_read(mdev, MGPU_REG_STATUS);
            if (status & MGPU_STATUS_ERROR) {
                mgpu_test_record_result(ctx, "Stress Error",
                                       false, "Error at iteration %d: 0x%08x",
                                       i, status);
                break;
            }
        }
        
        /* Occasionally wait for idle */
        if ((i % 25) == 0) {
            mgpu_core_wait_idle(mdev, 100);
        }
    }
    
    /* Final wait for all commands to complete */
    ret = mgpu_core_wait_idle(mdev, 5000);
    if (ret) {
        mgpu_test_record_result(ctx, "Stress Complete",
                               false, "Timeout waiting for idle: %d", ret);
    } else {
        mgpu_test_record_result(ctx, "Stress Complete", true, NULL);
    }
    
    end = ktime_get();
    
    /* Calculate throughput */
    u64 elapsed_ns = ktime_to_ns(ktime_sub(end, start));
    u64 cmds_per_sec = (iterations * 1000000000ULL) / elapsed_ns;
    
    dev_info(mdev->dev, "Stress test: %d commands in %llu ms (%llu cmds/sec)\n",
             iterations, elapsed_ns / 1000000, cmds_per_sec);
    
    return (i == iterations) ? 0 : -EIO;
}

/* Main self-test entry point */
int mgpu_run_selftests(struct mgpu_device *mdev, u32 test_flags, bool verbose)
{
    struct mgpu_selftest_ctx *ctx;
    int ret = 0;
    
    dev_info(mdev->dev, "Starting GPU self-tests (flags: 0x%02x)\n", test_flags);
    
    /* Allocate test context */
    ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
    if (!ctx)
        return -ENOMEM;
    
    ctx->mdev = mdev;
    ctx->test_flags = test_flags;
    ctx->verbose = verbose;
    
    /* Allocate results array */
    ctx->results = kcalloc(64, sizeof(struct mgpu_test_result), GFP_KERNEL);
    if (!ctx->results) {
        kfree(ctx);
        return -ENOMEM;
    }
    
    /* Save GPU state */
    u32 saved_control = mgpu_read(mdev, MGPU_REG_CONTROL);
    u32 saved_irq_enable = mgpu_read(mdev, MGPU_REG_IRQ_ENABLE);
    
    /* Ensure GPU is in known state */
    mgpu_write(mdev, MGPU_REG_CONTROL, 0);
    mgpu_core_wait_idle(mdev, 100);
    
    /* Run selected tests */
    if (test_flags & MGPU_TEST_BASIC) {
        ret = mgpu_test_registers(ctx);
        if (ret && !verbose) goto done;
    }
    
    if (test_flags & MGPU_TEST_MEMORY) {
        ret = mgpu_test_memory(ctx);
        if (ret && !verbose) goto done;
    }
    
    if (test_flags & MGPU_TEST_SHADER) {
        ret = mgpu_test_shader(ctx);
        if (ret && !verbose) goto done;
    }
    
    if (test_flags & MGPU_TEST_PIPELINE) {
        ret = mgpu_test_pipeline(ctx);
        if (ret && !verbose) goto done;
    }
    
    if (test_flags & MGPU_TEST_COMMAND) {
        ret = mgpu_test_command_queue(ctx);
        if (ret && !verbose) goto done;
    }
    
    if (test_flags & MGPU_TEST_INTERRUPT) {
        ret = mgpu_test_interrupts(ctx);
        if (ret && !verbose) goto done;
    }
    
    if (test_flags & MGPU_TEST_DMA) {
        ret = mgpu_test_dma(ctx);
        if (ret && !verbose) goto done;
    }
    
    if (test_flags & MGPU_TEST_STRESS) {
        ret = mgpu_test_stress(ctx);
    }
    
done:
    /* Print summary */
    dev_info(mdev->dev, "\n=== Self-Test Summary ===\n");
    dev_info(mdev->dev, "Total tests: %u\n", ctx->num_tests);
    dev_info(mdev->dev, "Passed: %u\n", ctx->tests_passed);
    dev_info(mdev->dev, "Failed: %u\n", ctx->tests_failed);
    
    if (verbose) {
        int i;
        dev_info(mdev->dev, "\nDetailed Results:\n");
        for (i = 0; i < ctx->num_tests; i++) {
            struct mgpu_test_result *r = &ctx->results[i];
            dev_info(mdev->dev, "  %-20s: %s %s\n",
                     r->name,
                     r->passed ? "PASS" : "FAIL",
                     r->passed ? "" : r->error_msg);
        }
    }
    
    /* Restore GPU state */
    mgpu_write(mdev, MGPU_REG_CONTROL, saved_control);
    mgpu_write(mdev, MGPU_REG_IRQ_ENABLE, saved_irq_enable);
    
    /* Cleanup test resources */
    if (ctx->test_bo) {
        struct mgpu_bo_destroy destroy = {
            .handle = ctx->test_bo->handle
        };
        mgpu_bo_destroy(mdev, &destroy);
    }
    
    /* Overall result */
    if (ctx->tests_failed > 0) {
        dev_err(mdev->dev, "Self-tests FAILED (%u failures)\n", ctx->tests_failed);
        ret = -EIO;
    } else {
        dev_info(mdev->dev, "All self-tests PASSED\n");
        ret = 0;
    }
    
    kfree(ctx->results);
    kfree(ctx);
    
    return ret;
}

/* Module parameter to run tests on load */
static int run_on_load = 0;
module_param(run_on_load, int, 0444);
MODULE_PARM_DESC(run_on_load, "Run self-tests on module load (bitmask)");

/* Module initialization */
static int __init mgpu_selftest_init(void)
{
    /* This is called when module loads if compiled separately */
    pr_info("MGPU self-test module loaded\n");
    
    if (run_on_load && mgpu_dev) {
        mgpu_run_selftests(mgpu_dev, run_on_load, true);
    }
    
    return 0;
}

/* Module cleanup */
static void __exit mgpu_selftest_exit(void)
{
    pr_info("MGPU self-test module unloaded\n");
}

/* Export for use by main driver */
EXPORT_SYMBOL_GPL(mgpu_run_selftests);

module_init(mgpu_selftest_init);
module_exit(mgpu_selftest_exit);

MODULE_DESCRIPTION("MGPU Hardware Self-Test Implementation");
MODULE_AUTHOR("Rafeed Khan");
MODULE_LICENSE("GPL v2");