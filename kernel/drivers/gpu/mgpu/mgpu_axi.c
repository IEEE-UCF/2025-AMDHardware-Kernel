/*
 * MGPU AXI Transport Backend
 * Based on axi_wrapper.sv hardware implementation
 * 
 * Handles AXI4-Lite slave interface for register access
 * and AXI4 master interface for DDR memory access
 *
 * Copyright (C) 2025
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/amba/bus.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/completion.h>
#include <linux/jiffies.h>

#include "mgpu_drm.h"
#include "mgpu_regs.h"
#include "mgpu_internal.h"

/* AXI specific defines from axi_wrapper.sv */
#define AXI_BURST_TYPE_FIXED  0x00
#define AXI_BURST_TYPE_INCR   0x01
#define AXI_BURST_TYPE_WRAP   0x02

#define AXI_SIZE_1_BYTE       0x00
#define AXI_SIZE_2_BYTES      0x01
#define AXI_SIZE_4_BYTES      0x02
#define AXI_SIZE_8_BYTES      0x03

#define AXI_RESP_OKAY         0x00
#define AXI_RESP_EXOKAY       0x01
#define AXI_RESP_SLVERR       0x02
#define AXI_RESP_DECERR       0x03

/* AXI cache attributes */
#define AXI_CACHE_BUFFERABLE  (1 << 0)
#define AXI_CACHE_CACHEABLE   (1 << 1)
#define AXI_CACHE_RA          (1 << 2)  /* Read-allocate */
#define AXI_CACHE_WA          (1 << 3)  /* Write-allocate */

/* Default cache setting for normal memory */
#define AXI_CACHE_NORMAL      0x0F  /* Cacheable, bufferable, allocate */
#define AXI_CACHE_DEVICE      0x00  /* Non-cacheable, non-bufferable */

/* AXI transaction states (from axi_wrapper.sv) */
enum axi_state {
    AXI_IDLE = 0,
    AXI_WRITE_ADDR,
    AXI_WRITE_DATA, 
    AXI_WRITE_RESP,
    AXI_READ_ADDR,
    AXI_READ_DATA
};

/* AXI transaction descriptor */
struct mgpu_axi_transaction {
    dma_addr_t addr;
    void *data;
    size_t size;
    bool is_write;
    bool is_burst;
    u32 burst_len;
    u32 burst_size;
    u32 burst_type;
    u32 cache_attr;
    u32 prot_attr;
    struct completion completion;
    int status;
};

/* AXI controller state */
struct mgpu_axi_ctrl {
    struct mgpu_device *mdev;
    
    /* AXI configuration */
    u32 data_width;     /* 32 or 64 bits */
    u32 addr_width;     /* Address bus width */
    u32 id_width;       /* Transaction ID width */
    u32 max_burst_len;  /* Maximum burst length */
    
    /* Current transaction */
    struct mgpu_axi_transaction *current_txn;
    enum axi_state state;
    spinlock_t lock;
    
    /* Performance counters */
    u64 read_txns;
    u64 write_txns;
    u64 read_bytes;
    u64 write_bytes;
    u64 error_count;
    
    /* Error tracking */
    u32 last_error_addr;
    u32 last_error_resp;
    
    /* Timeout handling */
    struct timer_list timeout_timer;
    unsigned long timeout_jiffies;
};

/* Get AXI controller from device */
static struct mgpu_axi_ctrl *mgpu_get_axi_ctrl(struct mgpu_device *mdev)
{
    return mdev->axi_ctrl;
}

/* AXI timeout handler */
static void mgpu_axi_timeout(struct timer_list *t)
{
    struct mgpu_axi_ctrl *ctrl = from_timer(ctrl, t, timeout_timer);
    struct mgpu_device *mdev = ctrl->mdev;
    
    dev_err(mdev->dev, "AXI transaction timeout in state %d\n", ctrl->state);
    
    /* Mark transaction as failed */
    if (ctrl->current_txn) {
        ctrl->current_txn->status = -ETIMEDOUT;
        complete(&ctrl->current_txn->completion);
        ctrl->current_txn = NULL;
    }
    
    /* Reset AXI state machine */
    ctrl->state = AXI_IDLE;
    ctrl->error_count++;
    
    /* Trigger GPU reset if too many errors */
    if (ctrl->error_count > 10) {
        dev_err(mdev->dev, "Too many AXI errors, triggering GPU reset\n");
        mgpu_reset_schedule(mdev);
    }
}

