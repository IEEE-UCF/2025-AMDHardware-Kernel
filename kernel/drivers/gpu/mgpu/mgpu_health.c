/*
 * MGPU Health Monitoring
 *
 * Copyright (C) 2024
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/delay.h>

#include "mgpu_drm.h"
#include "mgpu_regs.h"
#include "mgpu_internal.h"

/* Health check intervals */
#define MGPU_HEALTH_CHECK_INTERVAL_MS  1000    /* 1 second */
#define MGPU_HEARTBEAT_TIMEOUT_MS      5000    /* 5 seconds */
#define MGPU_HANG_CHECK_INTERVAL_MS    2000    /* 2 seconds */
#define MGPU_ERROR_THRESHOLD           10      /* Errors before reset */

/* Health monitoring state */
struct mgpu_health_monitor {
    struct mgpu_device *mdev;
    
    /* Health check work */
    struct delayed_work health_work;
    struct delayed_work hang_check_work;
    
    /* Monitoring thread */
    struct task_struct *monitor_thread;
    bool monitoring_enabled;
    
    /* Health statistics */
    struct {
        unsigned long check_count;
        unsigned long error_count;
        unsigned long hang_count;
        unsigned long recovery_count;
        unsigned long heartbeat_misses;
        ktime_t last_check;
        ktime_t last_error;
        ktime_t last_hang;
        ktime_t uptime_start;
    } stats;
    
    /* Error tracking */
    u32 consecutive_errors;
    u32 last_fence_value;
    u32 last_cmd_head;
    unsigned long last_activity;
    
    /* Heartbeat */
    u32 heartbeat_counter;
    unsigned long last_heartbeat;
    
    /* Temperature monitoring (if available) */
    int temperature;
    int max_temperature;
    bool thermal_throttled;
};

/* Error codes and descriptions */
struct mgpu_error_info {
    u32 code;
    const char *name;
    const char *description;
    bool recoverable;
};

static const struct mgpu_error_info mgpu_error_table[] = {
    { MGPU_ERROR_NONE,         "NONE",         "No error",                false },
    { MGPU_ERROR_INVALID_CMD,  "INVALID_CMD",  "Invalid command",         true  },
    { MGPU_ERROR_MEM_FAULT,    "MEM_FAULT",    "Memory access fault",     true  },
    { MGPU_ERROR_SHADER_FAULT, "SHADER_FAULT", "Shader execution fault",  true  },
    { MGPU_ERROR_TIMEOUT,      "TIMEOUT",      "Operation timeout",       true  },
    { MGPU_ERROR_OVERFLOW,     "OVERFLOW",     "Queue overflow",          true  },
};

/* Get error description */
static const struct mgpu_error_info *mgpu_get_error_info(u32 error_code)
{
    int i;
    
    for (i = 0; i < ARRAY_SIZE(mgpu_error_table); i++) {
        if (mgpu_error_table[i].code == error_code)
            return &mgpu_error_table[i];
    }
    
    return &mgpu_error_table[0]; /* Return "NONE" for unknown */
}

/* Check GPU heartbeat */
static bool mgpu_health_check_heartbeat(struct mgpu_health_monitor *monitor)
{
    struct mgpu_device *mdev = monitor->mdev;
    u32 scratch;
    bool alive = true;
    
    /* Write heartbeat value to scratch register */
    monitor->heartbeat_counter++;
    mgpu_write(mdev, MGPU_REG_SCRATCH, monitor->heartbeat_counter);
    
    /* Read back and verify */
    scratch = mgpu_read(mdev, MGPU_REG_SCRATCH);
    if (scratch != monitor->heartbeat_counter) {
        dev_err(mdev->dev, "Heartbeat failed: wrote 0x%08x, read 0x%08x\n",
                monitor->heartbeat_counter, scratch);
        monitor->stats.heartbeat_misses++;
        alive = false;
    }
    
    monitor->last_heartbeat = jiffies;
    
    return alive;
}

