#!/bin/bash

# Original script code...
# Author: Michail Flouris (C) 2012

if [ $# -ne 2 ] ; then  # Must have TWO command-line args
	echo "Usage: $0 <dms src dir> <leave dir mounted: y/n>"
	echo "First arg: path to dm-mirror_sync root source dir, Second: y/n to leave mounted"
	exit -1
fi

# CAUTION: this is ONLY a shortcut for the specific TEST VM SETUP OVER NBD!!

# CAUTION: The path names in this test may need changes on another host!

# CAUTION: THIS TEST OVERWRITES THE WHOLE DEVICES USED!

# => MUST CONFIGURE DESIRED DEVICES HERE!!
#DEV=(sdb sdc) # 2 devs
#DEV=(sdb sdc sdd) # 3 devs
#DEV=(sdb sdc sdd sde) # 4 devs
DEV=(sdb sdc sdd sde sdf) # 5 devs
#DEV=(sdb sdc sdd sde sdf sdg) # 6 devs

base_nbd_port=4320
mindevsize=0
for idx in ${!DEV[*]}; do
	dname[$idx]=${DEV[$idx]}
	#bsize[$idx]=`/sbin/blockdev --getsz /dev/${dname[$idx]}`
	bsize[$idx]=`/sbin/blockdev --getsize /dev/${dname[$idx]}`
	devid[$idx]=`cat /sys/block/${dname[$idx]}/dev`
	nbddev[$idx]="nbd$idx"
	nbdport[$idx]=$(($base_nbd_port+$idx))

	if [ $idx == '0' ]; then
		mindevsize=$((${bsize[$idx]}))
	elif [ $mindevsize -gt $((${bsize[$idx]})) ]; then
		mindevsize=$((${bsize[$idx]}))
	fi

	if [ ! -e "/sys/block/${dname[$idx]}" ] || [ ! -e "/dev/${dname[$idx]}" ]; then
		echo "Device /dev/${dname[$idx]} does not exist!"
		exit -1
	fi
	# tune the device scheduler...
	#echo noop > /sys/block/${dname[$idx]}/queue/scheduler

	#echo "$idx : mindevsize= $mindevsize"
done

devcount="${#DEV[@]}"
nbd_devs="${#DEV[@]}"
for idx in ${!DEV[*]}; do
	echo Device $idx: /dev/${dname[$idx]} ID: ${devno[$idx]} - Size: ${bsize[$idx]}
	nbd_devs+=" /dev/${nbddev[$idx]} 0"
done
echo "nbd_devs= $nbd_devs"
echo "mindevsize= $mindevsize"

cwddir=`pwd`
srcdir=$1
leave_dir_mounted=$2

# ATTENTION: MUST Configure correctly the binaries of nbd to use:
# the original or the statically-compiled nbd server & client from ramdisk_builder?
use_orig_nbd=true
if $use_orig_nbd ; then
	#nbd_bindir=../nbd_orig_bin
	nbd_bindir=/home/flouris/projects/onapp/nbd_orig_bin
	echo "Using ORIGINAL nbd binaries: $nbd_bindir"
else
	#nbd_bindir=../nbd_bin
	nbd_bindir=/home/flouris/projects/onapp/nbd_bin
	# The following are just for error handling tests with the nbd that fails...
	# FIXME: remove these??
	nbd_ok_bindir=/home/flouris/projects/onapp/nbd_orig_bin
	nbd_ok_server=$nbd_ok_bindir/nbd-server
	nbd_ok_client=$nbd_ok_bindir/nbd-client
	echo "Using OUR CUSTOM nbd binaries: $nbd_bindir"
fi

nbd_server=$nbd_bindir/nbd-server
nbd_client=$nbd_bindir/nbd-client
if [ ! -f $nbd_server ] || [ ! -e $nbd_client ] ; then
	echo "Could not find nbd binaries ! Aborting..."
	exit -1
fi

mkfscmd="/sbin/mkfs.xfs -f -b size=4096 -L DMS_Test"
mountcmd="/bin/mount -t xfs"
umountcmd="/bin/umount"
ddcmd="/bin/dd oflag=direct"

use_orig_dm_mirror=false # user the ORIGINAL dm-mirror module ??
if $use_orig_dm_mirror ; then
	dms_name=mirror
	dms_module=/lib/modules/`uname -r`/kernel/drivers/md/dm-$dms_name
else
	dms_name=mirror_sync
	dms_module=dm-$dms_name
fi

dms_loaded=`/sbin/lsmod | grep $dms_name | wc -l`
dms_mounted=`/bin/mount | grep dms | wc -l`
dms_device=/dev/mapper/dms
dms_mntdir=/mnt/dms

benchdir=bench
pmexe=run_postmark
pmconfig=pm_test.cfg
pmtmpcfg=pm_tmp.tst
pmdir=$dms_mntdir/pmtest

# CONFIGURE THIS FOR SELECTING SPECIFIC TESTS
run_basic_fs_io_tests=true
run_basic_postmark_tests=true
run_2gb_postmark_tests=true
run_fs_read_change_balance_tests=false

# go to the source dir and look for the modules and bench executable...
cd $srcdir
if [ ! -e */$dms_module.ko ] || [ ! -e $benchdir/$pmexe ] ; then
	echo "Cannot find $dms_module.ko or $benchdir/$pmexe !"
	echo "Perhaps you should run make?"
	exit -1
fi
#if [ ! -e $benchdir/$pmconfig ] ; then
#	echo "Cannot find benchmark config file: $benchdir/$pmconfig !"
#	exit -1
#fi
if [ ! -e $dms_mntdir ] ; then
	mkdir -p $dms_mntdir
fi

# FIXME: get the actual sizes from the devices via blockdev + create max size dm table

sync;sync;sync
nbd_loaded=`/sbin/lsmod | grep nbd | wc -l`
nbd_up=`ps gaux | grep nbd | grep -v grep | wc -l`
if [ $nbd_loaded -eq 0 ] ; then
	/sbin/modprobe nbd
	echo 'NBD MODULE LOADED!'
fi

#setup the nbd server & client...
for idx in ${!DEV[*]}; do
	echo Starting nbd-server on device $idx: /dev/${dname[$idx]} - Port: ${nbdport[$idx]}
	# FIXME: remove that??
	if [ $idx == '1' ]; then
		echo 'STARTING OK SERVER!!'
		#$nbd_ok_server ${nbdport[$idx]} /dev/${dname[$idx]}
		$nbd_server ${nbdport[$idx]} /dev/${dname[$idx]}
	else
		$nbd_server ${nbdport[$idx]} /dev/${dname[$idx]}
	fi
	#$nbd_server ${nbdport[$idx]} /dev/${dname[$idx]}
done
echo 'NBD SERVER SETUP OK!'

for idx in ${!DEV[*]}; do
	echo Starting nbd-client on device $idx: /dev/${nbddev[$idx]} - Port: ${nbdport[$idx]}
	if [ $idx == '1' ]; then
		echo 'STARTING OK CLIENT!!'
		#$nbd_ok_client localhost ${nbdport[$idx]} /dev/${nbddev[$idx]} -b 4096
		$nbd_client localhost ${nbdport[$idx]} /dev/${nbddev[$idx]} -b 4096
	else
		$nbd_client localhost ${nbdport[$idx]} /dev/${nbddev[$idx]} -b 4096
	fi
	#$nbd_client localhost ${nbdport[$idx]} /dev/${nbddev[$idx]} -b 4096
done
echo 'NBD CLIENT SETUP OK!'
sync;sync;sync

# dms mounted and/or module loaded??
sync;sync;sync
if [ $dms_mounted -eq 0 ] ; then

	if [ $dms_loaded -eq 0 ] ; then

		echo "Loading DMS module..."
		#/sbin/insmod $dms_module.ko
		make ins || exit -1
		echo "DMS module ($dms_module) loaded!"
	fi

	nbd_loaded=`/sbin/lsmod | grep nbd | wc -l`
	nbd_up=`ps gaux | grep nbd | grep -v grep | wc -l`
	if [ $nbd_up -eq 0 ] || [ $nbd_loaded -eq 0 ] ; then
		echo "Could not load or start nbd [BEFORE DMS]! Aborting..."
		exit -1
	fi

	if [ ! -b $dms_device ] ; then
		echo "Creating DMS device & map..."
		/sbin/dmsetup create dms --table "0 $mindevsize $dms_name core 2 64 nosync $nbd_devs"
		echo 'DMSETUP Created OK!'
		#echo Size: `/sbin/blockdev --getsize $dms_device`
	fi

	sync;sync;sync
	sleep 1
	$mkfscmd $dms_device
	sync;sync;sync
	$mountcmd $dms_device $dms_mntdir
	sync;sync;sync
	sleep 1
	if [ ! -e $pmdir ] ; then
		mkdir -p $pmdir
	fi
	sync;sync;sync
fi

#check if everything is loaded ok, else exit...
sync;sync;sync
dms_loaded=`/sbin/lsmod | grep $dms_name | wc -l`
dms_mounted=`/bin/mount | grep dms | wc -l`
nbd_loaded=`/sbin/lsmod | grep nbd | wc -l`
nbd_up=`ps gaux | grep nbd | grep -v grep | wc -l`

if [ $nbd_up -eq 0 ] || [ $nbd_loaded -eq 0 ] ; then
	echo "Could not load or start nbd! Aborting..."
	exit -1
fi
if [ $dms_mounted -eq 0 ] || [ $dms_loaded -eq 0 ] || [ ! -b $dms_device ] ; then
	echo "Could not load device! Aborting..."
	exit -1
fi

#/sbin/dmsetup ls
#ls -la /dev/mapper/*

#/sbin/dmsetup targets
#/sbin/dmsetup info

if $run_basic_fs_io_tests ; then
#if [ $run_all_tests -o $run_basic_fs_io_tests ]; then
# -----------------------------------------------------------------------
	echo 'STARTING DD WRITES!'
	sync;sync;sync
	$ddcmd if=/dev/zero of=$dms_mntdir/dd.out bs=4096 count=10K
	echo 'DONE WITH DD WRITES!'
	sync;sync;sync
	$ddcmd of=/dev/null if=$dms_mntdir/dd.out bs=4096 count=10K
	#$ddcmd of=/dev/null if=$dms_mntdir/dd.out bs=4096 count=10 skip=252
	echo 'DONE WITH DD READS!'
	rm -f $dms_mntdir/dd.out
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
/sbin/dmsetup status dms

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
/sbin/dmsetup status dms

if $run_fs_read_change_balance_tests; then
#if [ $run_all_tests -o $run_fs_read_change_balance_tests ]; then
# -----------------------------------------------------------------------
	#/sbin/dmsetup info dms
	#echo 'DMSETUP INFO OK!'

	/sbin/dmsetup status dms
	echo 'DMSETUP STATUS OK!'

	/sbin/dmsetup message dms 0 'io_balance round_robin ios 64'
	/sbin/dmsetup status dms
	echo 'DMSETUP STATUS RR OK!'

	/sbin/dmsetup message dms 0 'io_balance logical_part io_chunk 512'
	/sbin/dmsetup status dms
	echo 'DMSETUP STATUS LP OK!'

	/sbin/dmsetup message dms 0 'io_balance weighted dev_weight 50'
	/sbin/dmsetup status dms
	echo 'DMSETUP STATUS CW OK!'

	/sbin/dmsetup message dms 0 'io_cmd set_weight 0 10'
	/sbin/dmsetup message dms 0 'io_cmd set_weight 1 100'
	/sbin/dmsetup status dms
	echo 'DMSETUP STATUS CW OK!'

	/sbin/dmsetup message dms 0 'io_balance logical_part io_chunk 1024'
# -----------------------------------------------------------------------
fi

sync;sync;sync
/sbin/dmsetup status dms

# re-eval the system state
dms_loaded=`/sbin/lsmod | grep $dms_name | wc -l`
dms_mounted=`/bin/mount | grep dms | wc -l`
nbd_loaded=`/sbin/lsmod | grep nbd | wc -l`
nbd_up=`ps gaux | grep nbd | grep -v grep | wc -l`

# unmount & unload...
if [ $leave_dir_mounted != "y" ] ; then 
	if [ $dms_mounted -gt 0 ] || [ $nbd_up -gt 0 ] ; then

	sync;sync;sync
	$umountcmd $dms_mntdir
	sync;sync;sync

	if [ $dms_loaded -gt 0 ] ; then

		echo "Unloading DMS module..."

		/sbin/dmsetup remove dms
		echo 'DMSETUP Remove OK!'

		if [ $nbd_up -gt 0 ] || [ $nbd_loaded -gt 0 ] ; then

		sync;sync;sync
		for idx in ${!DEV[*]}; do
			$nbd_client -d /dev/${nbddev[$idx]}
		done
		sync;sync;sync
		echo 'STOPPED NBD-CLIENTS!'
		sleep 1

		sync;sync;sync
		killall -HUP nbd-client
		sleep 1
		killall -HUP nbd-server
		echo 'KILLED NBD-SERVERS!'
		sleep 1

		/sbin/modprobe -r nbd

		fi

		#/sbin/rmmod $dms_module.ko
		make rmm
		echo "DMS module ($dms_module) unloaded!"
	fi

	fi
fi

#back to our directory
cd $cdir

echo 'ALL DONE!'

