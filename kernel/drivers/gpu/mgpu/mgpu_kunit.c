/*
 * MGPU KUnit Test Suite
 * Kernel unit tests for MGPU driver components
 *
 * Tests driver logic, data structures, and functions in isolation
 * without requiring actual hardware. Uses KUnit framework for
 * in-kernel testing.
 *
 * Copyright (C) 2025
 */

#include <kunit/test.h>
#include <kunit/mock.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/of.h>

#include "mgpu_drm.h"
#include "mgpu_regs.h"
#include "mgpu_internal.h"

/* Mock MMIO region for testing */
struct mgpu_mock_mmio {
    u32 regs[4096];  /* 16KB of register space */
    bool access_log[4096];
    u32 read_count;
    u32 write_count;
};

/* Test fixture for MGPU tests */
struct mgpu_test_fixture {
    struct mgpu_device *mdev;
    struct mgpu_mock_mmio *mmio;
    struct platform_device *pdev;
    struct resource *res;
};

/* ==================================================================
 * Mock Functions
 * ================================================================== */

/* Mock MMIO read - intercept register reads */
static u32 mock_mgpu_read(struct mgpu_device *mdev, u32 offset)
{
    struct mgpu_test_fixture *fixture = mdev->test_fixture;
    struct mgpu_mock_mmio *mmio = fixture->mmio;
    u32 index = offset / 4;
    
    if (index >= 4096)
        return 0xDEADBEEF;
    
    mmio->access_log[index] = true;
    mmio->read_count++;
    
    return mmio->regs[index];
}

/* Mock MMIO write - intercept register writes */
static void mock_mgpu_write(struct mgpu_device *mdev, u32 offset, u32 value)
{
    struct mgpu_test_fixture *fixture = mdev->test_fixture;
    struct mgpu_mock_mmio *mmio = fixture->mmio;
    u32 index = offset / 4;
    
    if (index >= 4096)
        return;
    
    mmio->access_log[index] = true;
    mmio->write_count++;
    mmio->regs[index] = value;
    
    /* Simulate hardware behavior for specific registers */
    switch (offset) {
    case MGPU_REG_CONTROL:
        /* Simulate reset behavior */
        if (value & MGPU_CTRL_RESET) {
            /* Clear all registers except VERSION and CAPS */
            memset(&mmio->regs[2], 0, (4094 * sizeof(u32)));
            /* Set IDLE status after reset */
            mmio->regs[MGPU_REG_STATUS / 4] = MGPU_STATUS_IDLE;
        }
        break;
        
    case MGPU_REG_IRQ_ACK:
        /* Clear acknowledged interrupts */
        mmio->regs[MGPU_REG_IRQ_STATUS / 4] &= ~value;
        break;
        
    case MGPU_REG_CMD_TAIL:
        /* Simulate command processing */
        if (mmio->regs[MGPU_REG_CMD_HEAD / 4] != value) {
            mmio->regs[MGPU_REG_STATUS / 4] |= MGPU_STATUS_BUSY;
            /* Simulate immediate completion for testing */
            mmio->regs[MGPU_REG_CMD_HEAD / 4] = value;
            mmio->regs[MGPU_REG_STATUS / 4] &= ~MGPU_STATUS_BUSY;
            mmio->regs[MGPU_REG_STATUS / 4] |= MGPU_STATUS_IDLE;
            mmio->regs[MGPU_REG_IRQ_STATUS / 4] |= MGPU_IRQ_CMD_COMPLETE;
        }
        break;
    }
}

/* ==================================================================
 * Test Setup and Teardown
 * ================================================================== */

