/*
 * MGPU Memory Map and MMIO Definitions
 * Based on actual FPGA hardware implementation
 *
 * Memory regions from gpu_top.sv and axi_wrapper.sv:
 * - Control/Status registers: 0x00000000 - 0x00000FFF
 * - Instruction memory:        0x00001000 - 0x00001FFF  
 * - Doorbell region:          0x00002000 - 0x00002FFF
 * - Reserved:                 0x00003000 - 0x0000FFFF
 *
 * Copyright (C) 2025
 */

#ifndef _MGPU_MMIO_H_
#define _MGPU_MMIO_H_

#include <linux/types.h>
#include <linux/io.h>

/* ==================================================================
 * Memory Map Layout (from gpu_top.sv ADDR_* parameters)
 * ================================================================== */

/* Base addresses for different register groups */
#define MGPU_MMIO_BASE              0x00000000
#define MGPU_MMIO_SIZE              0x00010000  /* 64KB total */

/* Control/Status Register Space */
#define MGPU_REG_SPACE_BASE         0x00000000
#define MGPU_REG_SPACE_SIZE         0x00001000  /* 4KB */

/* Instruction Memory Window (shader_loader.sv) */
#define MGPU_INSTR_MEM_BASE         0x00001000
#define MGPU_INSTR_MEM_SIZE         0x00001000  /* 4KB = 1024 instructions */
#define MGPU_INSTR_MEM_SLOTS        16          /* 16 shader slots */
#define MGPU_INSTR_SLOT_SIZE        256         /* 256 dwords per slot */

/* Doorbell Region (for queue notifications) */
#define MGPU_DOORBELL_BASE          0x00002000
#define MGPU_DOORBELL_SIZE          0x00001000  /* 4KB */
#define MGPU_DOORBELL_COUNT         16          /* Max 16 doorbells */
#define MGPU_DOORBELL_STRIDE        4           /* 4 bytes per doorbell */

/* Reserved/Future Expansion */
#define MGPU_RESERVED_BASE          0x00003000
#define MGPU_RESERVED_SIZE          0x0000D000  /* Remaining space */

/* ==================================================================
 * Register Offsets (from controller.sv, gpu_top.sv)
 * ================================================================== */

/* Base Registers (0x0000 - 0x001F) */
#define MGPU_REG_VERSION            0x0000  /* RO: Hardware version */
#define MGPU_REG_CAPS               0x0004  /* RO: Capabilities */
#define MGPU_REG_CONTROL            0x0008  /* RW: Control register */
#define MGPU_REG_STATUS             0x000C  /* RO: Status register */
#define MGPU_REG_SCRATCH            0x0010  /* RW: Scratch register */

/* Interrupt Registers (0x0020 - 0x002F) */
#define MGPU_REG_IRQ_STATUS         0x0020  /* RO: Interrupt status */
#define MGPU_REG_IRQ_ENABLE         0x0024  /* RW: Interrupt enable */
#define MGPU_REG_IRQ_ACK            0x0028  /* WO: Interrupt acknowledge */

/* Command Queue Registers (0x0040 - 0x005F) */
#define MGPU_REG_CMD_BASE           0x0040  /* RW: Command buffer base */
#define MGPU_REG_CMD_SIZE           0x0044  /* RW: Command buffer size */
#define MGPU_REG_CMD_HEAD           0x0048  /* RO: Command head (GPU read ptr) */
#define MGPU_REG_CMD_TAIL           0x004C  /* RW: Command tail (CPU write ptr) */
#define MGPU_REG_CMD_DOORBELL       0x0050  /* WO: Command doorbell */

/* Fence Registers (0x0060 - 0x006F) */
#define MGPU_REG_FENCE_ADDR         0x0060  /* RW: Fence memory address */
#define MGPU_REG_FENCE_VALUE        0x0064  /* RO: Current fence value */

/* Vertex Processing Registers (0x0080 - 0x008F) */
#define MGPU_REG_VERTEX_BASE        0x0080  /* RW: Vertex buffer base */
#define MGPU_REG_VERTEX_COUNT       0x0084  /* RW: Vertex count */
#define MGPU_REG_VERTEX_STRIDE      0x0088  /* RW: Vertex stride (default 44) */

/* Shader Registers (0x00A0 - 0x00AF) */
#define MGPU_REG_SHADER_PC          0x00A0  /* RW: Shader program counter */
#define MGPU_REG_SHADER_ADDR        0x00A4  /* RW: Shader memory address */
#define MGPU_REG_SHADER_DATA        0x00A8  /* RW: Shader data port */
#define MGPU_REG_SHADER_CTRL        0x00AC  /* RW: Shader control */

