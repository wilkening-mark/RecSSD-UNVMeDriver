UNVMe - A User Space NVMe Driver
================================

UNVMe is a user space NVMe driver developed at Micron Technology.

The driver in this model is implemented as a library (libunvme.a) that
applications can be linked with.  Upon start, an application will first
initialize the NVMe device(s) and then can perform I/O directly.

Note that an application can access multiple devices simultaneously, but 
a device can only be accessed by one application at any given time.
Device name is to be specified in PCI format with optional NSID
(e.g. 0a:00.0 or 0a:00.0/1). NSID 1 is assumed if /NSID is omitted.

User space driver, in general, is a specially customized solution.
Applications must use the provided APIs to access the device instead of
the system provided POSIX APIs.


A design note:  UNVMe is designed modularly with independent components
including NVME (unvme_nvme.h) and VFIO (unvme_vfio.h), in which the driver
itself is built on top of those modules.

The programs under test/nvme are built directly on those modules independent
of the UNVMe driver.  They are served as examples for building individual
NVMe admin commands.

The programs under test/unvme are examples for developing applications
using the UNVMe interface (unvme.h).



System Requirements
===================

UNVMe has a dependency on features provided by the VFIO module in the Linux
kernel (introduced since 3.6).  UNVMe code has been built and tested only
with CentOS 6 and 7 running on x86_64 CPU based systems.

UNVMe requires the following hardware and software support:

    VT-d    -   CPU must support VT-d (Virtualization Technology for Directed I/O).
                Check <http://ark.intel.com/> for Intel product specifications.
                VT-d setting is normally found in the BIOS configuration.

    VFIO    -   Linux OS must have kernel 3.6 or later compiled with the
                following configurations:

                    CONFIG_IOMMU_API=y
                    CONFIG_IOMMU_SUPPORT=y
                    CONFIG_INTEL_IOMMU=y
                    CONFIG_VFIO=m
                    CONFIG_VFIO_PCI=m
                    CONFIG_VFIO_IOMMU_TYPE1=m

                The boot command line must set "intel_iommu=on" argument.

                To verify the system correctly configured with VFIO support,
                check that /sys/kernel/iommu_groups directory is not empty but
                contains other subdirectories (i.e. group numbers).

On CentOS 6, which comes with kernel version 2.x (i.e. prior to 3.6),
the user must compile and boot a newer kernel that has the VFIO module.
The user must also copy the header file from the kernel source directory
include/uapi/linux/vfio.h to /usr/include/linux if that is missing.

UNVMe requires root privilege to access a device.



Build, Run, and Test
====================

To download and install the driver library, run:

    $ git clone https://github.com/MicronSSD/unvme.git
    $ make install


To setup a device for UNVMe usage (do once before running applications), run:

    $ unvme-setup bind

    By default, all NVMe devices found in the system will be bound to the
    VFIO driver enabling them for UNVMe usage.  Specific PCI device(s)
    may also be specified for binding, e.g. unvme-setup bind 0a:00.0.


To reset device(s) to the NVMe kernel space driver, run:

    $ unvme-setup reset


For usage help, invoke unvme-setup without argument.


To run UNVMe tests, specify the device(s) with command:

    $ test/unvme-test 0a:00.0 0b:00.0


The commands under test/nvme may also be invoked individually, e.g.:

    $ test/nvme/nvme_identify 0a:00.0
    $ test/nvme/nvme_get_features 0a:00.0
    $ test/nvme/nvme_get_log_page 0a:00.0 1 2
    ...



I/O Benchmark Tests
===================

To run fio benchmark tests against UNVMe:

    1) Download and compile the fio source code (available on https://github.com/axboe/fio).


    2) Edit unvme/Makefile.def and set FIODIR to the compiled fio source
       directory (or alternatively export the FIODIR variable).


    3) Rerun make to include building the fio engine, since setting FIODIR
       will enable ioengine/unvme_fio to be built.
    
       $ make

       Note that the fio source code is constantly changing, and unvme_fio.c
       has been verified to work with the fio versions 2.7 through 2.19


    4) Set up for UNVMe driver (if not already):

       $ unvme-setup bind


    5) Launch the test script:
    
       $ test/unvme-benchmark DEVICENAME

       Note the benchmark test, by default, will run random write and read
       tests with 1, 4, 8, and 16 threads, and io depth of 1, 4, 8, and 16.
       Each test will be run for 120 seconds after a ramp time of 60 seconds.
       These default settings can be overridden from the shell command line, e.g.:

       $ RAMPTIME=10 RUNTIME=20 NUMJOBS="1 4" IODEPTH="4 8" test/unvme-benchmark 0a:00.0


To run the same tests against the kernel space driver:

    $ unvme-setup reset

    $ test/unvme-benchmark /dev/nvme0n1


All the FIO results, by default, will be stored in test/out directory.



Application Programming Interfaces
==================================

The UNVMe APIs are designed with application ease of use in mind.
As defined in unvme.h, the following functions are supported:

    unvme_open()    -   This function must be invoked first to establish a
                        connection to the specified PCI device.

    unvme_close()   -   Close a device connection.

    unvme_alloc()   -   Allocate an I/O buffer.

    unvme_free()    -   Free the allocated I/O buffer.

    unvme_write()   -   Write the specified number of blocks (nlb) to the
                        device starting at logical block address (slba).
                        The buffer must be acquired from unvme_alloc().
                        The qid (range from 0 to 1 less than the number of
                        queues supported by the device) may be used for
                        thread safe I/O operations.  Each queue must only
                        be accessed by a one thread at any one time.

    unvme_read()    -   Read from the device (i.e. like unvme_write).

    unvme_awrite()  -   Send a write command to the device asynchronously
                        and return immediately.  The returned descriptor
                        is used via apoll() to poll for completion.

    unvme_aread()   -   Send an asynchronous read (i.e. like unvme_awrite).

    unvme_apoll()   -   Poll an asynchronous read/write for completion.



Note that a user space filesystem, namely UNFS, has also been developed
at Micron to work with the UNVMe driver.  Such available filesystem enables
major applications like MongoDB to work with UNVMe driver.
See https://github.com/MicronSSD/unfs.git for details.

