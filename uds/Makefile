UDS_VERSION = 8.0.5.1

SOURCES =  $(notdir $(wildcard $(src)/*.c)) murmur/MurmurHash3.c
SOURCES += $(addprefix util/,$(notdir $(wildcard $(src)/util/*.c)))
OBJECTS = $(SOURCES:%.c=%.o)
INCLUDES = -I$(src)

EXTRA_CFLAGS =	-std=gnu99					\
		-fno-builtin-memset				\
		-Werror						\
		$(if $(CONFIG_KASAN),,-Wframe-larger-than=400)	\
		-Wno-declaration-after-statement		\
		-DUDS_VERSION=\"$(UDS_VERSION)\"		\
		$(INCLUDES)

CFLAGS_REMOVE_deltaIndex.o = -std=gnu99
CFLAGS_REMOVE_masterIndex005.o = -std=gnu99

obj-m += uds.o

uds-objs = $(OBJECTS)
