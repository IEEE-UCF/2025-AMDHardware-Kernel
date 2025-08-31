diff --git a/kernel/drivers/gpu/mgpu/mgpu_dma.c b/kernel/drivers/gpu/mgpu/mgpu_dma.c
index 2ef3c7cd75f187c16ecd2a03cec1b94df8baccea..c706f96e9b9c584c71bf7e6be7815dcb88b95c0a 100644
--- a/kernel/drivers/gpu/mgpu/mgpu_dma.c
+++ b/kernel/drivers/gpu/mgpu/mgpu_dma.c
@@ -1,6 +1,203 @@
-/* mgpu_dma.c - GEN1 skeleton */
+/* mgpu_dma.c - DMA helper implementation */
 #include <linux/module.h>
-#include <linux/kernel.h>
-#include "mgpu_mmio.h"
+#include <linux/slab.h>
+#include <linux/dma-mapping.h>
+#include <linux/delay.h>
 
-/* TODO: implement mgpu_dma */
+#include "mgpu_drm.h"
+#include "mgpu_regs.h"
+#include "mgpu_internal.h"
+
+/* Local copy of the command ring structure (see mgpu_cmdq.c) */
+struct mgpu_ring {
+    struct mgpu_device *mdev;
+
+    void *vaddr;
+    dma_addr_t dma_addr;
+    size_t size;
+
+    u32 head;          /* GPU read pointer */
+    u32 tail;          /* CPU write pointer */
+
+    u32 queue_id;
+
+    u64 submitted_cmds;
+    u64 completed_cmds;
+};
+
+/* ------- Ring helpers (duplicated from mgpu_cmdq.c) ------- */
+
+/* Get available space in ring in dwords */
+static u32 mgpu_ring_space(struct mgpu_ring *ring)
+{
+    u32 head, tail, space;
+
+    head = mgpu_read(ring->mdev, MGPU_REG_CMD_HEAD + (ring->queue_id * 0x10));
+    tail = ring->tail;
+
+    if (head <= tail)
+        space = ring->size - (tail - head) - 1;
+    else
+        space = head - tail - 1;
+
+    return space;
+}
+
+/* Wait for space in ring */
+static int mgpu_ring_wait_space(struct mgpu_ring *ring, u32 needed)
+{
+    int timeout = 1000; /* ms */
+
+    while (mgpu_ring_space(ring) < needed) {
+        if (timeout-- <= 0) {
+            dev_err(ring->mdev->dev,
+                    "Ring %u timeout waiting for space\n",
+                    ring->queue_id);
+            return -ETIMEDOUT;
+        }
+        msleep(1);
+    }
+
+    return 0;
+}
+
+/* Write data to ring */
+static void mgpu_ring_write(struct mgpu_ring *ring, const u32 *data,
+                            u32 dwords)
+{
+    u32 *ring_ptr = ring->vaddr;
+    u32 tail = ring->tail;
+    u32 ring_size_dw = ring->size / 4;
+    u32 i;
+
+    for (i = 0; i < dwords; i++) {
+        ring_ptr[tail] = data[i];
+        tail = (tail + 1) & (ring_size_dw - 1);
+    }
+
+    /* Ensure data visible before updating tail */
+    wmb();
+
+    ring->tail = tail;
+}
+
+/* Kick ring (doorbell) */
+static void mgpu_ring_kick(struct mgpu_ring *ring)
+{
+    struct mgpu_device *mdev = ring->mdev;
+
+    mgpu_write(mdev, MGPU_REG_CMD_TAIL + (ring->queue_id * 0x10), ring->tail);
+    mgpu_write(mdev, MGPU_REG_DOORBELL(ring->queue_id), 1);
+    ring->submitted_cmds++;
+}
+
+/* ----------------- DMA subsystem ----------------- */
+
+int mgpu_dma_init(struct mgpu_device *mdev)
+{
+    int ret;
+
+    /* Prefer 64-bit DMA, fall back to 32-bit */
+    ret = dma_set_mask_and_coherent(mdev->dev, DMA_BIT_MASK(64));
+    if (ret) {
+        ret = dma_set_mask_and_coherent(mdev->dev, DMA_BIT_MASK(32));
+        if (ret) {
+            dev_err(mdev->dev, "No suitable DMA configuration\n");
+            return ret;
+        }
+        dev_info(mdev->dev, "Using 32-bit DMA addressing\n");
+    } else {
+        dev_info(mdev->dev, "Using 64-bit DMA addressing\n");
+    }
+
+    return 0;
+}
+EXPORT_SYMBOL_GPL(mgpu_dma_init);
+
+void mgpu_dma_fini(struct mgpu_device *mdev)
+{
+    /* Nothing to do for now */
+}
+EXPORT_SYMBOL_GPL(mgpu_dma_fini);
+
+void *mgpu_dma_alloc(struct mgpu_device *mdev, size_t size,
+                     dma_addr_t *dma_addr, gfp_t gfp, unsigned long attrs)
+{
+    void *vaddr;
+
+    vaddr = dma_alloc_attrs(mdev->dev, size, dma_addr, gfp, attrs);
+    if (!vaddr)
+        dev_err(mdev->dev, "Failed to allocate %zu bytes of DMA memory\n",
+                size);
+
+    return vaddr;
+}
+EXPORT_SYMBOL_GPL(mgpu_dma_alloc);
+
+void mgpu_dma_free(struct mgpu_device *mdev, size_t size,
+                   void *vaddr, dma_addr_t dma_addr, unsigned long attrs)
+{
+    if (vaddr)
+        dma_free_attrs(mdev->dev, size, vaddr, dma_addr, attrs);
+}
+EXPORT_SYMBOL_GPL(mgpu_dma_free);
+
+void mgpu_dma_sync_for_cpu(struct mgpu_device *mdev, dma_addr_t dma_addr,
+                           size_t size, enum dma_data_direction dir)
+{
+    dma_sync_single_for_cpu(mdev->dev, dma_addr, size, dir);
+}
+EXPORT_SYMBOL_GPL(mgpu_dma_sync_for_cpu);
+
+void mgpu_dma_sync_for_device(struct mgpu_device *mdev, dma_addr_t dma_addr,
+                              size_t size, enum dma_data_direction dir)
+{
+    dma_sync_single_for_device(mdev->dev, dma_addr, size, dir);
+}
+EXPORT_SYMBOL_GPL(mgpu_dma_sync_for_device);
+
+/* Submit a DMA copy command to the GPU. If wait is true, wait for completion. */
+int mgpu_dma_copy(struct mgpu_device *mdev, dma_addr_t src, dma_addr_t dst,
+                  u32 size, bool wait)
+{
+    struct mgpu_ring *ring = mdev->cmd_ring;
+    struct mgpu_cmd_dma cmd = {
+        .header = {
+            .opcode = MGPU_CMD_DMA,
+            .size = sizeof(struct mgpu_cmd_dma) / 4,
+            .flags = 0,
+        },
+        .src_addr = lower_32_bits(src),
+        .dst_addr = lower_32_bits(dst),
+        .size = size,
+        .flags = 0,
+    };
+    unsigned long flags;
+    int ret;
+
+    if (!ring)
+        return -ENODEV;
+
+    spin_lock_irqsave(&mdev->cmd_lock, flags);
+
+    ret = mgpu_ring_wait_space(ring, sizeof(cmd) / 4);
+    if (!ret) {
+        mgpu_ring_write(ring, (u32 *)&cmd, sizeof(cmd) / 4);
+        mgpu_ring_kick(ring);
+    }
+
+    spin_unlock_irqrestore(&mdev->cmd_lock, flags);
+
+    if (ret)
+        return ret;
+
+    if (wait)
+        ret = mgpu_core_wait_idle(mdev, 1000); /* wait up to 1s */
+
+    return ret;
+}
+EXPORT_SYMBOL_GPL(mgpu_dma_copy);
+
+MODULE_DESCRIPTION("mGPU DMA helpers");
+MODULE_AUTHOR("Johnny Doe");
+MODULE_LICENSE("GPL");
