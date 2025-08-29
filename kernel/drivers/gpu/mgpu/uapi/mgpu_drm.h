/* Userspace-visible UAPI (skeleton) */
#ifndef UAPI_MGPU_DRM_H
#define UAPI_MGPU_DRM_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define MGPU_UAPI_MAGIC 'M'

struct mgpu_get_info { __u32 version; __u32 caps; };
struct mgpu_bo_create { __u64 size; __u32 handle; __u32 flags; };
struct mgpu_bo_mmap   { __u32 handle; __u32 pad; __u64 offset; };
struct mgpu_submit    { __u64 cb_addr; __u32 cb_bytes; __u32 fence_id; };
struct mgpu_wait_fence{ __u32 fence_id; __u32 timeout_ms; };
struct mgpu_load_shader { __u64 user_ptr; __u32 bytes; __u32 reserved; };
struct mgpu_rw_reg    { __u32 offset; __u32 value; };

#define MGPU_GET_INFO     _IOR(MGPU_UAPI_MAGIC, 0x00, struct mgpu_get_info)
#define MGPU_BO_CREATE    _IOWR(MGPU_UAPI_MAGIC, 0x01, struct mgpu_bo_create)
#define MGPU_BO_MMAP      _IOWR(MGPU_UAPI_MAGIC, 0x02, struct mgpu_bo_mmap)
#define MGPU_SUBMIT       _IOW(MGPU_UAPI_MAGIC, 0x03, struct mgpu_submit)
#define MGPU_WAIT_FENCE   _IOWR(MGPU_UAPI_MAGIC, 0x04, struct mgpu_wait_fence)
#define MGPU_LOAD_SHADER  _IOW(MGPU_UAPI_MAGIC, 0x05, struct mgpu_load_shader)
#define MGPU_READ_REG     _IOWR(MGPU_UAPI_MAGIC, 0x06, struct mgpu_rw_reg)
#define MGPU_WRITE_REG    _IOW(MGPU_UAPI_MAGIC, 0x07, struct mgpu_rw_reg)

#endif
