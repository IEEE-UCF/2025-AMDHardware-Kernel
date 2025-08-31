/*
 * MGPU Power Management
 *
 * Copyright (C) 2024
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/regulator/consumer.h>

#include "mgpu_drm.h"
#include "mgpu_regs.h"
#include "mgpu_internal.h"

/* Power state names for debugging */
static const char * const mgpu_power_state_names[] = {
    [MGPU_POWER_D0] = "D0 (Active)",
    [MGPU_POWER_D1] = "D1 (Idle)",
    [MGPU_POWER_D2] = "D2 (Standby)",
    [MGPU_POWER_D3] = "D3 (Off)",
};

/* Clock management */
struct mgpu_clocks {
    struct clk *core_clk;
    struct clk *mem_clk;
    struct clk *axi_clk;
    bool enabled;
};

/* Voltage regulators */
struct mgpu_regulators {
    struct regulator *vdd_core;
    struct regulator *vdd_mem;
    bool enabled;
};

/* Extended PM state */
struct mgpu_pm_state {
    enum mgpu_power_state power_state;
    struct mgpu_clocks clocks;
    struct mgpu_regulators regulators;
    
    /* Runtime PM */
    bool runtime_enabled;
    atomic_t usage_count;
    
    /* Suspend/resume state */
    u32 saved_regs[64];
    bool suspended;
    
    /* Statistics */
    unsigned long suspend_count;
    unsigned long resume_count;
    unsigned long idle_count;
    ktime_t last_suspend;
    ktime_t last_resume;
    ktime_t total_active_time;
    ktime_t total_idle_time;
};

/* Save GPU register state */
static void mgpu_pm_save_registers(struct mgpu_device *mdev)
{
    struct mgpu_pm_state *pm = mdev->pm_state;
    
    /* Save critical registers */
    pm->saved_regs[0] = mgpu_read(mdev, MGPU_REG_CONTROL);
    pm->saved_regs[1] = mgpu_read(mdev, MGPU_REG_IRQ_ENABLE);
    pm->saved_regs[2] = mgpu_read(mdev, MGPU_REG_CMD_BASE);
    pm->saved_regs[3] = mgpu_read(mdev, MGPU_REG_CMD_SIZE);
    pm->saved_regs[4] = mgpu_read(mdev, MGPU_REG_FENCE_ADDR);
    pm->saved_regs[5] = mgpu_read(mdev, MGPU_REG_VERTEX_BASE);
    pm->saved_regs[6] = mgpu_read(mdev, MGPU_REG_VERTEX_COUNT);
    pm->saved_regs[7] = mgpu_read(mdev, MGPU_REG_VERTEX_STRIDE);
    pm->saved_regs[8] = mgpu_read(mdev, MGPU_REG_SHADER_PC);
    
    dev_dbg(mdev->dev, "Saved GPU register state\n");
}

/* Restore GPU register state */
static void mgpu_pm_restore_registers(struct mgpu_device *mdev)
{
    struct mgpu_pm_state *pm = mdev->pm_state;
    
    /* Restore critical registers */
    mgpu_write(mdev, MGPU_REG_CMD_BASE, pm->saved_regs[2]);
    mgpu_write(mdev, MGPU_REG_CMD_SIZE, pm->saved_regs[3]);
    mgpu_write(mdev, MGPU_REG_FENCE_ADDR, pm->saved_regs[4]);
    mgpu_write(mdev, MGPU_REG_VERTEX_BASE, pm->saved_regs[5]);
    mgpu_write(mdev, MGPU_REG_VERTEX_COUNT, pm->saved_regs[6]);
    mgpu_write(mdev, MGPU_REG_VERTEX_STRIDE, pm->saved_regs[7]);
    mgpu_write(mdev, MGPU_REG_SHADER_PC, pm->saved_regs[8]);
    mgpu_write(mdev, MGPU_REG_IRQ_ENABLE, pm->saved_regs[1]);
    mgpu_write(mdev, MGPU_REG_CONTROL, pm->saved_regs[0]);
    
    dev_dbg(mdev->dev, "Restored GPU register state\n");
}