/* Setup AXI burst parameters */
static void mgpu_axi_setup_burst(struct mgpu_axi_transaction *txn,
                                 dma_addr_t addr, size_t size)
{
    /* Calculate burst parameters */
    if (size <= 4) {
        /* Single beat transfer */
        txn->is_burst = false;
        txn->burst_len = 0;
        txn->burst_size = AXI_SIZE_4_BYTES;
        txn->burst_type = AXI_BURST_TYPE_FIXED;
    } else {
        /* Burst transfer */
        txn->is_burst = true;
        txn->burst_len = (size / 4) - 1;  /* Length in beats minus 1 */
        txn->burst_size = AXI_SIZE_4_BYTES;
        txn->burst_type = AXI_BURST_TYPE_INCR;
        
        /* Limit burst length to hardware maximum (256 beats) */
        if (txn->burst_len > 255) {
            txn->burst_len = 255;
        }
    }
    
    /* Set cache attributes based on address range */
    if (addr >= 0x00000000 && addr < 0x40000000) {
        /* DDR memory - cacheable */
        txn->cache_attr = AXI_CACHE_NORMAL;
    } else {
        /* Device memory - non-cacheable */
        txn->cache_attr = AXI_CACHE_DEVICE;
    }
    
    /* Protection: non-secure, non-privileged, data access */
    txn->prot_attr = 0x0;
}

/* Initiate AXI write transaction */
static int mgpu_axi_write(struct mgpu_device *mdev, dma_addr_t addr,
                         void *data, size_t size)
{
    struct mgpu_axi_ctrl *ctrl = mgpu_get_axi_ctrl(mdev);
    struct mgpu_axi_transaction txn;
    unsigned long flags;
    int ret = 0;
    
    if (!ctrl)
        return -ENODEV;
    
    /* Initialize transaction */
    memset(&txn, 0, sizeof(txn));
    txn.addr = addr;
    txn.data = data;
    txn.size = size;
    txn.is_write = true;
    init_completion(&txn.completion);
    
    /* Setup burst parameters */
    mgpu_axi_setup_burst(&txn, addr, size);
    
    /* Submit transaction */
    spin_lock_irqsave(&ctrl->lock, flags);
    
    if (ctrl->current_txn) {
        spin_unlock_irqrestore(&ctrl->lock, flags);
        dev_err(mdev->dev, "AXI controller busy\n");
        return -EBUSY;
    }
    
    ctrl->current_txn = &txn;
    ctrl->state = AXI_WRITE_ADDR;
    
    /* Start timeout timer */
    mod_timer(&ctrl->timeout_timer, jiffies + ctrl->timeout_jiffies);
    
    /* Trigger hardware state machine by writing to control register */
    /* Note: In real hardware, this would trigger the AXI FSM */
    mgpu_write(mdev, MGPU_REG_CONTROL, 
               mgpu_read(mdev, MGPU_REG_CONTROL) | MGPU_CTRL_ENABLE);
    
    spin_unlock_irqrestore(&ctrl->lock, flags);
    
    /* Wait for completion */
    ret = wait_for_completion_timeout(&txn.completion,
                                      ctrl->timeout_jiffies);
    if (ret == 0) {
        dev_err(mdev->dev, "AXI write timeout\n");
        return -ETIMEDOUT;
    }
    
    /* Update statistics */
    if (txn.status == 0) {
        ctrl->write_txns++;
        ctrl->write_bytes += size;
    }
    
    return txn.status;
}

