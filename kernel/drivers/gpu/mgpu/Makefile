#kbuild

# yousef can fix this later..

mgpu-y := mgpu_drv.o \
          mgpu_core.o \
          mgpu_irq.o \
          mgpu_dma.o \
          mgpu_cmdq.o \
          mgpu_fence.o \
          mgpu_gem.o \
          mgpu_uapi.o \
          mgpu_debugfs.o \
          mgpu_reset.o \
          mgpu_pm.o \
          mgpu_health.o

# Optional components
mgpu-$(CONFIG_MGPU_SCHEDULER) += mgpu_sched.o
mgpu-$(CONFIG_MGPU_SHADER) += mgpu_shader.o mgpu_pipeline.o
mgpu-$(CONFIG_MGPU_SPI) += mgpu_spi.o
mgpu-$(CONFIG_MGPU_DRM) += mgpu_drm.o
mgpu-$(CONFIG_MGPU_TRACE) += mgpu_trace.o
mgpu-$(CONFIG_MGPU_SELFTEST) += mgpu_selftest.o
mgpu-$(CONFIG_MGPU_KUNIT) += mgpu_kunit.o
mgpu-$(CONFIG_MGPU_COREDUMP) += mgpu_coredump.o

obj-$(CONFIG_DRM_MGPU) += mgpu.o

# For out-of tree builds
ifneq ($(KERNELRELEASE),)
# Called from kernel build system
obj-m += mgpu.o

else
# Called from command line
KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

default:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules_install
	depmod -a

# Development helpers
load:
	sudo insmod mgpu.ko run_selftests=1

unload:
	sudo rmmod mgpu

reload: unload load

test:
	sudo dmesg -C
	$(MAKE) reload
	sudo dmesg

# Generate documentation
doc:
	kernel-doc -rst *.c > documentation.rst

# Static analysis
check:
	sparse -D__CHECK_ENDIAN__ *.c
	scripts/checkpatch.pl --no-tree -f *.c *.h

.PHONY: default clean install load unload reload test doc check

endif