/* Check for GPU hang */
static bool mgpu_health_check_hang(struct mgpu_health_monitor *monitor)
{
    struct mgpu_device *mdev = monitor->mdev;
    u32 status, cmd_head, fence_value;
    bool hung = false;
    
    status = mgpu_read(mdev, MGPU_REG_STATUS);
    
    /* Check if GPU claims to be busy */
    if (!(status & MGPU_STATUS_BUSY))
        return false;
    
    /* Check command progress */
    cmd_head = mgpu_read(mdev, MGPU_REG_CMD_HEAD);
    if (cmd_head == monitor->last_cmd_head) {
        /* No progress in command processing */
        if (time_after(jiffies, 
                      monitor->last_activity + msecs_to_jiffies(MGPU_HEARTBEAT_TIMEOUT_MS))) {
            dev_warn(mdev->dev, "GPU hang detected: command head stuck at %u\n",
                     cmd_head);
            hung = true;
        }
    } else {
        monitor->last_cmd_head = cmd_head;
        monitor->last_activity = jiffies;
    }
    
    /* Check fence progress */
    fence_value = mgpu_read(mdev, MGPU_REG_FENCE_VALUE);
    if (fence_value == monitor->last_fence_value) {
        /* No fence progress */
        if (time_after(jiffies,
                      monitor->last_activity + msecs_to_jiffies(MGPU_HEARTBEAT_TIMEOUT_MS))) {
            dev_warn(mdev->dev, "GPU hang detected: fence stuck at %u\n",
                     fence_value);
            hung = true;
        }
    } else {
        monitor->last_fence_value = fence_value;
        monitor->last_activity = jiffies;
    }
    
    if (hung) {
        monitor->stats.hang_count++;
        monitor->stats.last_hang = ktime_get();
    }
    
    return hung;
}

/* Check for GPU errors */
static int mgpu_health_check_errors(struct mgpu_health_monitor *monitor)
{
    struct mgpu_device *mdev = monitor->mdev;
    u32 status;
    int error_count = 0;
    
    status = mgpu_read(mdev, MGPU_REG_STATUS);
    
    /* Check error bit */
    if (status & MGPU_STATUS_ERROR) {
        const struct mgpu_error_info *info;
        u32 error_code = (status >> 16) & 0xFF; /* Assuming error code in upper bits */
        
        info = mgpu_get_error_info(error_code);
        dev_err(mdev->dev, "GPU error detected: %s - %s\n",
                info->name, info->description);
        
        monitor->stats.error_count++;
        monitor->stats.last_error = ktime_get();
        monitor->consecutive_errors++;
        error_count++;
        
        /* Clear error if recoverable */
        if (info->recoverable) {
            /* Clear error status */
            mgpu_write(mdev, MGPU_REG_STATUS, status & ~MGPU_STATUS_ERROR);
        }
    } else {
        /* Reset consecutive error counter if no error */
        monitor->consecutive_errors = 0;
    }
    
    /* Check if halted */
    if (status & MGPU_STATUS_HALTED) {
        dev_err(mdev->dev, "GPU halted\n");
        error_count++;
    }
    
    /* Check command queue status */
    if (status & MGPU_STATUS_CMD_FULL) {
        dev_warn(mdev->dev, "Command queue full\n");
        /* Not necessarily an error, but worth noting */
    }
    
    return error_count;
}

/* Perform health check */
static void mgpu_health_check(struct mgpu_health_monitor *monitor)
{
    struct mgpu_device *mdev = monitor->mdev;
    bool needs_reset = false;
    int error_count;
    
    monitor->stats.check_count++;
    monitor->stats.last_check = ktime_get();
    
    /* Check heartbeat */
    if (!mgpu_health_check_heartbeat(monitor)) {
        dev_err(mdev->dev, "GPU heartbeat check failed\n");
        needs_reset = true;
    }
    
    /* Check for errors */
    error_count = mgpu_health_check_errors(monitor);
    if (error_count > 0) {
        dev_warn(mdev->dev, "Health check found %d errors\n", error_count);
        
        /* Too many consecutive errors? */
        if (monitor->consecutive_errors >= MGPU_ERROR_THRESHOLD) {
            dev_err(mdev->dev, "Error threshold exceeded (%u errors)\n",
                    monitor->consecutive_errors);
            needs_reset = true;
        }
    }
    
    /* Check for hang */
    if (mgpu_health_check_hang(monitor)) {
        dev_err(mdev->dev, "GPU hang detected\n");
        needs_reset = true;
    }
    
    /* Trigger reset if needed */
    if (needs_reset) {
        dev_err(mdev->dev, "Health check triggering GPU reset\n");
        monitor->stats.recovery_count++;
        mgpu_reset_schedule(mdev);
    }
}