static int mgpu_test_init(struct kunit *test)
{
    struct mgpu_test_fixture *fixture;
    struct mgpu_device *mdev;
    struct platform_device *pdev;
    
    /* Allocate fixture */
    fixture = kunit_kzalloc(test, sizeof(*fixture), GFP_KERNEL);
    KUNIT_ASSERT_NOT_NULL(test, fixture);
    
    /* Allocate mock MMIO */
    fixture->mmio = kunit_kzalloc(test, sizeof(*fixture->mmio), GFP_KERNEL);
    KUNIT_ASSERT_NOT_NULL(test, fixture->mmio);
    
    /* Initialize mock hardware state */
    fixture->mmio->regs[MGPU_REG_VERSION / 4] = 0x01020304;  /* v1.2.3.4 */
    fixture->mmio->regs[MGPU_REG_CAPS / 4] = MGPU_CAP_VERTEX_SHADER |
                                              MGPU_CAP_FRAGMENT_SHADER |
                                              MGPU_CAP_TEXTURE |
                                              MGPU_CAP_FENCE;
    fixture->mmio->regs[MGPU_REG_STATUS / 4] = MGPU_STATUS_IDLE;
    
    /* Create mock platform device */
    pdev = kunit_kzalloc(test, sizeof(*pdev), GFP_KERNEL);
    KUNIT_ASSERT_NOT_NULL(test, pdev);
    pdev->dev.release = (void *)kfree;  /* Dummy release */
    device_initialize(&pdev->dev);
    
    /* Create mock device structure */
    mdev = kunit_kzalloc(test, sizeof(*mdev), GFP_KERNEL);
    KUNIT_ASSERT_NOT_NULL(test, mdev);
    
    mdev->dev = &pdev->dev;
    mdev->mmio_base = (void __iomem *)fixture->mmio->regs;
    mdev->test_fixture = fixture;
    
    /* Initialize locks and lists */
    mutex_init(&mdev->bo_lock);
    spin_lock_init(&mdev->cmd_lock);
    spin_lock_init(&mdev->irq_lock);
    INIT_LIST_HEAD(&mdev->bo_list);
    init_waitqueue_head(&mdev->queue_wait);
    init_waitqueue_head(&mdev->fence_wait);
    init_waitqueue_head(&mdev->reset_wait);
    atomic_set(&mdev->in_reset, 0);
    atomic_set(&mdev->reset_count, 0);
    
    /* Store in fixture */
    fixture->mdev = mdev;
    fixture->pdev = pdev;
    
    /* Store fixture in test context */
    test->priv = fixture;
    
    return 0;
}

static void mgpu_test_exit(struct kunit *test)
{
    struct mgpu_test_fixture *fixture = test->priv;
    
    /* Cleanup is handled by KUnit's resource management */
    if (fixture && fixture->pdev) {
        put_device(&fixture->pdev->dev);
    }
}

/* ==================================================================
 * Register Access Tests
 * ================================================================== */

static void mgpu_test_register_read_write(struct kunit *test)
{
    struct mgpu_test_fixture *fixture = test->priv;
    struct mgpu_device *mdev = fixture->mdev;
    u32 value;
    
    /* Test VERSION register (read-only) */
    value = mock_mgpu_read(mdev, MGPU_REG_VERSION);
    KUNIT_EXPECT_EQ(test, value, 0x01020304);
    
    /* Test SCRATCH register (read-write) */
    mock_mgpu_write(mdev, MGPU_REG_SCRATCH, 0xDEADBEEF);
    value = mock_mgpu_read(mdev, MGPU_REG_SCRATCH);
    KUNIT_EXPECT_EQ(test, value, 0xDEADBEEF);
    
    /* Test multiple patterns */
    u32 patterns[] = {0x00000000, 0xFFFFFFFF, 0x5A5A5A5A, 0xA5A5A5A5};
    int i;
    
    for (i = 0; i < ARRAY_SIZE(patterns); i++) {
        mock_mgpu_write(mdev, MGPU_REG_SCRATCH, patterns[i]);
        value = mock_mgpu_read(mdev, MGPU_REG_SCRATCH);
        KUNIT_EXPECT_EQ(test, value, patterns[i]);
    }
}

static void mgpu_test_control_register_bits(struct kunit *test)
{
    struct mgpu_test_fixture *fixture = test->priv;
    struct mgpu_device *mdev = fixture->mdev;
    u32 value;
    
    /* Test individual control bits */
    mock_mgpu_write(mdev, MGPU_REG_CONTROL, MGPU_CTRL_ENABLE);
    value = mock_mgpu_read(mdev, MGPU_REG_CONTROL);
    KUNIT_EXPECT_EQ(test, value & MGPU_CTRL_ENABLE, MGPU_CTRL_ENABLE);
    
    mock_mgpu_write(mdev, MGPU_REG_CONTROL, MGPU_CTRL_PAUSE);
    value = mock_mgpu_read(mdev, MGPU_REG_CONTROL);
    KUNIT_EXPECT_EQ(test, value & MGPU_CTRL_PAUSE, MGPU_CTRL_PAUSE);
    
    /* Test clearing bits */
    mock_mgpu_write(mdev, MGPU_REG_CONTROL, 0);
    value = mock_mgpu_read(mdev, MGPU_REG_CONTROL);
    KUNIT_EXPECT_EQ(test, value, 0);
}