/* Enable clocks */
static int mgpu_pm_enable_clocks(struct mgpu_device *mdev)
{
    struct mgpu_pm_state *pm = mdev->pm_state;
    int ret;
    
    if (pm->clocks.enabled)
        return 0;
    
    /* Enable core clock */
    if (pm->clocks.core_clk) {
        ret = clk_prepare_enable(pm->clocks.core_clk);
        if (ret) {
            dev_err(mdev->dev, "Failed to enable core clock: %d\n", ret);
            return ret;
        }
    }
    
    /* Enable memory clock */
    if (pm->clocks.mem_clk) {
        ret = clk_prepare_enable(pm->clocks.mem_clk);
        if (ret) {
            dev_err(mdev->dev, "Failed to enable memory clock: %d\n", ret);
            goto err_mem_clk;
        }
    }
    
    /* Enable AXI clock */
    if (pm->clocks.axi_clk) {
        ret = clk_prepare_enable(pm->clocks.axi_clk);
        if (ret) {
            dev_err(mdev->dev, "Failed to enable AXI clock: %d\n", ret);
            goto err_axi_clk;
        }
    }
    
    pm->clocks.enabled = true;
    dev_dbg(mdev->dev, "Clocks enabled\n");
    
    return 0;
    
err_axi_clk:
    if (pm->clocks.mem_clk)
        clk_disable_unprepare(pm->clocks.mem_clk);
err_mem_clk:
    if (pm->clocks.core_clk)
        clk_disable_unprepare(pm->clocks.core_clk);
    
    return ret;
}

/* Disable clocks */
static void mgpu_pm_disable_clocks(struct mgpu_device *mdev)
{
    struct mgpu_pm_state *pm = mdev->pm_state;
    
    if (!pm->clocks.enabled)
        return;
    
    if (pm->clocks.axi_clk)
        clk_disable_unprepare(pm->clocks.axi_clk);
    
    if (pm->clocks.mem_clk)
        clk_disable_unprepare(pm->clocks.mem_clk);
    
    if (pm->clocks.core_clk)
        clk_disable_unprepare(pm->clocks.core_clk);
    
    pm->clocks.enabled = false;
    dev_dbg(mdev->dev, "Clocks disabled\n");
}

/* Set power state */
static int mgpu_pm_set_power_state(struct mgpu_device *mdev,
                                   enum mgpu_power_state state)
{
    struct mgpu_pm_state *pm = mdev->pm_state;
    int ret = 0;
    
    if (pm->power_state == state)
        return 0;
    
    dev_dbg(mdev->dev, "Transitioning from %s to %s\n",
            mgpu_power_state_names[pm->power_state],
            mgpu_power_state_names[state]);
    
    switch (state) {
    case MGPU_POWER_D0:
        /* Full power */
        ret = mgpu_pm_enable_clocks(mdev);
        if (ret)
            return ret;
        
        /* Enable GPU */
        mgpu_write(mdev, MGPU_REG_CONTROL, MGPU_CTRL_ENABLE);
        break;
        
    case MGPU_POWER_D1:
        /* Idle - reduce clocks */
        if (pm->clocks.core_clk) {
            ret = clk_set_rate(pm->clocks.core_clk,
                             clk_get_rate(pm->clocks.core_clk) / 2);
            if (ret)
                dev_warn(mdev->dev, "Failed to reduce clock rate\n");
        }
        break;
        
    case MGPU_POWER_D2:
        /* Standby - disable GPU but keep clocks */
        mgpu_write(mdev, MGPU_REG_CONTROL, 0);
        break;
        
    case MGPU_POWER_D3:
        /* Off - disable everything */
        mgpu_write(mdev, MGPU_REG_CONTROL, 0);
        mgpu_pm_disable_clocks(mdev);
        break;
    }
    
    pm->power_state = state;
    
    return ret;
}

