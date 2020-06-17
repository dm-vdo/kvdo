UDS_VERSION = 8.1.0.120

SOURCES =  $(notdir $(wildcard $(src)/*.c)) murmur/MurmurHash3.c
SOURCES += $(addprefix util/,$(notdir $(wildcard $(src)/util/*.c)))
OBJECTS = $(SOURCES:%.c=%.o)
INCLUDES = -I$(src)

EXTRA_CFLAGS =	-fno-builtin-memset				\
		$(if $(CONFIG_KASAN),,-Wframe-larger-than=400)	\
		-Wno-declaration-after-statement		\
		-DUDS_VERSION=\"$(UDS_VERSION)\"		\
		$(INCLUDES)

obj-m += uds.o

uds-objs = $(OBJECTS)
