#ifndef _MGPU_REGS_H_
#define _MGPU_REGS_H_

/* Base register offsets - from registers.yaml */
#define MGPU_REG_VERSION        0x0000
#define MGPU_REG_CAPS           0x0004
#define MGPU_REG_CONTROL        0x0008
#define MGPU_REG_STATUS         0x000C
#define MGPU_REG_SCRATCH        0x0010

/* Interrupt registers */
#define MGPU_REG_IRQ_STATUS     0x0020
#define MGPU_REG_IRQ_ENABLE     0x0024
#define MGPU_REG_IRQ_ACK        0x0028

/* Command queue registers */
#define MGPU_REG_CMD_BASE       0x0040
#define MGPU_REG_CMD_SIZE       0x0044
#define MGPU_REG_CMD_HEAD       0x0048
#define MGPU_REG_CMD_TAIL       0x004C
#define MGPU_REG_CMD_DOORBELL   0x0050

/* Fence registers */
#define MGPU_REG_FENCE_ADDR     0x0060
#define MGPU_REG_FENCE_VALUE    0x0064

/* Vertex registers */
#define MGPU_REG_VERTEX_BASE    0x0080
#define MGPU_REG_VERTEX_COUNT   0x0084
#define MGPU_REG_VERTEX_STRIDE  0x0088

/* Shader registers */
#define MGPU_REG_SHADER_PC      0x00A0
#define MGPU_REG_SHADER_ADDR    0x00A4
#define MGPU_REG_SHADER_DATA    0x00A8
#define MGPU_REG_SHADER_CTRL    0x00AC

/* Instruction memory window */
#define MGPU_REG_INSTR_MEM_BASE 0x1000
#define MGPU_REG_INSTR_MEM_SIZE 0x1000  /* 4KB */

/* Doorbell region */
#define MGPU_REG_DOORBELL_BASE  0x2000
#define MGPU_REG_DOORBELL(n)    (MGPU_REG_DOORBELL_BASE + ((n) * 4))

/* Control register bits */
#define MGPU_CTRL_ENABLE        (1 << 0)
#define MGPU_CTRL_RESET         (1 << 1)
#define MGPU_CTRL_PAUSE         (1 << 2)
#define MGPU_CTRL_SINGLE_STEP   (1 << 3)
#define MGPU_CTRL_FLUSH_CACHE   (1 << 4)
#define MGPU_CTRL_PERF_COUNTER  (1 << 5)

/* Status register bits */
#define MGPU_STATUS_IDLE        (1 << 0)
#define MGPU_STATUS_BUSY        (1 << 1)
#define MGPU_STATUS_ERROR       (1 << 2)
#define MGPU_STATUS_HALTED      (1 << 3)
#define MGPU_STATUS_FENCE_DONE  (1 << 4)
#define MGPU_STATUS_CMD_EMPTY   (1 << 5)
#define MGPU_STATUS_CMD_FULL    (1 << 6)

/* IRQ bits */
#define MGPU_IRQ_CMD_COMPLETE   (1 << 0)
#define MGPU_IRQ_ERROR          (1 << 1)
#define MGPU_IRQ_FENCE          (1 << 2)
#define MGPU_IRQ_QUEUE_EMPTY    (1 << 3)
#define MGPU_IRQ_SHADER_HALT    (1 << 4)
#define MGPU_IRQ_PERF_COUNTER   (1 << 5)

/* Capability bits */
#define MGPU_CAP_VERTEX_SHADER  (1 << 0)
#define MGPU_CAP_FRAGMENT_SHADER (1 << 1)
#define MGPU_CAP_TEXTURE        (1 << 2)
#define MGPU_CAP_FLOAT16        (1 << 3)
#define MGPU_CAP_FLOAT32        (1 << 4)
#define MGPU_CAP_INT32          (1 << 5)
#define MGPU_CAP_ATOMIC         (1 << 6)
#define MGPU_CAP_FENCE          (1 << 7)
#define MGPU_CAP_MULTI_QUEUE    (1 << 8)
#define MGPU_CAP_PREEMPTION     (1 << 9)

/* Version field extraction */
#define MGPU_VERSION_MAJOR(v)   (((v) >> 24) & 0xFF)
#define MGPU_VERSION_MINOR(v)   (((v) >> 16) & 0xFF)
#define MGPU_VERSION_PATCH(v)   (((v) >> 8) & 0xFF)
#define MGPU_VERSION_BUILD(v)   ((v) & 0xFF)

/* Error codes */
#define MGPU_ERROR_NONE         0x00
#define MGPU_ERROR_INVALID_CMD  0x01
#define MGPU_ERROR_MEM_FAULT    0x02
#define MGPU_ERROR_SHADER_FAULT 0x03
#define MGPU_ERROR_TIMEOUT      0x04
#define MGPU_ERROR_OVERFLOW     0x05

/* Command opcodes */
#define MGPU_CMD_NOP            0x00
#define MGPU_CMD_DRAW           0x01
#define MGPU_CMD_COMPUTE        0x02
#define MGPU_CMD_DMA            0x03
#define MGPU_CMD_FENCE          0x04
#define MGPU_CMD_WAIT           0x05
#define MGPU_CMD_REG_WRITE      0x06
#define MGPU_CMD_REG_READ       0x07

/* Limits */
#define MGPU_RING_SIZE_MIN      4096
#define MGPU_RING_SIZE_MAX      262144  /* 256KB */
#define MGPU_MAX_QUEUES         16
#define MGPU_MAX_ENGINES        4

#endif /* _MGPU_REGS_H_ */