/* Runtime PM suspend */
static int mgpu_pm_runtime_suspend(struct device *dev)
{
    struct platform_device *pdev = to_platform_device(dev);
    struct mgpu_device *mdev = platform_get_drvdata(pdev);
    struct mgpu_pm_state *pm = mdev->pm_state;
    int ret;
    
    dev_dbg(dev, "Runtime suspend\n");
    
    /* Wait for GPU to idle */
    ret = mgpu_core_wait_idle(mdev, MGPU_IDLE_TIMEOUT_MS);
    if (ret) {
        dev_warn(dev, "GPU not idle for runtime suspend\n");
        return -EBUSY;
    }
    
    /* Save state */
    mgpu_pm_save_registers(mdev);
    
    /* Move to D2 (standby) */
    ret = mgpu_pm_set_power_state(mdev, MGPU_POWER_D2);
    if (ret)
        return ret;
    
    pm->idle_count++;
    
    return 0;
}

/* Runtime PM resume */
static int mgpu_pm_runtime_resume(struct device *dev)
{
    struct platform_device *pdev = to_platform_device(dev);
    struct mgpu_device *mdev = platform_get_drvdata(pdev);
    int ret;
    
    dev_dbg(dev, "Runtime resume\n");
    
    /* Move to D0 (active) */
    ret = mgpu_pm_set_power_state(mdev, MGPU_POWER_D0);
    if (ret)
        return ret;
    
    /* Restore state */
    mgpu_pm_restore_registers(mdev);
    
    /* Verify GPU is responsive */
    ret = mgpu_core_test_alive(mdev);
    if (ret) {
        dev_err(dev, "GPU not responsive after runtime resume\n");
        return ret;
    }
    
    return 0;
}

/* System suspend */
static int mgpu_pm_suspend(struct device *dev)
{
    struct platform_device *pdev = to_platform_device(dev);
    struct mgpu_device *mdev = platform_get_drvdata(pdev);
    struct mgpu_pm_state *pm = mdev->pm_state;
    int ret;
    
    dev_info(dev, "System suspend\n");
    
    /* Already suspended? */
    if (pm->suspended)
        return 0;
    
    /* Disable runtime PM during system suspend */
    pm_runtime_disable(dev);
    
    /* Stop all GPU activity */
    mgpu_cmdq_suspend(mdev);
    
    /* Wait for GPU to idle */
    ret = mgpu_core_wait_idle(mdev, MGPU_IDLE_TIMEOUT_MS);
    if (ret) {
        dev_err(dev, "GPU failed to idle for suspend\n");
        pm_runtime_enable(dev);
        return ret;
    }
    
    /* Save complete state */
    mgpu_pm_save_registers(mdev);
    
    /* Disable interrupts */
    mgpu_irq_suspend(mdev);
    
    /* Power down to D3 */
    ret = mgpu_pm_set_power_state(mdev, MGPU_POWER_D3);
    if (ret) {
        mgpu_irq_resume(mdev);
        pm_runtime_enable(dev);
        return ret;
    }
    
    pm->suspended = true;
    pm->suspend_count++;
    pm->last_suspend = ktime_get();
    
    return 0;
}

/* System resume */
static int mgpu_pm_resume(struct device *dev)
{
    struct platform_device *pdev = to_platform_device(dev);
    struct mgpu_device *mdev = platform_get_drvdata(pdev);
    struct mgpu_pm_state *pm = mdev->pm_state;
    int ret;
    
    dev_info(dev, "System resume\n");
    
    if (!pm->suspended)
        return 0;
    
    /* Power up to D0 */
    ret = mgpu_pm_set_power_state(mdev, MGPU_POWER_D0);
    if (ret) {
        dev_err(dev, "Failed to power up GPU: %d\n", ret);
        return ret;
    }
    
    /* Reinitialize hardware */
    ret = mgpu_core_init(mdev);
    if (ret) {
        dev_err(dev, "Failed to reinitialize GPU: %d\n", ret);
        return ret;
    }
    
    /* Restore state */
    mgpu_pm_restore_registers(mdev);
    
    /* Resume interrupts */
    ret = mgpu_irq_resume(mdev);
    if (ret) {
        dev_err(dev, "Failed to resume IRQ: %d\n", ret);
        return ret;
    }
    
    /* Resume command queues */
    ret = mgpu_cmdq_resume(mdev);
    if (ret) {
        dev_err(dev, "Failed to resume command queues: %d\n", ret);
        return ret;
    }
    
    /* Re-enable runtime PM */
    pm_runtime_enable(dev);
    
    pm->suspended = false;
    pm->resume_count++;
    pm->last_resume = ktime_get();
    
    return 0;
}

