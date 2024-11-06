VDO_VERSION = 8.2.5.2

SOURCES  = $(notdir $(wildcard $(src)/*.c))
OBJECTS = $(SOURCES:%.c=%.o)
INCLUDES = -I$(src)

RHEL_RELEASE_EXTRA ?= $(shell uname -r | \
		        sed -e 's/^[0-9]\+\.[0-9]\+\.[0-9]\+\-\([0-9]\+\).*/\1/')

EXTRA_CFLAGS =	-std=gnu11					\
		-fno-builtin-memset				\
		-Werror						\
		$(if $(CONFIG_KASAN),,-Wframe-larger-than=400)	\
		-DCURRENT_VERSION=\"$(VDO_VERSION)\"		\
		-DRHEL_RELEASE_EXTRA=$(RHEL_RELEASE_EXTRA)	\
		$(INCLUDES)

obj-m += kvdo.o

kvdo-objs = $(OBJECTS)
