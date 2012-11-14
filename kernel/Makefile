
LINUX ?= /lib/modules/$(shell uname -r)/build
ZIO ?= $(M)/../zio
SPEC_SW ?= $(M)/../spec-sw
FMC_BUS ?= $(M)/../fmc-bus

KBUILD_EXTRA_SYMBOLS := \
	$(ZIO)/Module.symvers \
	$(SPEC_SW)/kernel/Module.symvers \
	$(FMC_BUS)/kernel/Module.symvers

ccflags-y = \
	-I$(ZIO)/include \
	-I$(SPEC_SW)/kernel \
	-I$(SPEC_SW)/kernel/include \
	-I$(FMC_BUS)/kernel/include \
	-I$M

ccflags-y += -DDEBUG # temporary

subdirs-ccflags-y = $(ccflags-y)

obj-m := fmc-adc.o
fmc-adc-objs =  fa-zio-drv.o 
fmc-adc-objs += fa-core.o
fmc-adc-objs += fa-zio-trg.o
fmc-adc-objs += fa-dma.o
fmc-adc-objs += onewire.o
fmc-adc-objs += spi.o
	
all modules:
	$(MAKE) -C $(LINUX) M=$(shell /bin/pwd) modules

install modules_install:
	$(MAKE) -C $(LINUX) M=$(shell /bin/pwd) modules_install

# be able to run the "clean" rule even if $(LINUX) is not valid
clean:
	rm -rf *.o *~  .*.cmd *.ko *.mod.c .tmp_versions Module.symvers \
		Module.markers modules.order