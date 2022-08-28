dm-vdo
======

The dm-vdo device mapper target provides block-level deduplication,
compression, and thin provisioning. As a device mapper target, it can add these
features to the storage stack, compatible with any filesystem. VDO does not
protect against data corruption, and relies on the data integrity of the
storage below it, for instance using a RAID device.

Userspace component
===================

Formatting a VDO volume requires the use of the 'vdoformat' tool, available at:

https://github.com/dm-vdo/vdo/

Other userspace tools are also available for inspecting VDO volumes.

It is strongly recommended that lvm be used to manage VDO volumes. See
lvm-vdo(7).

Target interface
================

Table line
----------

::

        <offset> <logical device size> vdo V4 <storage device>
        <storage device size> <minimum IO size> <block map cache size>
        <block map era length> [optional arguments]


Required parameters:

	offset:
		The offset, in sectors, at which the VDO volume's
		logical space begins.

	logical device size:
		The size of the device which the VDO volume will
		service, in sectors. Must match the current logical size
		of the VDO volume.

	storage device:
		The device holding the VDO volume's data and metadata.

	storage device size:
		The size of the device to use for the VDO volume, as a
		number of 4096-byte blocks. Must match the current size
		of the VDO volume.

	minimum IO size:
		The minimum IO size for this VDO volume to accept, in
		bytes. Valid values are 512 or 4096. The recommended
		value is 4096.

	block map cache size:
		The size of the block map cache, as a number of 4096-byte
                blocks. The minimum and recommended value is 32768
		blocks. If the logical thread count is non-zero, the
		cache size must be at least 4096 blocks per logical
		thread.

	block map era length:
		The speed with which the block map cache writes out
		modified block map pages. A smaller era length is
		likely to reduce the amount time spent rebuilding, at
		the cost of increased block map writes during normal
		operation. The maximum and recommended value is 16380;
		the minimum value is 1.

Optional parameters:
--------------------
Some or all of these parameters may be specified as <key name> <value> pairs.

Thread related parameters:

VDO assigns different categories of work to separate thread groups, and the
number of threads in each group can be configured separately. Most groups can
use up to 100 threads.

If <hash>, <logical>, and <physical> are all set to 0, the work handled by all
three thread types will be handled by a single thread. If any of these values
are non-zero, all of them must be non-zero.

	ack:
		The number of threads used to complete bios. Since
		completing a bio calls an arbitrary completion function
		outside the VDO volume, threads of this type allow the
		VDO volume to continue processing requests even when bio
		completion is slow. The default is 1.

	bio:
		The number of threads used to issue bios to the
		underlying storage. Threads of this type allow the VDO
		volume to continue processing requests even when bio
		submission is slow. The default is 4.

	bioRotationInterval:
		The number of bios to enqueue on each bio thread before
		switching to the next thread. The value must be greater
		than 0 and not more than 1024; the default is 64.

	cpu:
		The number of threads used to do CPU-intensive work,
		such as hashing and compression. The default is 1.

	hash:
		The number of threads used to manage data comparisons for
                deduplication based on the hash value of data blocks.
                Default is 1.

	logical:
		The number of threads used to manage caching and locking based
                on the logical address of incoming bios. The default is 0; the
                maximum is 60.

	physical:
		The number of threads used to manage administration of
		the underlying storage device. At format time, a slab
		size for the VDO is chosen; the VDO storage device must
		be large enough to have at least 1 slab per physical
		thread. The default is 0; the maximum is 16.

Miscellaneous parameters:

	maxDiscard:
		The maximum size of discard bio accepted, in 4096-byte
		blocks. I/O requests to a VDO volume are normally split
		into 4096-byte blocks, and processed up to 2048 at a
		time. However, discard requests to a VDO volume can be
		automatically split to a larger size, up to
		<maxDiscard> 4096-byte blocks in a single bio, and are
		limited to 1500 at a time. Increasing this value may
		provide better overall performance, at the cost of
		increased latency for the individual discard requests.
		The default and minimum is 1; the maximum is
		UINT_MAX / 4096.

	deduplication:
                Whether deduplication should be started. The default is 'on';
                the acceptable values are 'on' and 'off'.