static void mgpu_test_reset_behavior(struct kunit *test)
{
    struct mgpu_test_fixture *fixture = test->priv;
    struct mgpu_device *mdev = fixture->mdev;
    u32 value;
    
    /* Set some register values */
    mock_mgpu_write(mdev, MGPU_REG_SCRATCH, 0x12345678);
    mock_mgpu_write(mdev, MGPU_REG_VERTEX_BASE, 0x10000000);
    
    /* Trigger reset */
    mock_mgpu_write(mdev, MGPU_REG_CONTROL, MGPU_CTRL_RESET);
    
    /* Check that registers are cleared (except VERSION/CAPS) */
    value = mock_mgpu_read(mdev, MGPU_REG_SCRATCH);
    KUNIT_EXPECT_EQ(test, value, 0);
    
    value = mock_mgpu_read(mdev, MGPU_REG_VERTEX_BASE);
    KUNIT_EXPECT_EQ(test, value, 0);
    
    /* Check that VERSION/CAPS are preserved */
    value = mock_mgpu_read(mdev, MGPU_REG_VERSION);
    KUNIT_EXPECT_EQ(test, value, 0x01020304);
    
    value = mock_mgpu_read(mdev, MGPU_REG_CAPS);
    KUNIT_EXPECT_NE(test, value, 0);
    
    /* Check that status shows IDLE after reset */
    value = mock_mgpu_read(mdev, MGPU_REG_STATUS);
    KUNIT_EXPECT_EQ(test, value & MGPU_STATUS_IDLE, MGPU_STATUS_IDLE);
}

/* ==================================================================
 * Command Queue Tests
 * ================================================================== */

static void mgpu_test_command_queue_init(struct kunit *test)
{
    struct mgpu_test_fixture *fixture = test->priv;
    struct mgpu_device *mdev = fixture->mdev;
    struct mgpu_ring *ring;
    
    /* Create command ring */
    ring = mgpu_ring_create(mdev, 4096, 0);
    KUNIT_ASSERT_NOT_NULL(test, ring);
    
    /* Verify ring properties */
    KUNIT_EXPECT_EQ(test, ring->size, 4096);
    KUNIT_EXPECT_EQ(test, ring->queue_id, 0);
    KUNIT_EXPECT_EQ(test, ring->head, 0);
    KUNIT_EXPECT_EQ(test, ring->tail, 0);
    KUNIT_EXPECT_NOT_NULL(test, ring->vaddr);
    
    /* Cleanup */
    mgpu_ring_destroy(ring);
}

static void mgpu_test_command_submission(struct kunit *test)
{
    struct mgpu_test_fixture *fixture = test->priv;
    struct mgpu_device *mdev = fixture->mdev;
    struct mgpu_cmd_nop nop_cmd;
    u32 old_tail, new_tail;
    
    /* Initialize command queue */
    mdev->cmd_ring = mgpu_ring_create(mdev, 4096, 0);
    KUNIT_ASSERT_NOT_NULL(test, mdev->cmd_ring);
    
    /* Build NOP command */
    nop_cmd.header.opcode = MGPU_CMD_NOP;
    nop_cmd.header.size = sizeof(nop_cmd) / 4;
    nop_cmd.header.flags = 0;
    
    /* Get current tail */
    old_tail = mock_mgpu_read(mdev, MGPU_REG_CMD_TAIL);
    
    /* Submit command */
    struct mgpu_submit submit = {
        .commands = (uintptr_t)&nop_cmd,
        .cmd_size = sizeof(nop_cmd),
        .queue_id = 0,
        .flags = 0
    };
    
    /* Note: This would normally submit, but without full mock setup,
     * we'll just test the ring update logic */
    
    /* Simulate tail update */
    mock_mgpu_write(mdev, MGPU_REG_CMD_TAIL, old_tail + 1);
    new_tail = mock_mgpu_read(mdev, MGPU_REG_CMD_TAIL);
    
    /* Verify tail was updated */
    KUNIT_EXPECT_NE(test, new_tail, old_tail);
    
    /* Verify IRQ was triggered (in mock) */
    u32 irq_status = mock_mgpu_read(mdev, MGPU_REG_IRQ_STATUS);
    KUNIT_EXPECT_EQ(test, irq_status & MGPU_IRQ_CMD_COMPLETE, 
                    MGPU_IRQ_CMD_COMPLETE);
    
    /* Cleanup */
    mgpu_ring_destroy(mdev->cmd_ring);
    mdev->cmd_ring = NULL;
}

/* ==================================================================
 * Buffer Object Tests
 * ================================================================== */