/* Health check work function */
static void mgpu_health_work_func(struct work_struct *work)
{
    struct mgpu_health_monitor *monitor =
        container_of(work, struct mgpu_health_monitor, health_work.work);
    
    if (!monitor->monitoring_enabled)
        return;
    
    mgpu_health_check(monitor);
    
    /* Schedule next check */
    schedule_delayed_work(&monitor->health_work,
                         msecs_to_jiffies(MGPU_HEALTH_CHECK_INTERVAL_MS));
}

/* Hang check work function */
static void mgpu_hang_check_work_func(struct work_struct *work)
{
    struct mgpu_health_monitor *monitor =
        container_of(work, struct mgpu_health_monitor, hang_check_work.work);
    
    if (!monitor->monitoring_enabled)
        return;
    
    if (mgpu_health_check_hang(monitor)) {
        dev_err(monitor->mdev->dev, "Hang check detected GPU hang\n");
        mgpu_reset_schedule(monitor->mdev);
    }
    
    /* Schedule next check */
    schedule_delayed_work(&monitor->hang_check_work,
                         msecs_to_jiffies(MGPU_HANG_CHECK_INTERVAL_MS));
}

/* Monitoring thread */
static int mgpu_monitor_thread(void *data)
{
    struct mgpu_health_monitor *monitor = data;
    struct mgpu_device *mdev = monitor->mdev;
    
    dev_info(mdev->dev, "Health monitor thread started\n");
    
    while (!kthread_should_stop()) {
        if (monitor->monitoring_enabled) {
            /* Perform detailed health check */
            mgpu_health_check(monitor);
            
            /* Check temperature if available */
            /* TODO: Read temperature from thermal sensor if present */
            
            /* Log statistics periodically */
            if (monitor->stats.check_count % 60 == 0) { /* Every minute */
                dev_dbg(mdev->dev,
                        "Health stats: checks=%lu, errors=%lu, hangs=%lu, recoveries=%lu\n",
                        monitor->stats.check_count,
                        monitor->stats.error_count,
                        monitor->stats.hang_count,
                        monitor->stats.recovery_count);
            }
        }
        
        /* Sleep for check interval */
        msleep(MGPU_HEALTH_CHECK_INTERVAL_MS);
    }
    
    dev_info(mdev->dev, "Health monitor thread stopped\n");
    
    return 0;
}

/* Initialize health monitoring */
int mgpu_health_init(struct mgpu_device *mdev)
{
    struct mgpu_health_monitor *monitor;
    
    monitor = kzalloc(sizeof(*monitor), GFP_KERNEL);
    if (!monitor)
        return -ENOMEM;
    
    monitor->mdev = mdev;
    monitor->stats.uptime_start = ktime_get();
    monitor->last_activity = jiffies;
    
    /* Initialize work queues */
    INIT_DELAYED_WORK(&monitor->health_work, mgpu_health_work_func);
    INIT_DELAYED_WORK(&monitor->hang_check_work, mgpu_hang_check_work_func);
    
    /* Create monitoring thread */
    monitor->monitor_thread = kthread_create(mgpu_monitor_thread, monitor,
                                            "mgpu_monitor");
    if (IS_ERR(monitor->monitor_thread)) {
        int ret = PTR_ERR(monitor->monitor_thread);
        kfree(monitor);
        return ret;
    }
    
    /* Store monitor in device */
    mdev->health_monitor = monitor;
    
    /* Start monitoring */
    monitor->monitoring_enabled = true;
    wake_up_process(monitor->monitor_thread);
    schedule_delayed_work(&monitor->health_work,
                         msecs_to_jiffies(MGPU_HEALTH_CHECK_INTERVAL_MS));
    schedule_delayed_work(&monitor->hang_check_work,
                         msecs_to_jiffies(MGPU_HANG_CHECK_INTERVAL_MS));
    
    dev_info(mdev->dev, "Health monitoring initialized\n");
    
    return 0;
}