Device modification
-------------------

A modified table may be loaded into a running, non-suspended VDO volume. The
modifications will take effect when the device is next resumed. The modifiable
parameters are <logical device size>, <physical device size>, <write policy>,
<maxDiscard>, and <deduplication>. 

If the logical device size or physical device size are changed, upon successful
resume VDO will store the new values and require them on future startups. These
two parameters may not be decreased. The logical device size may not exceed 4
PB. The physical device size must increase by at least 32832 4096-byte blocks
if at all, and must not exceed the size of the underlying storage device.
Additionally, when formatting the VDO device, a slab size is chosen: the
physical device size may never increase above the size which provides 8192
slabs, and each increase must be large enough to add at least one new slab.


Examples:

Start a previously-formatted VDO volume with 1G logical space and 1G physical
space, storing to /dev/dm-1 which has more than 1G space.

::

	dmsetup create vdo0 --table \
	"0 2097152 vdo V4 /dev/dm-1 262144 4096 32768 16380"

Grow the logical size to 4G.

::

	dmsetup reload vdo0 --table \
	"0 8388608 vdo V4 /dev/dm-1 262144 4096 32768 16380"
	dmsetup resume vdo0

Grow the physical size to 2G.

::

	dmsetup reload vdo0 --table \
	"0 8388608 vdo V4 /dev/dm-1 524288 4096 32768 16380"
	dmsetup resume vdo0

Grow the physical size by 1G more and increase max discard sectors.

::

	dmsetup reload vdo0 --table \
	"0 10485760 vdo V4 /dev/dm-1 786432 4096 32768 16380 maxDiscard 8"
	dmsetup resume vdo0

Stop the VDO volume.

::

	dmsetup remove vdo0

Start the VDO volume again. Note that the logical and physical device sizes
must still match, but other parameters can change.

::

	dmsetup create vdo1 --table \
	"0 10485760 vdo V2 /dev/dm-1 786432 512 65550 5000 hash 1 logical 3 physical 2"

Messages
--------
VDO devices accept several messages for adjusting various parameters on the
fly. All of them may be sent in the form

::
        dmsetup message vdo1 0 <message-name> <message-params>

Possible messages are:

        stats: Outputs the current view of the VDO statistics. Mostly used by
                the vdostats userspace program to interpret the output buffer.

        dump: Dumps many internal structures to the system log. This is not
                always safe to run, so it should only be used to debug a
                hung VDO. Optional params to specify structures to dump are:

               viopool: The pool of structures used for user IO inside VDO 
               pools: A synonym of 'viopool'
               vdo: Most of the structures managing on-disk data 
               queues: Basic information about each thread VDO is using
               threads: A synonym of 'queues'
               default: Equivalent to 'queues vdo' 
               all: All of the above.
        
        dump-on-shutdown: Perform a default dump next time VDO shuts down.

        compression: Can be used to change whether compression is enabled
                without shutting VDO down. Must have either "on" or "off"
                specified.

        index-create: Reformat the deduplication index belonging to this VDO.

        index-close: Turn off and save the deduplication index belonging to
                this VDO.

        index-enable: Enable deduplication.
        index-disable: Disable deduplication.


Status
------