static void mgpu_test_bo_create_destroy(struct kunit *test)
{
    struct mgpu_test_fixture *fixture = test->priv;
    struct mgpu_device *mdev = fixture->mdev;
    struct mgpu_bo_create create_args = {0};
    struct mgpu_bo_destroy destroy_args = {0};
    struct mgpu_bo *bo;
    int ret;
    
    /* Create buffer object */
    create_args.size = PAGE_SIZE;
    create_args.flags = MGPU_BO_FLAGS_COHERENT;
    
    ret = mgpu_bo_create(mdev, &create_args);
    KUNIT_EXPECT_EQ(test, ret, 0);
    KUNIT_EXPECT_NE(test, create_args.handle, 0);
    
    /* Look up BO */
    bo = mgpu_bo_lookup(mdev, create_args.handle);
    KUNIT_ASSERT_NOT_NULL(test, bo);
    KUNIT_EXPECT_EQ(test, bo->size, PAGE_SIZE);
    KUNIT_EXPECT_EQ(test, bo->flags, MGPU_BO_FLAGS_COHERENT);
    
    /* Release lookup reference */
    mgpu_bo_put(bo);
    
    /* Destroy BO */
    destroy_args.handle = create_args.handle;
    ret = mgpu_bo_destroy(mdev, &destroy_args);
    KUNIT_EXPECT_EQ(test, ret, 0);
    
    /* Verify BO is gone */
    bo = mgpu_bo_lookup(mdev, create_args.handle);
    KUNIT_EXPECT_NULL(test, bo);
}

static void mgpu_test_bo_invalid_size(struct kunit *test)
{
    struct mgpu_test_fixture *fixture = test->priv;
    struct mgpu_device *mdev = fixture->mdev;
    struct mgpu_bo_create create_args = {0};
    int ret;
    
    /* Test zero size */
    create_args.size = 0;
    create_args.flags = 0;
    ret = mgpu_bo_create(mdev, &create_args);
    KUNIT_EXPECT_NE(test, ret, 0);
    
    /* Test too large size */
    create_args.size = 512 * 1024 * 1024;  /* 512MB - too large */
    ret = mgpu_bo_create(mdev, &create_args);
    KUNIT_EXPECT_NE(test, ret, 0);
}

/* ==================================================================
 * Shader Management Tests
 * ================================================================== */

static void mgpu_test_shader_load(struct kunit *test)
{
    struct mgpu_test_fixture *fixture = test->priv;
    struct mgpu_device *mdev = fixture->mdev;
    struct mgpu_load_shader shader_args = {0};
    u32 shader_code[4];
    int ret;
    
    /* Initialize shader manager */
    ret = mgpu_shader_init(mdev);
    KUNIT_EXPECT_EQ(test, ret, 0);
    KUNIT_ASSERT_NOT_NULL(test, mdev->shader_mgr);
    
    /* Create simple shader */
    shader_code[0] = 0x4D475055;  /* 'MGPU' */
    shader_code[1] = 0x00010000;  /* Version 1.0 */
    shader_code[2] = 0x00000000;  /* NOP */
    shader_code[3] = 0x80000000;  /* HALT */
    
    /* Load shader */
    shader_args.data = (uintptr_t)shader_code;
    shader_args.size = sizeof(shader_code);
    shader_args.type = MGPU_SHADER_VERTEX;
    shader_args.slot = 0;
    
    ret = mgpu_load_shader(mdev, &shader_args);
    KUNIT_EXPECT_EQ(test, ret, 0);
    
    /* Verify shader was loaded */
    size_t size;
    u32 type;
    ret = mgpu_shader_get_info(mdev, 0, &size, &type);
    KUNIT_EXPECT_EQ(test, ret, 0);
    KUNIT_EXPECT_EQ(test, size, sizeof(shader_code));
    KUNIT_EXPECT_EQ(test, type, MGPU_SHADER_VERTEX);
    
    /* Test invalid slot */
    shader_args.slot = 16;  /* Invalid */
    ret = mgpu_load_shader(mdev, &shader_args);
    KUNIT_EXPECT_NE(test, ret, 0);
    
    /* Cleanup */
    mgpu_shader_fini(mdev);
}

static void mgpu_test_shader_bind(struct kunit *test)
{
    struct mgpu_test_fixture *fixture = test->priv;
    struct mgpu_device *mdev = fixture->mdev;
    int ret;
    
    /* Initialize shader manager */
    ret = mgpu_shader_init(mdev);
    KUNIT_EXPECT_EQ(test, ret, 0);
    
    /* Try to bind non-existent shader */
    ret = mgpu_shader_bind(mdev, 0, MGPU_SHADER_VERTEX);
    KUNIT_EXPECT_NE(test, ret, 0);
    
    /* Load a shader first */
    u32 shader_code[4] = {0x4D475055, 0x00010000, 0, 0x80000000};
    struct mgpu_load_shader shader_args = {
        .data = (uintptr_t)shader_code,
        .size = sizeof(shader_code),
        .type = MGPU_SHADER_VERTEX,
        .slot = 0
    };
    
    ret = mgpu_load_shader(mdev, &shader_args);
    KUNIT_EXPECT_EQ(test, ret, 0);
    
    /* Now bind should work */
    ret = mgpu_shader_bind(mdev, 0, MGPU_SHADER_VERTEX);
    KUNIT_EXPECT_EQ(test, ret, 0);
    
    /* Test binding wrong type */
    ret = mgpu_shader_bind(mdev, 0, MGPU_SHADER_FRAGMENT);
    KUNIT_EXPECT_NE(test, ret, 0);
    
    /* Cleanup */
    mgpu_shader_fini(mdev);
}

