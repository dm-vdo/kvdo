VDO_VERSION = 6.1.2.41

VDO_VERSION_MAJOR = $(word 1,$(subst ., ,$(VDO_VERSION)))
VDO_VERSION_MINOR = $(word 2,$(subst ., ,$(VDO_VERSION)))
VDO_VERSION_MICRO = $(word 3,$(subst ., ,$(VDO_VERSION)))

SOURCES  = $(addprefix base/,$(notdir $(wildcard $(src)/base/*.c)))
SOURCES += $(addprefix kernel/,$(notdir $(wildcard $(src)/kernel/*.c)))
SOURCES += ../uds/murmur/MurmurHash3.c
OBJECTS = $(SOURCES:%.c=%.o)
INCLUDES = -I$(src)/base -I$(src)/kernel -I$(src)/../uds

EXTRA_CFLAGS =	-std=gnu99					\
		-fno-builtin-memset				\
		-Werror						\
		-Wframe-larger-than=400				\
		-Wno-declaration-after-statement		\
		-DVDO_VERSION_MAJOR=$(VDO_VERSION_MAJOR)	\
		-DVDO_VERSION_MINOR=$(VDO_VERSION_MINOR)	\
		-DVDO_VERSION_MICRO=$(VDO_VERSION_MICRO)	\
		-DCURRENT_VERSION=\"$(VDO_VERSION)\"		\
		$(INCLUDES)

CFLAGS_vio.o= -std=gnu89
CFLAGS_REMOVE_vio.o= -std=gnu99

obj-m += kvdo.o

kvdo-objs = $(OBJECTS)