/* Performance Counters (0x00C0 - 0x00CF) */
#define MGPU_REG_PERF_CTRL          0x00C0  /* RW: Performance control */
#define MGPU_REG_PERF_SELECT        0x00C4  /* RW: Counter select */
/* Note: No actual counter data registers in hardware */

/* Multi-Queue Support (0x0100 - 0x01FF) */
#define MGPU_REG_QUEUE_BASE(n)      (0x0100 + ((n) * 0x10))
#define MGPU_REG_QUEUE_SIZE(n)      (0x0104 + ((n) * 0x10))
#define MGPU_REG_QUEUE_HEAD(n)      (0x0108 + ((n) * 0x10))
#define MGPU_REG_QUEUE_TAIL(n)      (0x010C + ((n) * 0x10))

/* ==================================================================
 * Register Bit Definitions
 * ================================================================== */

/* CONTROL Register Bits (controller.sv) */
#define MGPU_CTRL_ENABLE            BIT(0)   /* Enable GPU */
#define MGPU_CTRL_RESET             BIT(1)   /* Software reset */
#define MGPU_CTRL_PAUSE             BIT(2)   /* Pause execution */
#define MGPU_CTRL_SINGLE_STEP       BIT(3)   /* Single step mode */
#define MGPU_CTRL_FLUSH_CACHE       BIT(4)   /* Flush caches */
#define MGPU_CTRL_PERF_COUNTER      BIT(5)   /* Enable perf counters */
#define MGPU_CTRL_START_PIPELINE    BIT(8)   /* Start pipeline (controller.sv) */
#define MGPU_CTRL_IRQ_CLEAR         BIT(9)   /* Clear IRQ (controller.sv) */

/* STATUS Register Bits */
#define MGPU_STATUS_IDLE            BIT(0)   /* GPU idle */
#define MGPU_STATUS_BUSY            BIT(1)   /* GPU busy */
#define MGPU_STATUS_ERROR           BIT(2)   /* Error occurred */
#define MGPU_STATUS_HALTED          BIT(3)   /* GPU halted */
#define MGPU_STATUS_FENCE_DONE      BIT(4)   /* Fence completed */
#define MGPU_STATUS_CMD_EMPTY       BIT(5)   /* Command queue empty */
#define MGPU_STATUS_CMD_FULL        BIT(6)   /* Command queue full */
#define MGPU_STATUS_PIPELINE_BUSY   BIT(8)   /* Pipeline busy (controller.sv) */
#define MGPU_STATUS_IRQ_PENDING     BIT(9)   /* IRQ pending (controller.sv) */
#define MGPU_STATUS_QUEUE_PENDING   BIT(10)  /* Queue has pending starts */

/* Queue count field in STATUS register (controller.sv) */
#define MGPU_STATUS_QUEUE_COUNT_SHIFT  4
#define MGPU_STATUS_QUEUE_COUNT_MASK   0xF   /* 4 bits for queue depth */

/* IRQ Status/Enable Bits */
#define MGPU_IRQ_CMD_COMPLETE       BIT(0)   /* Command completed */
#define MGPU_IRQ_ERROR              BIT(1)   /* Error interrupt */
#define MGPU_IRQ_FENCE              BIT(2)   /* Fence reached */
#define MGPU_IRQ_QUEUE_EMPTY        BIT(3)   /* Queue empty */
#define MGPU_IRQ_SHADER_HALT        BIT(4)   /* Shader halted */
#define MGPU_IRQ_PERF_COUNTER       BIT(5)   /* Perf counter overflow */
#define MGPU_IRQ_PIPELINE_DONE      BIT(8)   /* Pipeline completed */

/* CAPS Register Bits (capabilities) */
#define MGPU_CAP_VERTEX_SHADER      BIT(0)   /* Has vertex shader */
#define MGPU_CAP_FRAGMENT_SHADER    BIT(1)   /* Has fragment shader */
#define MGPU_CAP_TEXTURE            BIT(2)   /* Has texture unit */
#define MGPU_CAP_FLOAT16            BIT(3)   /* Float16 support */
#define MGPU_CAP_FLOAT32            BIT(4)   /* Float32 support */
#define MGPU_CAP_INT32              BIT(5)   /* Int32 support */
#define MGPU_CAP_ATOMIC             BIT(6)   /* Atomic operations */
#define MGPU_CAP_FENCE              BIT(7)   /* Fence support */
#define MGPU_CAP_MULTI_QUEUE        BIT(8)   /* Multiple queues */
#define MGPU_CAP_PREEMPTION         BIT(9)   /* Preemption support */
#define MGPU_CAP_COMPUTE            BIT(10)  /* Compute shaders */
#define MGPU_CAP_RASTERIZER         BIT(11)  /* Has rasterizer */
#define MGPU_CAP_DEPTH_TEST         BIT(12)  /* Depth testing */
#define MGPU_CAP_BLENDING           BIT(13)  /* Alpha blending */