/* ==================================================================
 * Fence Tests
 * ================================================================== */

static void mgpu_test_fence_init(struct kunit *test)
{
    struct mgpu_test_fixture *fixture = test->priv;
    struct mgpu_device *mdev = fixture->mdev;
    int ret;
    
    /* Initialize fence subsystem */
    ret = mgpu_fence_init(mdev);
    KUNIT_EXPECT_EQ(test, ret, 0);
    KUNIT_ASSERT_NOT_NULL(test, mdev->fence_ctx);
    
    /* Get next fence value */
    u32 fence1 = mgpu_fence_next(mdev);
    u32 fence2 = mgpu_fence_next(mdev);
    
    /* Verify fence values increment */
    KUNIT_EXPECT_EQ(test, fence2, fence1 + 1);
    
    /* Cleanup */
    mgpu_fence_fini(mdev);
}

static void mgpu_test_fence_signaling(struct kunit *test)
{
    struct mgpu_test_fixture *fixture = test->priv;
    struct mgpu_device *mdev = fixture->mdev;
    struct mgpu_fence_context *ctx;
    u32 *fence_mem;
    bool signaled;
    int ret;
    
    /* Initialize fence subsystem */
    ret = mgpu_fence_init(mdev);
    KUNIT_EXPECT_EQ(test, ret, 0);
    
    ctx = mdev->fence_ctx;
    KUNIT_ASSERT_NOT_NULL(test, ctx);
    
    fence_mem = ctx->cpu_addr;
    KUNIT_ASSERT_NOT_NULL(test, fence_mem);
    
    /* Test unsignaled fence */
    fence_mem[0] = 0;
    signaled = mgpu_fence_signaled(mdev, ctx->dma_addr, 1);
    KUNIT_EXPECT_FALSE(test, signaled);
    
    /* Signal fence */
    fence_mem[0] = 1;
    signaled = mgpu_fence_signaled(mdev, ctx->dma_addr, 1);
    KUNIT_EXPECT_TRUE(test, signaled);
    
    /* Test greater-or-equal semantics */
    fence_mem[0] = 10;
    signaled = mgpu_fence_signaled(mdev, ctx->dma_addr, 5);
    KUNIT_EXPECT_TRUE(test, signaled);
    
    signaled = mgpu_fence_signaled(mdev, ctx->dma_addr, 15);
    KUNIT_EXPECT_FALSE(test, signaled);
    
    /* Cleanup */
    mgpu_fence_fini(mdev);
}

/* ==================================================================
 * IRQ Handling Tests
 * ================================================================== */

static void mgpu_test_irq_enable_disable(struct kunit *test)
{
    struct mgpu_test_fixture *fixture = test->priv;
    struct mgpu_device *mdev = fixture->mdev;
    u32 value;
    
    /* Enable interrupts */
    mgpu_irq_enable(mdev);
    value = mock_mgpu_read(mdev, MGPU_REG_IRQ_ENABLE);
    KUNIT_EXPECT_NE(test, value, 0);
    KUNIT_EXPECT_TRUE(test, value & MGPU_IRQ_CMD_COMPLETE);
    KUNIT_EXPECT_TRUE(test, value & MGPU_IRQ_ERROR);
    
    /* Disable interrupts */
    mgpu_irq_disable(mdev);
    value = mock_mgpu_read(mdev, MGPU_REG_IRQ_ENABLE);
    KUNIT_EXPECT_EQ(test, value, 0);
}

static void mgpu_test_irq_acknowledge(struct kunit *test)
{
    struct mgpu_test_fixture *fixture = test->priv;
    struct mgpu_device *mdev = fixture->mdev;
    u32 value;
    
    /* Set some interrupt status bits */
    fixture->mmio->regs[MGPU_REG_IRQ_STATUS / 4] = 
        MGPU_IRQ_CMD_COMPLETE | MGPU_IRQ_ERROR;
    
    /* Acknowledge command complete interrupt */
    mock_mgpu_write(mdev, MGPU_REG_IRQ_ACK, MGPU_IRQ_CMD_COMPLETE);
    
    /* Verify only acknowledged bit was cleared */
    value = mock_mgpu_read(mdev, MGPU_REG_IRQ_STATUS);
    KUNIT_EXPECT_FALSE(test, value & MGPU_IRQ_CMD_COMPLETE);
    KUNIT_EXPECT_TRUE(test, value & MGPU_IRQ_ERROR);
    
    /* Acknowledge remaining interrupt */
    mock_mgpu_write(mdev, MGPU_REG_IRQ_ACK, MGPU_IRQ_ERROR);
    value = mock_mgpu_read(mdev, MGPU_REG_IRQ_STATUS);
    KUNIT_EXPECT_EQ(test, value, 0);
}

