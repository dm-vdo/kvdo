VDO_VERSION = 6.2.0.187

VDO_VERSION_MAJOR = $(word 1,$(subst ., ,$(VDO_VERSION)))
VDO_VERSION_MINOR = $(word 2,$(subst ., ,$(VDO_VERSION)))
VDO_VERSION_MICRO = $(word 3,$(subst ., ,$(VDO_VERSION)))

SOURCES  = $(addprefix base/,$(notdir $(wildcard $(src)/base/*.c)))
SOURCES += $(addprefix kernel/,$(notdir $(wildcard $(src)/kernel/*.c)))
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

obj-m += kvdo.o

kvdo-objs = $(OBJECTS)
