#!/bin/bash

# Original script code for block device testing...
# Author: Michail Flouris (C) 2012

# CAUTION: THIS TEST OVERWRITES THE WHOLE BLOCK DEVICE USED!

if [ $# -ne 1 ] ; then  # Must have ONE command-line arg
	echo "Usage: $0 <block dev name>"
	echo "First arg: path to block device (starting with /dev/X)"
	exit -1
fi

blkdev=$1
dname=`echo $blkdev | cut -c6-`
if [ `echo $blkdev | cut -c1-5` != '/dev/' ] ; then
	echo "Device name does not start with /dev/ ! Aborting..."
	exit -1
fi
if [ ! -b $blkdev ] ; then
	echo "Block device $blkdev does not exist! Aborting..."
	exit -1
fi

bsize=`/sbin/blockdev --getsize $blkdev`
if [ `echo $blkdev | grep mapper` ]; then
	devid=`cat /sys/block/dm-0/dev` # FIXME: not always dm-0 ??
else
	devid=`cat /sys/block/$dname/dev`
fi

echo "Dev: $blkdev bsize=$bsize dname=$dname devid=$devid"

# CONFIGURE THIS FOR SELECTING TESTS
run_basic_dd_io_tests=true
run_dt_tests=true

# CAUTION: overrides all previous run_*_tests values...
run_all_tests=true

ddcmd="/bin/dd oflag=direct"
dtcmd=/bin/dt

sync;sync;sync

#/sbin/dmsetup targets
#/sbin/dmsetup info

#check if everything is setup ok, else exit...
sync;sync;sync

if $run_all_tests || $run_basic_dd_io_tests ; then
# -----------------------------------------------------------------------
	echo 'WILL START DD WRITES!'
	sync;sync;sync
	$ddcmd if=/dev/zero of=$blkdev bs=4096 count=1 
	#$ddcmd if=/dev/zero of=$blkdev bs=4096 count=1k 
	#$ddcmd if=/dev/zero of=$blkdev bs=1M count=100 
	#$ddcmd if=/dev/zero of=$blkdev bs=4096 count=80K 
	echo 'DONE WITH DD WRITES!'
	sync;sync;sync

	sleep 1
	echo 'WILL START DD READS!'
	sync;sync;sync
	$ddcmd of=/dev/null if=$blkdev bs=4096 count=10 
	#$ddcmd of=/dev/null if=$blkdev bs=4096 count=10 skip=252 
	#$ddcmd of=/dev/null if=$blkdev bs=4096 count=8k 
	#$ddcmd of=/dev/null if=$blkdev bs=4096 count=80K 
	echo 'DONE WITH DD READS!'
	sync;sync;sync
# -----------------------------------------------------------------------
fi

if $run_all_tests || $run_dt_tests; then
# -----------------------------------------------------------------------
	echo 'Starting DT Tests...'
	$dtcmd of=$blkdev bs=4k pattern=iot flags=fsync
	echo 'DT TESTS DONE!'
# -----------------------------------------------------------------------
fi

echo 'ALL DONE!'