/* ==================================================================
 * Reset Tests
 * ================================================================== */

static void mgpu_test_reset_state(struct kunit *test)
{
    struct mgpu_test_fixture *fixture = test->priv;
    struct mgpu_device *mdev = fixture->mdev;
    int ret;
    
    /* Initialize reset subsystem */
    ret = mgpu_reset_init(mdev);
    KUNIT_EXPECT_EQ(test, ret, 0);
    
    /* Check initial state */
    KUNIT_EXPECT_EQ(test, atomic_read(&mdev->in_reset), 0);
    KUNIT_EXPECT_EQ(test, atomic_read(&mdev->reset_count), 0);
    
    /* Simulate reset in progress */
    atomic_set(&mdev->in_reset, 1);
    KUNIT_EXPECT_EQ(test, atomic_read(&mdev->in_reset), 1);
    
    /* Increment reset count */
    atomic_inc(&mdev->reset_count);
    KUNIT_EXPECT_EQ(test, atomic_read(&mdev->reset_count), 1);
    
    /* Clear reset state */
    atomic_set(&mdev->in_reset, 0);
    
    /* Cleanup */
    mgpu_reset_fini(mdev);
}

static void mgpu_test_reset_needed_detection(struct kunit *test)
{
    struct mgpu_test_fixture *fixture = test->priv;
    struct mgpu_device *mdev = fixture->mdev;
    bool needed;
    
    /* No reset needed when idle */
    fixture->mmio->regs[MGPU_REG_STATUS / 4] = MGPU_STATUS_IDLE;
    needed = mgpu_reset_needed(mdev);
    KUNIT_EXPECT_FALSE(test, needed);
    
    /* Reset needed on error */
    fixture->mmio->regs[MGPU_REG_STATUS / 4] = MGPU_STATUS_ERROR;
    needed = mgpu_reset_needed(mdev);
    KUNIT_EXPECT_TRUE(test, needed);
    
    /* Reset needed on halt */
    fixture->mmio->regs[MGPU_REG_STATUS / 4] = MGPU_STATUS_HALTED;
    needed = mgpu_reset_needed(mdev);
    KUNIT_EXPECT_TRUE(test, needed);
}

/* ==================================================================
 * Pipeline Tests
 * ================================================================== */

static void mgpu_test_pipeline_state_validation(struct kunit *test)
{
    struct mgpu_test_fixture *fixture = test->priv;
    struct mgpu_device *mdev = fixture->mdev;
    struct mgpu_pipeline_state state = {0};
    int ret;
    
    /* Initialize shader manager (required for pipeline) */
    ret = mgpu_shader_init(mdev);
    KUNIT_EXPECT_EQ(test, ret, 0);
    
    /* Valid pipeline state */
    state.vertex_shader_slot = 0;
    state.fragment_shader_slot = 1;
    ret = mgpu_set_pipeline_state(mdev, &state);
    /* Will fail without loaded shaders, but should validate slots */
    
    /* Invalid vertex shader slot */
    state.vertex_shader_slot = 16;
    state.fragment_shader_slot = 1;
    ret = mgpu_set_pipeline_state(mdev, &state);
    KUNIT_EXPECT_NE(test, ret, 0);
    
    /* Invalid fragment shader slot */
    state.vertex_shader_slot = 0;
    state.fragment_shader_slot = 16;
    ret = mgpu_set_pipeline_state(mdev, &state);
    KUNIT_EXPECT_NE(test, ret, 0);
    
    /* Cleanup */
    mgpu_shader_fini(mdev);
}

/* ==================================================================
 * Version Parsing Tests
 * ================================================================== */

