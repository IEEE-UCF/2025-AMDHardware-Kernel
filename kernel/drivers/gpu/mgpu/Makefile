# kbuild
obj-$(CONFIG_MGPU) += mgpu.o

mgpu-y := mgpu_drv.o mgpu_core.o mgpu_irq.o mgpu_dma.o mgpu_cmdq.o \
          mgpu_fence.o mgpu_sched.o mgpu_shader.o mgpu_pipeline.o \
          mgpu_uapi.o mgpu_gem.o mgpu_reset.o mgpu_pm.o mgpu_debugfs.o \
          mgpu_health.o mgpu_selftest.o mgpu_kunit.o mgpu_coredump.o

# Optional backends
mgpu-$(CONFIG_DRM) += mgpu_drm.o
mgpu-$(CONFIG_MGPU_SPI) += mgpu_spi.o
