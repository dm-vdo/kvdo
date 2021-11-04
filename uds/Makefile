UDS_VERSION = 8.2.0.68

SOURCES =  $(notdir $(wildcard $(src)/*.c)) murmur/murmurhash3.c
OBJECTS = $(SOURCES:%.c=%.o)
INCLUDES = -I$(src)

EXTRA_CFLAGS =	-fno-builtin-memset				\
		$(if $(CONFIG_KASAN),,-Wframe-larger-than=400)	\
		-Wno-declaration-after-statement		\
		-DUDS_VERSION=\"$(UDS_VERSION)\"		\
		$(INCLUDES)

obj-m += uds.o

uds-objs = $(OBJECTS)
