/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef COMMON_COMPILER_H
#define COMMON_COMPILER_H

#include <asm/rwonce.h>
#include <linux/compiler.h>


#define const_container_of(ptr, type, member)                           \
	__extension__({                                                 \
		const __typeof__(((type *) 0)->member) *__mptr = (ptr); \
		(const type *) ((const char *) __mptr -                 \
				offsetof(type, member));                \
	})

/*
 * The "inline" keyword alone takes effect only when the optimization level
 * is high enough.  Define INLINE to force the gcc to "always inline".
 */
#define INLINE __attribute__((always_inline)) inline



#define __STRING(x) #x


#endif /* COMMON_COMPILER_H */