/* Cleanup health monitoring */
void mgpu_health_fini(struct mgpu_device *mdev)
{
    struct mgpu_health_monitor *monitor = mdev->health_monitor;
    
    if (!monitor)
        return;
    
    /* Stop monitoring */
    monitor->monitoring_enabled = false;
    
    /* Cancel work */
    cancel_delayed_work_sync(&monitor->health_work);
    cancel_delayed_work_sync(&monitor->hang_check_work);
    
    /* Stop monitoring thread */
    if (monitor->monitor_thread) {
        kthread_stop(monitor->monitor_thread);
    }
    
    /* Log final statistics */
    dev_info(mdev->dev,
             "Health monitor final stats: checks=%lu, errors=%lu, hangs=%lu, recoveries=%lu\n",
             monitor->stats.check_count,
             monitor->stats.error_count,
             monitor->stats.hang_count,
             monitor->stats.recovery_count);
    
    kfree(monitor);
    mdev->health_monitor = NULL;
}

/* Perform immediate health check */
int mgpu_health_check_now(struct mgpu_device *mdev)
{
    struct mgpu_health_monitor *monitor = mdev->health_monitor;
    
    if (!monitor)
        return -ENODEV;
    
    mgpu_health_check(monitor);
    
    return 0;
}

/* Run self-test */
int mgpu_run_selftest(struct mgpu_device *mdev)
{
    u32 test_pattern[] = { 0xDEADBEEF, 0xCAFEBABE, 0x12345678, 0x87654321 };
    u32 readback;
    int i, ret = 0;
    
    dev_info(mdev->dev, "Running GPU self-test...\n");
    
    /* Test 1: Register read/write */
    for (i = 0; i < ARRAY_SIZE(test_pattern); i++) {
        mgpu_write(mdev, MGPU_REG_SCRATCH, test_pattern[i]);
        readback = mgpu_read(mdev, MGPU_REG_SCRATCH);
        if (readback != test_pattern[i]) {
            dev_err(mdev->dev,
                    "Self-test failed: register test pattern %d (wrote 0x%08x, read 0x%08x)\n",
                    i, test_pattern[i], readback);
            ret = -EIO;
        }
    }
    
    /* Test 2: Verify version register */
    readback = mgpu_read(mdev, MGPU_REG_VERSION);
    if (readback == 0 || readback == 0xFFFFFFFF) {
        dev_err(mdev->dev,
                "Self-test failed: invalid version register (0x%08x)\n",
                readback);
        ret = -EIO;
    }
    
    /* Test 3: Basic command submission */
    /* TODO: Submit a NOP command and verify completion */
    
    /* Test 4: Memory test */
    /* TODO: Allocate buffer, write pattern, read back */
    
    if (ret == 0) {
        dev_info(mdev->dev, "Self-test passed\n");
    } else {
        dev_err(mdev->dev, "Self-test failed\n");
    }
    
    return ret;
}

/* Dump GPU state for debugging */
void mgpu_dump_state(struct mgpu_device *mdev)
{
    struct mgpu_health_monitor *monitor = mdev->health_monitor;
    
    dev_info(mdev->dev, "=== GPU State Dump ===\n");
    
    /* Dump registers */
    mgpu_core_dump_state(mdev);
    
    /* Dump health statistics */
    if (monitor) {
        dev_info(mdev->dev, "Health Statistics:\n");
        dev_info(mdev->dev, "  Checks:     %lu\n", monitor->stats.check_count);
        dev_info(mdev->dev, "  Errors:     %lu\n", monitor->stats.error_count);
        dev_info(mdev->dev, "  Hangs:      %lu\n", monitor->stats.hang_count);
        dev_info(mdev->dev, "  Recoveries: %lu\n", monitor->stats.recovery_count);
        dev_info(mdev->dev, "  Heartbeat misses: %lu\n", monitor->stats.heartbeat_misses);
        dev_info(mdev->dev, "  Consecutive errors: %u\n", monitor->consecutive_errors);
    }
    
    dev_info(mdev->dev, "======================\n");
}

MODULE_DESCRIPTION("MGPU Health Monitoring");
MODULE_AUTHOR("Your Name");
MODULE_LICENSE("GPL v2");