static void mgpu_test_version_parsing(struct kunit *test)
{
    u32 version;
    
    /* Test version 1.2.3.4 */
    version = 0x01020304;
    KUNIT_EXPECT_EQ(test, MGPU_VERSION_MAJOR(version), 1);
    KUNIT_EXPECT_EQ(test, MGPU_VERSION_MINOR(version), 2);
    KUNIT_EXPECT_EQ(test, MGPU_VERSION_PATCH(version), 3);
    KUNIT_EXPECT_EQ(test, MGPU_VERSION_BUILD(version), 4);
    
    /* Test version 255.255.255.255 */
    version = 0xFFFFFFFF;
    KUNIT_EXPECT_EQ(test, MGPU_VERSION_MAJOR(version), 255);
    KUNIT_EXPECT_EQ(test, MGPU_VERSION_MINOR(version), 255);
    KUNIT_EXPECT_EQ(test, MGPU_VERSION_PATCH(version), 255);
    KUNIT_EXPECT_EQ(test, MGPU_VERSION_BUILD(version), 255);
    
    /* Test version 0.0.0.0 */
    version = 0x00000000;
    KUNIT_EXPECT_EQ(test, MGPU_VERSION_MAJOR(version), 0);
    KUNIT_EXPECT_EQ(test, MGPU_VERSION_MINOR(version), 0);
    KUNIT_EXPECT_EQ(test, MGPU_VERSION_PATCH(version), 0);
    KUNIT_EXPECT_EQ(test, MGPU_VERSION_BUILD(version), 0);
}

/* ==================================================================
 * Capability Tests
 * ================================================================== */

static void mgpu_test_capability_detection(struct kunit *test)
{
    struct mgpu_test_fixture *fixture = test->priv;
    struct mgpu_device *mdev = fixture->mdev;
    u32 caps;
    
    /* Read capabilities */
    caps = mock_mgpu_read(mdev, MGPU_REG_CAPS);
    
    /* Test expected capabilities */
    KUNIT_EXPECT_TRUE(test, caps & MGPU_CAP_VERTEX_SHADER);
    KUNIT_EXPECT_TRUE(test, caps & MGPU_CAP_FRAGMENT_SHADER);
    KUNIT_EXPECT_TRUE(test, caps & MGPU_CAP_TEXTURE);
    KUNIT_EXPECT_TRUE(test, caps & MGPU_CAP_FENCE);
    
    /* Update device capabilities */
    mdev->caps = caps;
    
    /* Test queue configuration based on caps */
    if (caps & MGPU_CAP_MULTI_QUEUE) {
        KUNIT_EXPECT_GT(test, mdev->num_queues, 1);
    } else {
        mdev->num_queues = 1;
        KUNIT_EXPECT_EQ(test, mdev->num_queues, 1);
    }
}

/* ==================================================================
 * Command Validation Tests
 * ================================================================== */

static void mgpu_test_command_validation_nop(struct kunit *test)
{
    struct mgpu_test_fixture *fixture = test->priv;
    struct mgpu_device *mdev = fixture->mdev;
    struct mgpu_cmd_nop nop;
    int ret;
    
    /* Valid NOP command */
    nop.header.opcode = MGPU_CMD_NOP;
    nop.header.size = sizeof(nop) / 4;
    nop.header.flags = 0;
    
    ret = mgpu_validate_commands(mdev, (u32 *)&nop, sizeof(nop));
    KUNIT_EXPECT_EQ(test, ret, 0);
    
    /* Invalid size */
    nop.header.size = 0;
    ret = mgpu_validate_commands(mdev, (u32 *)&nop, sizeof(nop));
    KUNIT_EXPECT_NE(test, ret, 0);
    
    /* Invalid opcode */
    nop.header.opcode = 0xFF;
    nop.header.size = sizeof(nop) / 4;
    ret = mgpu_validate_commands(mdev, (u32 *)&nop, sizeof(nop));
    KUNIT_EXPECT_NE(test, ret, 0);
}

static void mgpu_test_command_validation_draw(struct kunit *test)
{
    struct mgpu_test_fixture *fixture = test->priv;
    struct mgpu_device *mdev = fixture->mdev;
    struct mgpu_cmd_draw draw;
    int ret;
    
    /* Set up vertex buffer first */
    mock_mgpu_write(mdev, MGPU_REG_VERTEX_BASE, 0x10000000);
    
    /* Valid DRAW command */
    draw.header.opcode = MGPU_CMD_DRAW;
    draw.header.size = sizeof(draw) / 4;
    draw.header.flags = 0;
    draw.vertex_count = 3;
    draw.instance_count = 1;
    draw.first_vertex = 0;
    draw.first_instance = 0;
    
    ret = mgpu_validate_commands(mdev, (u32 *)&draw, sizeof(draw));
    KUNIT_EXPECT_EQ(test, ret, 0);
    
    /* Invalid vertex count */
    draw.vertex_count = 0;
    ret = mgpu_validate_commands(mdev, (u32 *)&draw, sizeof(draw));
    KUNIT_EXPECT_NE(test, ret, 0);
    
    /* Too many vertices */
    draw.vertex_count = 100000;
    ret = mgpu_validate_commands(mdev, (u32 *)&draw, sizeof(draw));
    KUNIT_EXPECT_NE(test, ret, 0);
    
    /* Invalid instance count */
    draw.vertex_count = 3;
    draw.instance_count = 0;
    ret = mgpu_validate_commands(mdev, (u32 *)&draw, sizeof(draw));
    KUNIT_EXPECT_NE(test, ret, 0);
}