/* Initialize power management */
int mgpu_pm_init(struct mgpu_device *mdev)
{
    struct device *dev = mdev->dev;
    struct mgpu_pm_state *pm;
    int ret;
    
    pm = kzalloc(sizeof(*pm), GFP_KERNEL);
    if (!pm)
        return -ENOMEM;
    
    mdev->pm_state = pm;
    pm->power_state = MGPU_POWER_D0;
    atomic_set(&pm->usage_count, 0);
    
    /* Try to get clocks (optional on some platforms) */
    pm->clocks.core_clk = devm_clk_get(dev, "core");
    if (IS_ERR(pm->clocks.core_clk)) {
        pm->clocks.core_clk = NULL;
        dev_info(dev, "Core clock not available\n");
    }
    
    pm->clocks.mem_clk = devm_clk_get(dev, "mem");
    if (IS_ERR(pm->clocks.mem_clk)) {
        pm->clocks.mem_clk = NULL;
        dev_info(dev, "Memory clock not available\n");
    }
    
    pm->clocks.axi_clk = devm_clk_get(dev, "axi");
    if (IS_ERR(pm->clocks.axi_clk)) {
        pm->clocks.axi_clk = NULL;
        dev_info(dev, "AXI clock not available\n");
    }
    
    /* Enable clocks if available */
    ret = mgpu_pm_enable_clocks(mdev);
    if (ret) {
        dev_warn(dev, "Failed to enable clocks: %d\n", ret);
        /* Continue without clocks */
    }
    
    /* Enable runtime PM */
    pm_runtime_set_autosuspend_delay(dev, 5000); /* 5 seconds */
    pm_runtime_use_autosuspend(dev);
    pm_runtime_set_active(dev);
    pm_runtime_enable(dev);
    pm->runtime_enabled = true;
    
    dev_info(dev, "Power management initialized\n");
    
    return 0;
}

/* Cleanup power management */
void mgpu_pm_fini(struct mgpu_device *mdev)
{
    struct device *dev = mdev->dev;
    struct mgpu_pm_state *pm = mdev->pm_state;
    
    if (!pm)
        return;
    
    /* Disable runtime PM */
    if (pm->runtime_enabled) {
        pm_runtime_disable(dev);
        pm_runtime_set_suspended(dev);
    }
    
    /* Power down */
    mgpu_pm_set_power_state(mdev, MGPU_POWER_D3);
    
    /* Disable clocks */
    mgpu_pm_disable_clocks(mdev);
    
    kfree(pm);
    mdev->pm_state = NULL;
}

/* PM operations */
const struct dev_pm_ops mgpu_pm_ops = {
    .suspend = mgpu_pm_suspend,
    .resume = mgpu_pm_resume,
    .runtime_suspend = mgpu_pm_runtime_suspend,
    .runtime_resume = mgpu_pm_runtime_resume,
};

/* Manual power state control (for debugging) */
int mgpu_pm_force_state(struct mgpu_device *mdev, enum mgpu_power_state state)
{
    return mgpu_pm_set_power_state(mdev, state);
}

/* Get current power state */
enum mgpu_power_state mgpu_pm_get_state(struct mgpu_device *mdev)
{
    struct mgpu_pm_state *pm = mdev->pm_state;
    return pm ? pm->power_state : MGPU_POWER_D0;
}

/* Handle performance counter interrupt */
void mgpu_pm_handle_perf_irq(struct mgpu_device *mdev)
{
    /* TODO: Read and process performance counters */
    dev_dbg(mdev->dev, "Performance counter interrupt\n");
}

MODULE_DESCRIPTION("MGPU Power Management");
MODULE_AUTHOR("Your Name");
MODULE_LICENSE("GPL v2");