/* Initiate AXI read transaction */
static int mgpu_axi_read(struct mgpu_device *mdev, dma_addr_t addr,
                        void *data, size_t size)
{
    struct mgpu_axi_ctrl *ctrl = mgpu_get_axi_ctrl(mdev);
    struct mgpu_axi_transaction txn;
    unsigned long flags;
    int ret = 0;
    
    if (!ctrl)
        return -ENODEV;
    
    /* Initialize transaction */
    memset(&txn, 0, sizeof(txn));
    txn.addr = addr;
    txn.data = data;
    txn.size = size;
    txn.is_write = false;
    init_completion(&txn.completion);
    
    /* Setup burst parameters */
    mgpu_axi_setup_burst(&txn, addr, size);
    
    /* Submit transaction */
    spin_lock_irqsave(&ctrl->lock, flags);
    
    if (ctrl->current_txn) {
        spin_unlock_irqrestore(&ctrl->lock, flags);
        dev_err(mdev->dev, "AXI controller busy\n");
        return -EBUSY;
    }
    
    ctrl->current_txn = &txn;
    ctrl->state = AXI_READ_ADDR;
    
    /* Start timeout timer */
    mod_timer(&ctrl->timeout_timer, jiffies + ctrl->timeout_jiffies);
    
    /* Trigger hardware state machine */
    mgpu_write(mdev, MGPU_REG_CONTROL,
               mgpu_read(mdev, MGPU_REG_CONTROL) | MGPU_CTRL_ENABLE);
    
    spin_unlock_irqrestore(&ctrl->lock, flags);
    
    /* Wait for completion */
    ret = wait_for_completion_timeout(&txn.completion,
                                      ctrl->timeout_jiffies);
    if (ret == 0) {
        dev_err(mdev->dev, "AXI read timeout\n");
        return -ETIMEDOUT;
    }
    
    /* Update statistics */
    if (txn.status == 0) {
        ctrl->read_txns++;
        ctrl->read_bytes += size;
    }
    
    return txn.status;
}

/* Handle AXI response from hardware */
static void mgpu_axi_handle_response(struct mgpu_axi_ctrl *ctrl, u32 resp)
{
    struct mgpu_device *mdev = ctrl->mdev;
    
    /* Check response code */
    switch (resp & 0x3) {
    case AXI_RESP_OKAY:
        /* Success */
        if (ctrl->current_txn) {
            ctrl->current_txn->status = 0;
        }
        break;
        
    case AXI_RESP_EXOKAY:
        /* Exclusive access okay */
        dev_dbg(mdev->dev, "AXI exclusive access okay\n");
        if (ctrl->current_txn) {
            ctrl->current_txn->status = 0;
        }
        break;
        
    case AXI_RESP_SLVERR:
        /* Slave error */
        dev_err(mdev->dev, "AXI slave error at addr 0x%08x\n",
                ctrl->current_txn ? (u32)ctrl->current_txn->addr : 0);
        if (ctrl->current_txn) {
            ctrl->current_txn->status = -EIO;
            ctrl->last_error_addr = ctrl->current_txn->addr;
            ctrl->last_error_resp = resp;
        }
        ctrl->error_count++;
        break;
        
    case AXI_RESP_DECERR:
        /* Decode error */
        dev_err(mdev->dev, "AXI decode error at addr 0x%08x\n",
                ctrl->current_txn ? (u32)ctrl->current_txn->addr : 0);
        if (ctrl->current_txn) {
            ctrl->current_txn->status = -EFAULT;
            ctrl->last_error_addr = ctrl->current_txn->addr;
            ctrl->last_error_resp = resp;
        }
        ctrl->error_count++;
        break;
    }
    
    /* Complete transaction */
    if (ctrl->current_txn) {
        del_timer(&ctrl->timeout_timer);
        complete(&ctrl->current_txn->completion);
        ctrl->current_txn = NULL;
    }
    
    /* Return to idle state */
    ctrl->state = AXI_IDLE;
}

