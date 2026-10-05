# Kbuild for the vws virtual weather station driver.
obj-m += vws.o
vws-objs := vws_main.o vws_model.o vws_fifo.o vws_sysfs.o vws_proc.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

ccflags-y := -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all clean
