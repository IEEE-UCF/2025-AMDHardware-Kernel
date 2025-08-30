#ifndef _UAPI_MGPU_DRM_H_
#define _UAPI_MGPU_DRM_H_

#include <linux/types.h>
#include <linux/ioctl.h>

#define MGPU_IOCTL_BASE 'M'

/* IOCTL commands */
#define MGPU_GET_INFO      _IOR(MGPU_IOCTL_BASE, 0x00, struct mgpu_info)
#define MGPU_BO_CREATE     _IOWR(MGPU_IOCTL_BASE, 0x01, struct mgpu_bo_create)
#define MGPU_BO_MMAP       _IOWR(MGPU_IOCTL_BASE, 0x02, struct mgpu_bo_mmap)
#define MGPU_BO_DESTROY    _IOW(MGPU_IOCTL_BASE, 0x03, struct mgpu_bo_destroy)
#define MGPU_SUBMIT        _IOW(MGPU_IOCTL_BASE, 0x04, struct mgpu_submit)
#define MGPU_WAIT_FENCE    _IOW(MGPU_IOCTL_BASE, 0x05, struct mgpu_wait_fence)
#define MGPU_LOAD_SHADER   _IOW(MGPU_IOCTL_BASE, 0x06, struct mgpu_load_shader)
#define MGPU_READ_REG      _IOWR(MGPU_IOCTL_BASE, 0x07, struct mgpu_reg_access)
#define MGPU_WRITE_REG     _IOW(MGPU_IOCTL_BASE, 0x08, struct mgpu_reg_access)

/* Device information */
struct mgpu_info {
    __u32 version_major;
    __u32 version_minor;
    __u32 version_patch;
    __u32 capabilities;
    __u32 num_engines;
    __u32 num_queues;
    __u32 max_width;
    __u32 max_height;
    __u32 max_threads;
    __u32 memory_size;
    __u32 shader_mem_size;
    __u32 reserved[5];
};

/* Buffer object creation */
struct mgpu_bo_create {
    __u32 size;           /* IN: Size in bytes */
    __u32 flags;          /* IN: Creation flags */
    __u32 handle;         /* OUT: BO handle */
    __u32 gpu_addr;       /* OUT: GPU virtual address */
};

/* BO creation flags */
#define MGPU_BO_FLAGS_CACHED     (1 << 0)
#define MGPU_BO_FLAGS_COHERENT   (1 << 1)
#define MGPU_BO_FLAGS_WRITE_COMBINE (1 << 2)
#define MGPU_BO_FLAGS_VERTEX     (1 << 3)
#define MGPU_BO_FLAGS_INDEX      (1 << 4)
#define MGPU_BO_FLAGS_TEXTURE    (1 << 5)
#define MGPU_BO_FLAGS_FRAMEBUFFER (1 << 6)
#define MGPU_BO_FLAGS_COMMAND    (1 << 7)

/* Buffer object mmap */
struct mgpu_bo_mmap {
    __u32 handle;         /* IN: BO handle */
    __u32 pad;
    __u64 offset;         /* OUT: mmap offset */
};

/* Buffer object destroy */
struct mgpu_bo_destroy {
    __u32 handle;         /* IN: BO handle */
    __u32 pad;
};

/* Command submission */
struct mgpu_submit {
    __u64 commands;       /* IN: Pointer to command buffer */
    __u32 cmd_size;       /* IN: Size of commands in bytes */
    __u32 queue_id;       /* IN: Queue to submit to */
    __u64 bo_handles;     /* IN: Array of BO handles used */
    __u32 num_bos;        /* IN: Number of BOs */
    __u32 flags;          /* IN: Submission flags */
    __u64 fence_addr;     /* IN: Fence address (optional) */
    __u32 fence_value;    /* IN: Fence value to write */
    __u32 pad;
};

/* Submit flags */
#define MGPU_SUBMIT_FLAGS_FENCE  (1 << 0)
#define MGPU_SUBMIT_FLAGS_SYNC   (1 << 1)
#define MGPU_SUBMIT_FLAGS_NO_FLUSH (1 << 2)

/* Fence wait */
struct mgpu_wait_fence {
    __u64 fence_addr;     /* IN: Fence address */
    __u32 fence_value;    /* IN: Value to wait for */
    __u32 timeout_ms;     /* IN: Timeout in milliseconds */
};

/* Shader loading */
struct mgpu_load_shader {
    __u64 data;           /* IN: Shader binary data */
    __u32 size;           /* IN: Size in bytes */
    __u32 type;           /* IN: Shader type */
    __u32 slot;           /* IN: Shader slot/index */
    __u32 flags;          /* IN: Load flags */
};

/* Shader types */
#define MGPU_SHADER_VERTEX    0
#define MGPU_SHADER_FRAGMENT  1
#define MGPU_SHADER_COMPUTE   2

/* Register access (for debugging) */
struct mgpu_reg_access {
    __u32 offset;         /* IN: Register offset */
    __u32 value;          /* IN/OUT: Register value */
};

/* Error codes */
#define MGPU_ERROR_NONE          0
#define MGPU_ERROR_INVALID_PARAM -1
#define MGPU_ERROR_NO_MEMORY     -2
#define MGPU_ERROR_TIMEOUT       -3
#define MGPU_ERROR_DEVICE_LOST   -4
#define MGPU_ERROR_NOT_SUPPORTED -5

/* Command buffer structures */
struct mgpu_cmd_header {
    __u32 opcode : 8;
    __u32 size : 8;       /* Size in dwords */
    __u32 flags : 16;
};

/* Draw command */
struct mgpu_cmd_draw {
    struct mgpu_cmd_header header;
    __u32 vertex_count;
    __u32 instance_count;
    __u32 first_vertex;
    __u32 first_instance;
};

/* DMA command */
struct mgpu_cmd_dma {
    struct mgpu_cmd_header header;
    __u32 src_addr;
    __u32 dst_addr;
    __u32 size;
    __u32 flags;
};

/* Fence command */
struct mgpu_cmd_fence {
    struct mgpu_cmd_header header;
    __u32 addr;
    __u32 value;
};

#endif /* _UAPI_MGPU_DRM_H_ */