/* AXI interrupt handler (called from main IRQ handler) */
void mgpu_axi_irq_handler(struct mgpu_device *mdev)
{
    struct mgpu_axi_ctrl *ctrl = mgpu_get_axi_ctrl(mdev);
    unsigned long flags;
    u32 status;
    
    if (!ctrl)
        return;
    
    spin_lock_irqsave(&ctrl->lock, flags);
    
    /* Read status to determine what happened */
    status = mgpu_read(mdev, MGPU_REG_STATUS);
    
    /* Handle based on current state */
    switch (ctrl->state) {
    case AXI_WRITE_RESP:
        /* Write response received */
        mgpu_axi_handle_response(ctrl, 0);  /* Response would be in status */
        break;
        
    case AXI_READ_DATA:
        /* Read data received */
        if (ctrl->current_txn && ctrl->current_txn->data) {
            /* In real hardware, we'd read data from AXI data registers */
            /* For now, simulate successful read */
            mgpu_axi_handle_response(ctrl, AXI_RESP_OKAY);
        }
        break;
        
    default:
        /* Unexpected interrupt */
        dev_dbg(mdev->dev, "AXI IRQ in state %d\n", ctrl->state);
        break;
    }
    
    spin_unlock_irqrestore(&ctrl->lock, flags);
}

/* Configure AXI QoS (Quality of Service) */
static int mgpu_axi_set_qos(struct mgpu_device *mdev, u32 priority)
{
    struct mgpu_axi_ctrl *ctrl = mgpu_get_axi_ctrl(mdev);
    
    if (!ctrl)
        return -ENODEV;
    
    /* Priority levels 0-15 */
    if (priority > 15) {
        dev_err(mdev->dev, "Invalid AXI QoS priority %u\n", priority);
        return -EINVAL;
    }
    
    /* In hardware, this would set AxQOS signals */
    /* For Red Pitaya, QoS may be handled by the PS interconnect */
    dev_dbg(mdev->dev, "Set AXI QoS priority to %u\n", priority);
    
    return 0;
}

/* Memory barrier for AXI transactions */
static void mgpu_axi_memory_barrier(struct mgpu_device *mdev)
{
    /* Ensure all previous AXI transactions are complete */
    mb();
    
    /* In hardware with AxUSER signals, we could issue a barrier transaction */
    /* For now, just ensure write buffer is flushed */
    wmb();
}

/* DMA transfer via AXI */
int mgpu_axi_dma_transfer(struct mgpu_device *mdev, dma_addr_t src,
                          dma_addr_t dst, size_t size, bool blocking)
{
    void *buffer;
    int ret;
    
    /* Validate parameters */
    if (!size || size > (16 * 1024 * 1024)) {
        dev_err(mdev->dev, "Invalid DMA size: %zu\n", size);
        return -EINVAL;
    }
    
    /* Check alignment (AXI requires 4-byte alignment minimum) */
    if ((src & 3) || (dst & 3) || (size & 3)) {
        dev_err(mdev->dev, "DMA addresses/size must be 4-byte aligned\n");
        return -EINVAL;
    }
    
    /* For large transfers, break into chunks */
    if (size > PAGE_SIZE) {
        size_t remaining = size;
        size_t chunk_size = PAGE_SIZE;
        
        while (remaining > 0) {
            if (remaining < chunk_size)
                chunk_size = remaining;
            
            ret = mgpu_axi_dma_transfer(mdev, src, dst, chunk_size, blocking);
            if (ret)
                return ret;
            
            src += chunk_size;
            dst += chunk_size;
            remaining -= chunk_size;
        }
        
        return 0;
    }
    
    /* Allocate temporary buffer */
    buffer = kmalloc(size, GFP_KERNEL);
    if (!buffer)
        return -ENOMEM;
    
    /* Read from source */
    ret = mgpu_axi_read(mdev, src, buffer, size);
    if (ret) {
        kfree(buffer);
        return ret;
    }
    
    /* Write to destination */
    ret = mgpu_axi_write(mdev, dst, buffer, size);
    
    kfree(buffer);
    return ret;
}

