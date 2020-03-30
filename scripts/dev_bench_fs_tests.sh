#!/bin/bash

# Original script code...
# Author: Michail Flouris (C) 2012

if [ $# -ne 3 ] ; then  # Must have TWO command-line args
	echo "Usage: $0 </dev/device> <dms src dir> <leave dir mounted: y/n>"
	echo "1st arg: /dev/ice, 2nd: path to dm-mirror_sync source dir, Second: y/n to leave mounted"
	exit -1
fi

# CAUTION: THIS TEST OVERWRITES THE WHOLE BLOCK DEVICE USED!

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
	dname=`echo $blkdev | cut -c13-`
	devid=`cat /sys/block/dm-0/dev` # FIXME: not always dm-0 ??
else
	devid=`cat /sys/block/$dname/dev`
fi

echo "Dev: $blkdev bsize=$bsize dname=$dname devid=$devid"

cwddir=`pwd`
srcdir=$2
leave_dir_mounted=$3

mkfscmd="/sbin/mkfs.xfs -f -b size=4096 -L DMS_Test"
mountcmd="/bin/mount -t xfs"
umountcmd="/bin/umount"
ddcmd="/bin/dd oflag=direct"

dev_mounted=`/bin/mount | grep $dname | wc -l`
mntdir=/mnt/pmtest

benchdir=bench
pmexe=run_postmark
pmconfig=pm_test.cfg
pmtmpcfg=pm_tmp.tst
pmdir=$mntdir/pmtest

# CONFIGURE THIS FOR SELECTING SPECIFIC TESTS
run_basic_fs_io_tests=true
run_basic_postmark_tests=true
run_2gb_postmark_tests=true
run_fs_read_change_balance_tests=false

# go to the source dir and look for the modules and bench executable...
cd $srcdir
if [ ! -e $benchdir/$pmexe ] ; then
	echo "Cannot find $benchdir/$pmexe !"
	exit -1
fi
if [ ! -e $mntdir ] ; then
	mkdir -p $mntdir
fi

# FIXME: get the actual sizes from the devices via blockdev + create max size dm table

# dev mounted and/or module loaded??
sync;sync;sync
if [ $dev_mounted -eq 0 ] ; then

	if [ ! -b $blkdev ] ; then
		echo "Device does not exist! Aborting..."
	fi

	sync;sync;sync
	sleep 1
	$mkfscmd $blkdev
	sync;sync;sync
	$mountcmd $blkdev $mntdir
	sync;sync;sync
	sleep 1
	if [ ! -e $pmdir ] ; then
		mkdir -p $pmdir
	fi
	sync;sync;sync
fi

#check if everything is loaded ok, else exit...
sync;sync;sync
dev_mounted=`/bin/mount | grep $dname | wc -l`

if [ $dev_mounted -eq 0 ] || [ ! -b $blkdev ] ; then
	echo "Could not load device! Aborting..."
	exit -1
fi

if $run_basic_fs_io_tests ; then
#if [ $run_all_tests -o $run_basic_fs_io_tests ]; then
# -----------------------------------------------------------------------
	echo 'STARTING DD WRITES!'
	sync;sync;sync
	$ddcmd if=/dev/zero of=$mntdir/dd.out bs=4096 count=10K
	echo 'DONE WITH DD WRITES!'
	sync;sync;sync
	$ddcmd of=/dev/null if=$mntdir/dd.out bs=4096 count=10K
	#$ddcmd of=/dev/null if=$mntdir/dd.out bs=4096 count=10 skip=252
	echo 'DONE WITH DD READS!'
	rm -f $mntdir/dd.out
	sync;sync;sync
# -----------------------------------------------------------------------
fi

if $run_basic_postmark_tests ; then
# -----------------------------------------------------------------------
	echo 'WILL START POSTMARK TEST!'
	sync;sync;sync
	\rm -f /tmp/$pmtmpcfg
	cat > /tmp/$pmtmpcfg <<ENDOFTEST
set size 500 1100000
set read 1048576
set write 1048576
set number 300
set bias read 7
set bias create 4
set transactions 500
set buffering false
set subdirectories 10
set location $pmdir
set report verbose
show
run
quit
ENDOFTEST

	if [ -e $pmdir ] ; then
		echo 'Starting Postmark... please wait...'
		$benchdir/$pmexe < /tmp/$pmtmpcfg
		#\rm -f /tmp/$pmtmpcfg
		echo 'DONE WITH POSTMARK TEST!'
	else
		echo "Cannot find postmark test dir $pmdir ! Aborting postmark test..."
	fi
	sync;sync;sync
	sleep 2
# -----------------------------------------------------------------------
fi

sync;sync;sync

if $run_2gb_postmark_tests ; then
# -----------------------------------------------------------------------
	echo 'WILL START POSTMARK TEST!'
	sync;sync;sync
	\rm -f /tmp/$pmtmpcfg
	cat > /tmp/$pmtmpcfg <<ENDOFTEST
set size 500 1100000
set read 1048576
set write 1048576
set number 1000
set bias read 7
set bias create 4
set transactions 3000
set buffering false
set subdirectories 50
set location $pmdir
set report verbose
show
run
quit
ENDOFTEST

	if [ -e $pmdir ] ; then
		echo 'Starting Postmark... please wait...'
		$benchdir/$pmexe < /tmp/$pmtmpcfg
		#\rm -f /tmp/$pmtmpcfg
		echo 'DONE WITH POSTMARK TEST!'
	else
		echo "Cannot find postmark test dir $pmdir ! Aborting postmark test..."
	fi
	sync;sync;sync
	sleep 2
# -----------------------------------------------------------------------
fi

sync;sync;sync

# re-eval the system state
dev_mounted=`/bin/mount | grep $dname | wc -l`

# unmount & unload...
if [ $leave_dir_mounted != "y" ] && [ $dev_mounted -gt 0 ] ; then

	sync;sync;sync
	$umountcmd $mntdir

fi

#back to our directory
cd $cdir

echo 'ALL DONE!'