/* Shader Control Register (shader_loader.sv) */
#define MGPU_SHADER_CTRL_SLOT_SHIFT    16    /* Shader slot number */
#define MGPU_SHADER_CTRL_SLOT_MASK     0xF   /* 4 bits = 16 slots */
#define MGPU_SHADER_CTRL_SIZE_MASK     0xFFFF /* Size in dwords */

/* ==================================================================
 * Hardware Constants (from .sv files)
 * ================================================================== */

/* From controller.sv */
#define MGPU_QUEUE_DEPTH            16       /* Default queue depth */
#define MGPU_MAX_QUEUES             16       /* Maximum queues supported */
#define MGPU_MAX_ENGINES            4        /* Maximum engines */

/* From vertex_fetch.sv */
#define MGPU_VERTEX_ATTR_COUNT      11       /* Attributes per vertex */
#define MGPU_VERTEX_ATTR_WIDTH      32       /* 32 bits per attribute */
#define MGPU_VERTEX_DEFAULT_STRIDE  44       /* 11 * 4 bytes */

/* From rasterizer.sv */
#define MGPU_RASTER_COORD_WIDTH     10       /* 10-bit coordinates */
#define MGPU_RASTER_MAX_X           1023     /* Max X coordinate */
#define MGPU_RASTER_MAX_Y           1023     /* Max Y coordinate */

/* From framebuffer.sv */
#define MGPU_FB_WIDTH               640      /* Fixed width */
#define MGPU_FB_HEIGHT              480      /* Fixed height */
#define MGPU_FB_COLOR_WIDTH         32       /* 32-bit color */
#define MGPU_FB_PIXELS              (MGPU_FB_WIDTH * MGPU_FB_HEIGHT)

/* From texture_unit.sv */
#define MGPU_TEX_WIDTH              256      /* Texture width */
#define MGPU_TEX_HEIGHT             256      /* Texture height */
#define MGPU_TEX_COORD_WIDTH        16       /* Coordinate precision */

/* From fifo.sv */
#define MGPU_FIFO_DEPTH             64       /* FIFO depth */
#define MGPU_FIFO_DATA_WIDTH        32       /* Data width */

/* From shader_core.sv */
#define MGPU_SHADER_NUM_REGS        16       /* GPRs per shader core */
#define MGPU_SHADER_VEC_SIZE        4        /* Vector width */
#define MGPU_SHADER_DATA_WIDTH      32       /* 32-bit data */

/* From alu.sv opcodes */
#define MGPU_ALU_OP_ADD             0x01     /* Addition */
#define MGPU_ALU_OP_SUB             0x02     /* Subtraction */
#define MGPU_ALU_OP_MUL             0x03     /* Multiplication */
#define MGPU_ALU_OP_AND             0x09     /* Bitwise AND */
#define MGPU_ALU_OP_OR              0x0A     /* Bitwise OR */
#define MGPU_ALU_OP_XOR             0x0B     /* Bitwise XOR */
#define MGPU_ALU_OP_MOV_A           0x11     /* Move operand A */
#define MGPU_ALU_OP_MOV_B           0x12     /* Move operand B */

/* ==================================================================
 * Command Format Definitions (from hardware implementation)
 * ================================================================== */

/* Command header format */
struct mgpu_cmd_header {
    u32 opcode : 8;      /* Command opcode */
    u32 size : 8;        /* Size in dwords */
    u32 flags : 16;      /* Command flags */
} __packed;

/* Command opcodes */
#define MGPU_CMD_NOP                0x00
#define MGPU_CMD_DRAW               0x01
#define MGPU_CMD_COMPUTE            0x02
#define MGPU_CMD_DMA                0x03
#define MGPU_CMD_FENCE              0x04
#define MGPU_CMD_WAIT               0x05
#define MGPU_CMD_REG_WRITE          0x06
#define MGPU_CMD_REG_READ           0x07
#define MGPU_CMD_TIMESTAMP          0x08
#define MGPU_CMD_FLUSH              0x09

/* ==================================================================
 * Access Helpers and Macros
 * ================================================================== */

/* Register access helpers */
static inline u32 mgpu_mmio_read32(void __iomem *base, u32 offset)
{
    return ioread32(base + offset);
}

static inline void mgpu_mmio_write32(void __iomem *base, u32 offset, u32 value)
{
    iowrite32(value, base + offset);
}

