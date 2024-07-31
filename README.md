# kvdo

The kernel module component of VDO which provides pools of deduplicated and/or
compressed block storage.

## Background

VDO is a device-mapper target that provides inline block-level deduplication,
compression, and thin provisioning capabilities for primary storage. VDO
is managed through LVM and can be integrated into any existing storage stack.

Deduplication is a technique for reducing the consumption of storage resources
by eliminating multiple copies of duplicate blocks. Compression takes the
individual unique blocks and shrinks them with coding algorithms; these reduced
blocks are then efficiently packed together into physical blocks. Thin
provisioning manages the mapping from logical block addresses presented by VDO
to where the data has actually been stored, and also eliminates any blocks of
all zeroes.

With deduplication, instead of writing the same data more than once each
duplicate block is detected and recorded as a reference to the original
block. VDO maintains a mapping from logical block addresses (presented to the
storage layer above VDO) to physical block addresses on the storage layer
under VDO. After deduplication, multiple logical block addresses may be mapped
to the same physical block address; these are called shared blocks and are
reference-counted by the software.

With VDO's compression, blocks are compressed with the fast LZ4 algorithm, and
collected together where possible so that multiple compressed blocks fit within
a single 4 KB block on the underlying storage. Each logical block address is
mapped to a physical block address and an index within it for the desired
compressed data. All compressed blocks are individually reference-counted for
correctness.

Block sharing and block compression are invisible to applications using the
storage, which read and write blocks as they would if VDO were not present.
When a shared block is overwritten, a new physical block is allocated for
storing the new block data to ensure that other logical block addresses that
are mapped to the shared physical block are not modified.

This repository includes the kvdo module, which can be built and loaded as an
out-of-tree kernel module. This module implements fine-grained storage
virtualization, thin provisioning, block sharing, compression, and
memory-efficient duplicate identification.

## History

VDO was originally developed by Permabit Technology Corp. as a proprietary set
of kernel modules and userspace tools. This software and technology has been
acquired by Red Hat and relicensed under the GPL (v2 or later). The kernel
module has been merged into the upstream Linux kernel as the dm-vdo devive
mapper target. The source for this module can be found in drivers/md/dm-vdo/.

## Documentation

- [RHEL9 VDO Documentation](https://access.redhat.com/documentation/en-us/red_hat_enterprise_linux/9/html/deduplicating_and_compressing_logical_volumes_on_rhel/index)
- [RHEL8 VDO Documentation](https://access.redhat.com/documentation/en-us/red_hat_enterprise_linux/8/html/deduplicating_and_compressing_storage/index)
- [RHEL7 VDO Integration Guide](https://access.redhat.com/documentation/en-us/red_hat_enterprise_linux/7/html/storage_administration_guide/vdo-integration)
- [RHEL7 VDO Evaluation Guide](https://access.redhat.com/documentation/en-us/red_hat_enterprise_linux/7/html/storage_administration_guide/vdo-evaluation)

## Releases

**This repository is no longer being updated for newer kernels.**

The most recent version of this project can be found in the upstream Linux
kernel, as the dm-vdo module. Each existing branch of this repository is
intended to work with a specific release of Enterprise Linux (Red Hat
Enterprise Linux, CentOS, etc.).

Version | Intended Enterprise Linux Release
------- | ---------------------------------
6.1.x.x | EL7 (3.10.0-*.el7)
6.2.x.x | EL8 (4.18.0-*.el8)
8.2.x.x | EL9 (5.14.0-*.el9)

* Pre-built versions with the required modifications for older Fedora releases
  can be found [here](https://copr.fedorainfracloud.org/coprs/rhawalsh/dm-vdo)
  and can be used by running `dnf copr enable rhawalsh/dm-vdo`.

## Upgrades

The latest version of this project is available in the Linux kernel as the
dm-vdo module starting in version 6.9. If you have a VDO volume created with
the kvdo module, be sure to shut it down cleanly before switching to the
dm-vdo module. Failure to do so may result in issues including loss of data.

As with any upgrade, it is advisable to create a snapshot of the device before
upgrading to the new module.

## Building

In order to build the kernel modules, invoke the following command
from the top directory of this tree:

        make -C /usr/src/kernels/`uname -r` M=`pwd`

To install the compiled module:

        make -C /usr/src/kernels/`uname -r` M=`pwd` modules_install

* There is a dkms.conf template that can be used in the kvdo.spec file which
  can take care of rebuilding and installing the kernel module any time a new
  kernel is booted.

* Patched sources that work with certain older upstream kernels can be found
  [here](https://github.com/rhawalsh/kvdo).

## Communication Channels and Contributions

Community feedback, participation and patches are welcome to the
[vdo-devel](https://github.com/dm-vdo/vdo-devel) repository, which is the
parent of this one. This repository does not accept pull requests.

## Licensing

[GPL v2.0 or later](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html).
All contributions retain ownership by their original author, but must also
be licensed under the GPL 2.0 or later to be merged.
