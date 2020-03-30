#!/bin/bash

# Author: Michail Flouris (C) 2012

# CAUTION: THIS TEST OVERWRITES THE WHOLE DEVICES used !

# => MUST CONFIGURE DESIRED DEVICES HERE!! 
NBDDEV=(nbd0 nbd1)
#NBDDEV=(nbd1 nbd2 nbd3)

server_ipadd='192.168.4.22'
base_nbd_port=60901
#mindevsize=10485760 # 5GB in sectors
mindevsize=1048576 # 1GB in sectors
for idx in ${!NBDDEV[*]}; do
	dname[$idx]=${NBDDEV[$idx]}
	nbddev[$idx]="nbd$idx"
	nbdport[$idx]=$(($base_nbd_port+$idx))
done

nbd_devs="${#NBDDEV[@]}"
for idx in ${!NBDDEV[*]}; do
	echo Device $idx: /dev/${dname[$idx]}
	nbd_devs+=" /dev/${nbddev[$idx]} 0"
done
echo "nbd_devs= $nbd_devs"
echo "mindevsize= $mindevsize"
nbd_block_size=65536

devcount="${#NBDDEV[@]}"
if [ ${#NBDDEV[@]} -lt 2 ] ; then
	echo "Need at least 2 devices!"
	exit -1
fi

# ATTENTION: MUST Configure correctly the binaries of nbd to use:
# the original or the statically-compiled nbd server & client from ramdisk_builder?
use_orig_nbd=false
if $use_orig_nbd ; then
	#nbd_bindir=../nbd_orig_bin
	nbd_bindir=/home/flouris/projects/onapp/nbd_orig_bin
	echo "Using ORIGINAL nbd binaries: $nbd_bindir"
else
	#nbd_bindir=../nbd_bin
	nbd_bindir=/home/flouris/projects/onapp/nbd_bin
	# The following are just for error handling tests with the nbd that fails...
	echo "Using OUR CUSTOM nbd binaries: $nbd_bindir"
fi

nbd_client=$nbd_bindir/nbd-client
if [ ! -e $nbd_client ] ; then
	echo "Could not find nbd binaries ! Aborting..."
	exit -1
fi

dms_name=mirror_sync
dms_module=dm-$dms_name
if [ ! -e */$dms_module.ko ] ; then
	echo "Cannot find $dms_module.ko !"
	echo "Perhaps you should run make?"
	exit -1
fi

nbd_mod_dir=/home/flouris/projects/onapp/nbd_kernel_mod
sync;sync;sync
#/sbin/modprobe nbd
(cd $nbd_mod_dir ; make rmm ; make ins )

dms_loaded=`/sbin/lsmod | grep $dms_name | wc -l`

if [ $dms_loaded -eq 0 ] ; then
	echo "Loading DMS module..."
	#/sbin/insmod $dms_module.ko
	make ins || exit -1
	echo "DMS module ($dms_module) loaded!"
fi
sync;sync;sync

# CAUTION: do NOT use -c it hides any nbd errors!
#$nbd_client -c -p 127.0.0.1 4321 /dev/nbd0 -b $nbd_block_size
#$nbd_client 127.0.0.1 4321 /dev/nbd0 -b $nbd_block_size

dms_table_string=" ${#NBDDEV[@]} "
for idx in ${!NBDDEV[*]}; do
	echo "Connecting nbd-client for device $idx: /dev/${nbddev[$idx]} -> $server_ipadd:${nbdport[$idx]}"
	if [ $idx == '1' ]; then
		echo 'STARTING OK CLIENT!!'
		$nbd_client $server_ipadd ${nbdport[$idx]} /dev/${nbddev[$idx]} -b $nbd_block_size
	else
		$nbd_client $server_ipadd ${nbdport[$idx]} /dev/${nbddev[$idx]} -b $nbd_block_size
	fi
	dms_table_string="$dms_table_string /dev/${nbddev[$idx]} 0 "
done
echo 'NBD CLIENT SETUP OK!'
sync;sync;sync
sleep 3

dms_dname=dms
dms_dev=/dev/mapper/$dms_dname

if [ ! -b $dms_dev ] ; then
	echo "Creating DMS device & map..."
	echo "/sbin/dmsetup create $dms_dname --table '0 $mindevsize $dms_name core 2 64 nosync $dms_table_string'"
	/sbin/dmsetup create $dms_dname --table "0 $mindevsize $dms_name core 2 64 nosync $dms_table_string"
	echo 'DMS Created OK!'
	echo DMS Size: `/sbin/blockdev --getsize $dms_dev`
fi
sync;sync;sync

#check if everything is loaded ok, else exit...
sync;sync;sync
dms_loaded=`/sbin/lsmod | grep mirror_sync | wc -l`

if [ $dms_loaded -eq 0 ] || [ ! -b $dms_dev ] ; then
	echo "Could not load Striped DMS device! Aborting..."
	exit -1
fi
echo 'DONE!'

