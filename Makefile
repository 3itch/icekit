KDIR ?= /usr/src/linux-headers-$(shell uname -r)

SRC_DIR := $(shell pwd)
BUILD_DIR := $(SRC_DIR)/build

obj-m += icepick.o
icepick-objs := ice.o iomap.o msr.o

all:
	$(MAKE) -C $(KDIR) M=$(SRC_DIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(SRC_DIR) clean

test: cache_fill_test/icepick_test.c
	gcc -o $(BUILD_DIR)/icepick_test icepick_test.c

.PHONY: all clean test
