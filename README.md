# kvdo

A pair of kernel modules which provide pools of deduplicated and/or compressed
block storage.

## Background

VDO (which includes [kvdo](https://github.com/dm-vdo/kvdo) and
[vdo](https://github.com/dm-vdo/vdo)) is software that provides inline
block-level deduplication, compression, and thin provisioning capabilities for
primary storage. VDO installs within the Linux device mapper framework, where
it takes ownership of existing physical block devices and remaps these to new,
higher-level block devices with data-efficiency capabilities.

Deduplication is a technique for reducing the consumption of storage resources
by eliminating multiple copies of duplicate blocks. Compression takes the
individual unique blocks and shrinks them with coding algorithms; these reduced
blocks are then efficiently packed together into physical blocks.  Thin
provisioning manages the mapping from LBAs presented by VDO to where the data
has actually been stored, and also eliminates any blocks of all zeroes.

With deduplication, instead of writing the same data more than once each
duplicate block is detected and recorded as a reference to the original
block. VDO maintains a mapping from logical block addresses (used by the
storage layer above VDO) to physical block addresses (used by the storage layer
under VDO). After deduplication, multiple logical block addresses may be mapped
to the same physical block address; these are called shared blocks and are
reference-counted by the software.

With VDO's compression, multiple blocks (or shared blocks) are compressed with
the fast LZ4 algorithm, and binned together where possible so that multiple
compressed blocks fit within a 4 KB block on the underlying storage.  Mapping
from LBA is to a physical block address and index within it for the desired
compressed data.  All compressed blocks are individually reference counted for
correctness.

Block sharing and block compression are invisible to applications using the
storage, which read and write blocks as they would if VDO were not
present. When a shared block is overwritten, a new physical block is allocated
for storing the new block data to ensure that other logical block addresses
that are mapped to the shared physical block are not modified.

This public source release of VDO includes two kernel modules, and a set of
userspace tools for managing them. The "kvdo" module implements fine-grained
storage virtualization, thin provisioning, block sharing, and compression; the
"uds" module provides memory-efficient duplicate identification. The userspace
tools include a pair of python scripts, "vdo" for creating and managing VDO
volumes, and "vdostats" for extracting statistics from those volumes.

## Documentation

Project documentation is being converted from its proprietary form and will be
added to this repository at a later date.

## Status

VDO was originally developed by Permabit Technology Corp. as a proprietary set
of kernel modules and userspace tools. This software and technology has been
acquired by Red Hat, has been relicensed under the GPL (v2 or later), and this
repository begins the process of preparing for integration with the upstream
kernel.

While this software has been relicensed there are a number of issues that must
still be addressed to be ready for upstream.  These include:

- Conformance with kernel coding standards
- Use of existing EXPORT_SYMBOL_GPL kernel interfaces where appropriate
- Refactoring of primitives (e.g. cryptographic) to appropriate kernel
  subsystems
- Support for non-x86-64 platforms
- Refactoring of platform layer abstractions and other changes requested by
  upstream maintainers

We expect addressing these issues to take some time. In the meanwhile, this
project allows interested parties to begin using VDO immediately. The
technology itself is thoroughly tested, mature, and in production use since
2014 in its previous proprietary form.

## Building

In order to build the kernel modules, invoke the following command
from the top directory of this tree:

  make -C /usr/src/kernels/<version> M=\`pwd\`

## Communication channels

Community feedback, participation and patches are welcome to the
vdo-devel@redhat.com mailing list -- subscribe
[here](https://www.redhat.com/mailman/listinfo/vdo-devel).

## Licensing

[GPL v2.0 or later](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html).
All contributions retain ownership by their original author, but must also
be licensed under the GPL 2.0 or later to be merged.

