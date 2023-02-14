VDO_VERSION = 8.2.1.6

SOURCES  = $(notdir $(wildcard $(src)/*.c))
OBJECTS = $(SOURCES:%.c=%.o)
INCLUDES = -I$(src)

EXTRA_CFLAGS =	-std=gnu99					\
		-fno-builtin-memset				\
		-Werror						\
		$(if $(CONFIG_KASAN),,-Wframe-larger-than=400)	\
		-DCURRENT_VERSION=\"$(VDO_VERSION)\"		\
		$(INCLUDES)

obj-m += kvdo.o

kvdo-objs = $(OBJECTS)