::

    <device> <operating mode> <in recovery> <index state> <compression state>
    <physical blocks used> <total physical blocks>

	device:
		The name of the VDO volume.

	operating mode:
		The current operating mode of the VDO volume; values
		may be 'normal', 'recovering' (the volume has
		detected an issue with its metadata and is attempting
		to repair itself), and 'read-only' (an error has
		occurred that forces the vdo volume to only support
		read operations and not writes).

	in recovery:
		Whether the VDO volume is currently in recovery
		mode; values may be 'recovering' or '-' which
		indicates not recovering.

	index state:
		The current state of the deduplication index in the
		VDO volume; values may be 'closed', 'closing',
		'error', 'offline', 'online', 'opening', and
		'unknown'.

	compression state:
		The current state of compression in the VDO volume;
		values may be 'offline' and 'online'.

	used physical blocks:
		The number of physical blocks in use by the VDO
		volume.

	total physical blocks:
		The total number of physical blocks the VDO volume
		may use; the difference between this value and the
		<used physical blocks> is the number of blocks the
		VDO volume has left before being full.

Runtime Use of VDO
==================

There are a couple of crucial VDO behaviors to keep in mind when running
workloads on VDO:

- When blocks are no longer in use, sending a discard request for those blocks
  lets VDO potentially mark that space as free. Future read requests to that
  block can then instantly return after reading the block map, instead of
  performing an IO request to read the outdated block. Additionally, if the
  VDO is thin provisioned, discarding unused blocks is absolutely necessary to
  prevent VDO from running out of space. For a filesystem, mounting with the
  -o discard or running fstrim regularly will perform the necessary discards.

- VDO is resilient against crashes if the underlying storage properly honors
  flush requests, and itself properly honors flushes. Specifically, when VDO
  receives a write request, it will allocate space for that request and then
  complete the request; there is no guarantee that the request will be
  reflected on disk until a subsequent flush request to VDO is finished. If
  a filesystem is in use on the VDO device, files should be fsync()d after
  being written in order for their data to be reliably persisted.

- VDO has high throughput at high IO depths -- it can process up to 2048 IO
  requests in parallel. While much of this processing happens after incoming
  requests are finished, for optimal performance a medium IO depth will
  greatly improve over a low IO depth.

Tuning
======

The VDO device has many options, and it can be difficult to choose optimal
choices without perfect knowledge of the workload. Additionally, most
configuration options must be set when VDO is started, and cannot be changed
without shutting the VDO down completely, so the configuration cannot be
easily changed while the workload is active. Therefore, before using VDO in
production, the optimal values for your hardware should be chosen by using
a simulated workload.

The most important value to adjust is the block map cache size.  VDO maintains
a table of mappings from logical block addresses to physical block addresses
in its block map, and VDO must look up the relevant mapping in the cache when
accessing any particular block. By default, VDO allocates 128 MB of metadata
cache in RAM to support efficient access to 100 GB of logical space at a time.
 
Working sets larger than the size that can fit in the configured block map
cache size will require additional I/O to service requests, thus reducing
performance. If the working set is larger than 100G, the block map cache size
should be scaled accordingly.

The logical and physical thread counts should also be adjusted. A logical
thread controls a disjoint section of the block map, so additional logical
threads increase parallelism and can increase throughput. Physical threads
control a disjoint section of the data blocks, so additional physical threads
can increase throughput also. However, excess threads can waste resources and
increase contention.

Bio submission threads control the parallelism involved in sending IOs to the
underlying storage; fewer threads mean there is more opportunity to reorder
IO requests for performance benefit, but also that each IO request has to wait
longer before being submitted.

Bio acknowledgement threads control parallelism in finishing IO requests when
VDO is ready to mark them as done. Usually one is sufficient, but occasionally
especially if the bio has a CPU-heavy callback function multiple are needed.

CPU threads are used for hashing and for compression; in workloads with
compression enabled, more threads may result in higher throughput.

Hash threads are used to sort active requests by hash and determine whether
they should deduplicate; the most CPU intensive actions done by these threads
are comparison of 4096-byte data blocks. 

The optimal thread config varies by your precise hardware and configuration,
and the default values are reasonable choices. For one high-end, many-CPU
system with NVMe storage, the best throughput has been seen with 4 logical, 3
physical, 4 cpu, 2 hash, 8 bio, and 2 ack threads. 


  ..
  Version History
  ===============
  TODO