/* Test AXI connectivity */
static int mgpu_axi_test(struct mgpu_device *mdev)
{
    u32 test_pattern = 0xDEADBEEF;
    u32 readback;
    int ret;
    
    dev_dbg(mdev->dev, "Testing AXI connectivity\n");
    
    /* Test register access via AXI4-Lite */
    mgpu_write(mdev, MGPU_REG_SCRATCH, test_pattern);
    readback = mgpu_read(mdev, MGPU_REG_SCRATCH);
    
    if (readback != test_pattern) {
        dev_err(mdev->dev, "AXI register test failed: wrote 0x%08x, read 0x%08x\n",
                test_pattern, readback);
        return -EIO;
    }
    
    /* Test with inverted pattern */
    test_pattern = ~test_pattern;
    mgpu_write(mdev, MGPU_REG_SCRATCH, test_pattern);
    readback = mgpu_read(mdev, MGPU_REG_SCRATCH);
    
    if (readback != test_pattern) {
        dev_err(mdev->dev, "AXI register test failed (inverted)\n");
        return -EIO;
    }
    
    dev_dbg(mdev->dev, "AXI connectivity test passed\n");
    
    return 0;
}

/* Parse AXI configuration from device tree */
static int mgpu_axi_parse_dt(struct mgpu_device *mdev)
{
    struct device_node *np = mdev->dev->of_node;
    struct mgpu_axi_ctrl *ctrl = mgpu_get_axi_ctrl(mdev);
    u32 val;
    
    if (!np || !ctrl)
        return -ENODEV;
    
    /* Get AXI bus widths */
    if (of_property_read_u32(np, "xlnx,axi-data-width", &val) == 0) {
        ctrl->data_width = val;
    } else {
        ctrl->data_width = 32;  /* Default from axi_wrapper.sv */
    }
    
    if (of_property_read_u32(np, "xlnx,axi-addr-width", &val) == 0) {
        ctrl->addr_width = val;
    } else {
        ctrl->addr_width = 32;  /* Default from axi_wrapper.sv */
    }
    
    if (of_property_read_u32(np, "xlnx,axi-id-width", &val) == 0) {
        ctrl->id_width = val;
    } else {
        ctrl->id_width = 4;  /* Default from axi_wrapper.sv */
    }
    
    /* Get maximum burst length */
    if (of_property_read_u32(np, "xlnx,max-burst-len", &val) == 0) {
        ctrl->max_burst_len = val;
    } else {
        ctrl->max_burst_len = 256;  /* Default: 256 beats */
    }
    
    dev_info(mdev->dev, "AXI configuration: data_width=%u, addr_width=%u, "
             "id_width=%u, max_burst=%u\n",
             ctrl->data_width, ctrl->addr_width,
             ctrl->id_width, ctrl->max_burst_len);
    
    return 0;
}

/* Initialize AXI transport */
int mgpu_axi_init(struct mgpu_device *mdev)
{
    struct mgpu_axi_ctrl *ctrl;
    int ret;
    
    dev_info(mdev->dev, "Initializing AXI transport\n");
    
    /* Allocate controller structure */
    ctrl = kzalloc(sizeof(*ctrl), GFP_KERNEL);
    if (!ctrl)
        return -ENOMEM;
    
    ctrl->mdev = mdev;
    spin_lock_init(&ctrl->lock);
    ctrl->state = AXI_IDLE;
    ctrl->timeout_jiffies = msecs_to_jiffies(1000);  /* 1 second timeout */
    
    /* Initialize timeout timer */
    timer_setup(&ctrl->timeout_timer, mgpu_axi_timeout, 0);
    
    /* Store in device */
    mdev->axi_ctrl = ctrl;
    
    /* Parse device tree configuration */
    ret = mgpu_axi_parse_dt(mdev);
    if (ret) {
        dev_warn(mdev->dev, "Failed to parse AXI DT config, using defaults\n");
    }
    
    /* Test AXI connectivity */
    ret = mgpu_axi_test(mdev);
    if (ret) {
        dev_err(mdev->dev, "AXI connectivity test failed\n");
        goto err_test;
    }
    
    /* Set default QoS */
    mgpu_axi_set_qos(mdev, 8);  /* Medium priority */
    
    dev_info(mdev->dev, "AXI transport initialized successfully\n");
    
    return 0;
    
err_test:
    del_timer_sync(&ctrl->timeout_timer);
    kfree(ctrl);
    mdev->axi_ctrl = NULL;
    return ret;
}