/* Doorbell helpers */
static inline void mgpu_ring_doorbell(void __iomem *base, u32 queue_id)
{
    iowrite32(1, base + MGPU_DOORBELL_BASE + (queue_id * MGPU_DOORBELL_STRIDE));
}

/* Instruction memory access helpers */
static inline void mgpu_write_instruction(void __iomem *base, u32 slot, u32 offset, u32 instr)
{
    u32 addr = (slot * MGPU_INSTR_SLOT_SIZE) + offset;
    mgpu_mmio_write32(base, MGPU_REG_SHADER_ADDR, addr);
    mgpu_mmio_write32(base, MGPU_REG_SHADER_DATA, instr);
}

static inline u32 mgpu_read_instruction(void __iomem *base, u32 slot, u32 offset)
{
    u32 addr = (slot * MGPU_INSTR_SLOT_SIZE) + offset;
    mgpu_mmio_write32(base, MGPU_REG_SHADER_ADDR, addr);
    return mgpu_mmio_read32(base, MGPU_REG_SHADER_DATA);
}

/* Status checking helpers */
static inline bool mgpu_is_idle(void __iomem *base)
{
    u32 status = mgpu_mmio_read32(base, MGPU_REG_STATUS);
    return (status & MGPU_STATUS_IDLE) && !(status & MGPU_STATUS_BUSY);
}

static inline bool mgpu_has_error(void __iomem *base)
{
    u32 status = mgpu_mmio_read32(base, MGPU_REG_STATUS);
    return !!(status & (MGPU_STATUS_ERROR | MGPU_STATUS_HALTED));
}

static inline u32 mgpu_get_queue_depth(void __iomem *base)
{
    u32 status = mgpu_mmio_read32(base, MGPU_REG_STATUS);
    return (status >> MGPU_STATUS_QUEUE_COUNT_SHIFT) & MGPU_STATUS_QUEUE_COUNT_MASK;
}

/* Memory barriers for MMIO operations */
#define mgpu_mmio_mb()      mb()
#define mgpu_mmio_rmb()     rmb()
#define mgpu_mmio_wmb()     wmb()

/* Debug register dump macro */
#ifdef CONFIG_MGPU_DEBUG
#define MGPU_DUMP_REG(base, reg) \
    pr_debug("MGPU: " #reg " = 0x%08x\n", mgpu_mmio_read32(base, reg))
#else
#define MGPU_DUMP_REG(base, reg) do {} while (0)
#endif

/* ==================================================================
 * Version Helpers
 * ================================================================== */

#define MGPU_VERSION_MAJOR(v)       (((v) >> 24) & 0xFF)
#define MGPU_VERSION_MINOR(v)       (((v) >> 16) & 0xFF)
#define MGPU_VERSION_PATCH(v)       (((v) >> 8) & 0xFF)
#define MGPU_VERSION_BUILD(v)       ((v) & 0xFF)

#define MGPU_MAKE_VERSION(maj, min, patch, build) \
    ((((maj) & 0xFF) << 24) | \
     (((min) & 0xFF) << 16) | \
     (((patch) & 0xFF) << 8) | \
     ((build) & 0xFF))

/* ==================================================================
 * Error Codes (hardware-reported)
 * ================================================================== */

#define MGPU_ERROR_NONE             0x00
#define MGPU_ERROR_INVALID_CMD      0x01
#define MGPU_ERROR_MEM_FAULT        0x02
#define MGPU_ERROR_SHADER_FAULT     0x03
#define MGPU_ERROR_TIMEOUT          0x04
#define MGPU_ERROR_OVERFLOW         0x05
#define MGPU_ERROR_UNDERFLOW        0x06
#define MGPU_ERROR_INVALID_ADDR     0x07
#define MGPU_ERROR_INVALID_OP       0x08

/* ==================================================================
 * Utility Macros
 * ================================================================== */

/* Missing kernel helpers for older kernels */
#ifndef lower_32_bits
#define lower_32_bits(n) ((u32)(n))
#endif

#ifndef upper_32_bits
#define upper_32_bits(n) ((u32)(((n) >> 16) >> 16))
#endif

#ifndef BIT
#define BIT(nr) (1UL << (nr))
#endif

#ifndef GENMASK
#define GENMASK(h, l) (((~0UL) << (l)) & (~0UL >> (BITS_PER_LONG - 1 - (h))))
#endif

/* Field extraction/insertion */
#define MGPU_FIELD_GET(val, mask, shift) (((val) >> (shift)) & (mask))
#define MGPU_FIELD_SET(val, mask, shift) (((val) & (mask)) << (shift))

/* Alignment macros */
#define MGPU_ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))
#define MGPU_IS_ALIGNED(x, a) (((x) & ((a) - 1)) == 0)

#endif /* _MGPU_MMIO_H_ */