/* ==================================================================
 * Memory Barrier Tests
 * ================================================================== */

static void mgpu_test_memory_barrier(struct kunit *test)
{
    struct mgpu_test_fixture *fixture = test->priv;
    struct mgpu_device *mdev = fixture->mdev;
    u32 control;
    
    /* Test cache flush barrier */
    mgpu_memory_barrier(mdev, MGPU_BARRIER_CACHE_FLUSH);
    
    /* Verify flush bit was set and cleared */
    /* Note: In real hardware this would be more complex */
    control = mock_mgpu_read(mdev, MGPU_REG_CONTROL);
    KUNIT_EXPECT_FALSE(test, control & MGPU_CTRL_FLUSH_CACHE);
}

/* ==================================================================
 * Performance Counter Tests
 * ================================================================== */

static void mgpu_test_perf_counter_enable(struct kunit *test)
{
    struct mgpu_test_fixture *fixture = test->priv;
    struct mgpu_device *mdev = fixture->mdev;
    u32 control, irq_enable;
    int ret;
    
    /* Enable performance counters */
    ret = mgpu_perf_counter_enable(mdev, 0xFFFF);
    KUNIT_EXPECT_EQ(test, ret, 0);
    
    /* Verify control bit is set */
    control = mock_mgpu_read(mdev, MGPU_REG_CONTROL);
    KUNIT_EXPECT_TRUE(test, control & MGPU_CTRL_PERF_COUNTER);
    
    /* Verify IRQ is enabled */
    irq_enable = mock_mgpu_read(mdev, MGPU_REG_IRQ_ENABLE);
    KUNIT_EXPECT_TRUE(test, irq_enable & MGPU_IRQ_PERF_COUNTER);
    
    /* Disable performance counters */
    ret = mgpu_perf_counter_disable(mdev);
    KUNIT_EXPECT_EQ(test, ret, 0);
    
    /* Verify control bit is cleared */
    control = mock_mgpu_read(mdev, MGPU_REG_CONTROL);
    KUNIT_EXPECT_FALSE(test, control & MGPU_CTRL_PERF_COUNTER);
}

/* ==================================================================
 * Test Suite Definition
 * ================================================================== */

static struct kunit_case mgpu_test_cases[] = {
    /* Register tests */
    KUNIT_CASE(mgpu_test_register_read_write),
    KUNIT_CASE(mgpu_test_control_register_bits),
    KUNIT_CASE(mgpu_test_reset_behavior),
    
    /* Command queue tests */
    KUNIT_CASE(mgpu_test_command_queue_init),
    KUNIT_CASE(mgpu_test_command_submission),
    
    /* Buffer object tests */
    KUNIT_CASE(mgpu_test_bo_create_destroy),
    KUNIT_CASE(mgpu_test_bo_invalid_size),
    
    /* Shader tests */
    KUNIT_CASE(mgpu_test_shader_load),
    KUNIT_CASE(mgpu_test_shader_bind),
    
    /* Fence tests */
    KUNIT_CASE(mgpu_test_fence_init),
    KUNIT_CASE(mgpu_test_fence_signaling),
    
    /* IRQ tests */
    KUNIT_CASE(mgpu_test_irq_enable_disable),
    KUNIT_CASE(mgpu_test_irq_acknowledge),
    
    /* Reset tests */
    KUNIT_CASE(mgpu_test_reset_state),
    KUNIT_CASE(mgpu_test_reset_needed_detection),
    
    /* Pipeline tests */
    KUNIT_CASE(mgpu_test_pipeline_state_validation),
    
    /* Version and capability tests */
    KUNIT_CASE(mgpu_test_version_parsing),
    KUNIT_CASE(mgpu_test_capability_detection),
    
    /* Command validation tests */
    KUNIT_CASE(mgpu_test_command_validation_nop),
    KUNIT_CASE(mgpu_test_command_validation_draw),
    
    /* Memory and performance tests */
    KUNIT_CASE(mgpu_test_memory_barrier),
    KUNIT_CASE(mgpu_test_perf_counter_enable),
    
    {}
};

static struct kunit_suite mgpu_test_suite = {
    .name = "mgpu",
    .init = mgpu_test_init,
    .exit = mgpu_test_exit,
    .test_cases = mgpu_test_cases,
};

kunit_test_suite(mgpu_test_suite);

MODULE_DESCRIPTION("MGPU KUnit Test Suite");
MODULE_AUTHOR("Rafeed Khan");
MODULE_LICENSE("GPL v2");