/* Cleanup AXI transport */
void mgpu_axi_fini(struct mgpu_device *mdev)
{
    struct mgpu_axi_ctrl *ctrl = mgpu_get_axi_ctrl(mdev);
    
    if (!ctrl)
        return;
    
    dev_info(mdev->dev, "Shutting down AXI transport\n");
    
    /* Cancel timeout timer */
    del_timer_sync(&ctrl->timeout_timer);
    
    /* Wait for any pending transactions */
    if (ctrl->current_txn) {
        ctrl->current_txn->status = -ECANCELED;
        complete(&ctrl->current_txn->completion);
    }
    
    /* Log statistics */
    dev_info(mdev->dev, "AXI stats: read_txns=%llu, write_txns=%llu, "
             "read_bytes=%llu, write_bytes=%llu, errors=%llu\n",
             ctrl->read_txns, ctrl->write_txns,
             ctrl->read_bytes, ctrl->write_bytes,
             ctrl->error_count);
    
    kfree(ctrl);
    mdev->axi_ctrl = NULL;
}

/* Suspend AXI operations */
int mgpu_axi_suspend(struct mgpu_device *mdev)
{
    struct mgpu_axi_ctrl *ctrl = mgpu_get_axi_ctrl(mdev);
    unsigned long flags;
    
    if (!ctrl)
        return 0;
    
    dev_dbg(mdev->dev, "Suspending AXI transport\n");
    
    spin_lock_irqsave(&ctrl->lock, flags);
    
    /* Wait for current transaction to complete */
    if (ctrl->current_txn) {
        spin_unlock_irqrestore(&ctrl->lock, flags);
        wait_for_completion_timeout(&ctrl->current_txn->completion,
                                    ctrl->timeout_jiffies);
        spin_lock_irqsave(&ctrl->lock, flags);
    }
    
    /* Ensure we're in idle state */
    ctrl->state = AXI_IDLE;
    
    spin_unlock_irqrestore(&ctrl->lock, flags);
    
    /* Cancel timeout timer */
    del_timer_sync(&ctrl->timeout_timer);
    
    return 0;
}

/* Resume AXI operations */
int mgpu_axi_resume(struct mgpu_device *mdev)
{
    struct mgpu_axi_ctrl *ctrl = mgpu_get_axi_ctrl(mdev);
    int ret;
    
    if (!ctrl)
        return 0;
    
    dev_dbg(mdev->dev, "Resuming AXI transport\n");
    
    /* Reset state */
    ctrl->state = AXI_IDLE;
    ctrl->current_txn = NULL;
    
    /* Test connectivity */
    ret = mgpu_axi_test(mdev);
    if (ret) {
        dev_err(mdev->dev, "AXI connectivity test failed after resume\n");
        return ret;
    }
    
    return 0;
}

/* Get AXI statistics */
void mgpu_axi_get_stats(struct mgpu_device *mdev, struct mgpu_axi_stats *stats)
{
    struct mgpu_axi_ctrl *ctrl = mgpu_get_axi_ctrl(mdev);
    
    if (!ctrl || !stats)
        return;
    
    stats->read_transactions = ctrl->read_txns;
    stats->write_transactions = ctrl->write_txns;
    stats->read_bytes = ctrl->read_bytes;
    stats->write_bytes = ctrl->write_bytes;
    stats->error_count = ctrl->error_count;
    stats->last_error_addr = ctrl->last_error_addr;
    stats->last_error_resp = ctrl->last_error_resp;
    stats->current_state = ctrl->state;
}

/* Reset AXI statistics */
void mgpu_axi_reset_stats(struct mgpu_device *mdev)
{
    struct mgpu_axi_ctrl *ctrl = mgpu_get_axi_ctrl(mdev);
    
    if (!ctrl)
        return;
    
    ctrl->read_txns = 0;
    ctrl->write_txns = 0;
    ctrl->read_bytes = 0;
    ctrl->write_bytes = 0;
    ctrl->error_count = 0;
}

MODULE_DESCRIPTION("MGPU AXI Transport Backend");
MODULE_AUTHOR("Rafeed Khan");
MODULE_LICENSE("GPL v2");