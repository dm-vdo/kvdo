UDS_VERSION = 7.0.0.3

SOURCES =  $(notdir $(wildcard $(src)/*.c)) murmur/MurmurHash3.c
SOURCES += $(addprefix util/,$(notdir $(wildcard $(src)/util/*.c)))
OBJECTS = $(SOURCES:%.c=%.o)
INCLUDES = -I$(src)

EXTRA_CFLAGS =	-std=gnu99				\
		-fno-builtin-memset			\
		-Werror					\
		-Wframe-larger-than=400			\
		-Wno-declaration-after-statement	\
		-DUDS_VERSION=\"$(UDS_VERSION)\"	\
		$(INCLUDES)

obj-m += uds.o

uds-objs = $(OBJECTS)
