//-----------------------------------------------------------------------------
// MurmurHash3 was written by Austin Appleby, and is placed in the public
// domain. The author hereby disclaims copyright to this source code.

#ifndef _MURMURHASH3_H_
#define _MURMURHASH3_H_

//-----------------------------------------------------------------------------
// Platform-specific functions and macros

// Linux kernel

#ifdef __KERNEL__
# include <linux/types.h>

// Microsoft Visual Studio

#else // defined(__KERNEL__)
# if defined(_MSC_VER)

  typedef unsigned char uint8_t;
  typedef unsigned long uint32_t;
  typedef unsigned __int64 uint64_t;

// Other compilers

# else	// defined(_MSC_VER)

#  include <stdint.h>

# endif // !defined(_MSC_VER)
#endif // !defined(__KERNEL__)

//-----------------------------------------------------------------------------

void MurmurHash3_x86_32  ( const void * key, int len, uint32_t seed, void * out );

void MurmurHash3_x86_128 ( const void * key, int len, uint32_t seed, void * out );

void MurmurHash3_x64_128 ( const void * key, int len, uint32_t seed, void * out );

void MurmurHash3_x64_128_double (const void * key,
                                 int          len,
                                 uint32_t     seed1,
                                 uint32_t     seed2,
                                 void *       out );
//-----------------------------------------------------------------------------

#endif // _MURMURHASH3_H_
