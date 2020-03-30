#!/bin/bash

# Author: Michail Flouris (C) 2012

# => MUST CONFIGURE DESIRED DEVICES HERE!! (NEED EXACTLY 4 DEVS!)
DEV=(sdb sdc sdd sde) # 4 devs

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

nbd_devs="${#DEV[@]}"
for idx in ${!DEV[*]}; do
	echo Device $idx: /dev/${dname[$idx]} ID: ${devno[$idx]} - Size: ${bsize[$idx]}
	nbd_devs+=" /dev/${nbddev[$idx]} 0"
done
echo "nbd_devs= $nbd_devs"
echo "mindevsize= $mindevsize"
nbd_block_size=65536

devcount="${#DEV[@]}"
if [ ${#DEV[@]} -ne 4 ] ; then
	echo "Need EXACTLY 4 devices!"
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

nbd_server=$nbd_bindir/nbd-server
nbd_client=$nbd_bindir/nbd-client
if [ ! -f $nbd_server ] || [ ! -e $nbd_client ] ; then
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

# CAUTION: do NOT use -c it hides any nbd errors!
#$nbd_client -c -p 127.0.0.1 4321 /dev/nbd0 -b $nbd_block_size
#$nbd_client 127.0.0.1 4321 /dev/nbd0 -b $nbd_block_size

for idx in ${!DEV[*]}; do
	echo Starting nbd-client on device $idx: /dev/${nbddev[$idx]} - Port: ${nbdport[$idx]}
	if [ $idx == '1' ]; then
		echo 'STARTING OK CLIENT!!'
		$nbd_client 127.0.0.1 ${nbdport[$idx]} /dev/${nbddev[$idx]} -b $nbd_block_size
	else
		$nbd_client 127.0.0.1 ${nbdport[$idx]} /dev/${nbddev[$idx]} -b $nbd_block_size
	fi
	#$nbd_client 127.0.0.1 ${nbdport[$idx]} /dev/${nbddev[$idx]} -b $nbd_block_size
done
echo 'NBD CLIENT SETUP OK!'
sync;sync;sync
sleep 5

dms_devpair1="2 /dev/${nbddev[0]} 0 /dev/${nbddev[1]} 0"
dms_devpair2="2 /dev/${nbddev[2]} 0 /dev/${nbddev[3]} 0"

dms_dname1=dms1
dms_dev1=/dev/mapper/$dms_dname1
dms_dname2=dms2
dms_dev2=/dev/mapper/$dms_dname2

sdms_dname="sdms"
sdms_dev=/dev/mapper/$sdms_dname
stripe_chunk_kb=256
stripe_chunk_secs=$(( $stripe_chunk_kb * 2 ))
if [ ! -b $sdms_dev ] ; then
	echo "Creating DMS device & map 1..."
	echo "/sbin/dmsetup create $dms_dname1 --table '0 $mindevsize $dms_name core 2 64 nosync $dms_devpair1'"
	/sbin/dmsetup create $dms_dname1 --table "0 $mindevsize $dms_name core 2 64 nosync $dms_devpair1"
	echo 'DMS 1 Created OK!'
	echo DMS 1 Size: `/sbin/blockdev --getsize $dms_dev1`

	echo "Creating DMS device & map 2..."
	echo "/sbin/dmsetup create $dms_dname2 --table '0 $mindevsize $dms_name core 2 64 nosync $dms_devpair2'"
	/sbin/dmsetup create $dms_dname2 --table "0 $mindevsize $dms_name core 2 64 nosync $dms_devpair2"
	echo 'DMS 2 Created OK!'
	echo DMS 2 Size: `/sbin/blockdev --getsize $dms_dev2`

	echo "Creating DM Stripe device & map..."
	echo "/sbin/dmsetup create $sdms_dname --table '0 $mindevsize striped 2 $stripe_chunk_secs /dev/mapper/$dms_dname1 0 /dev/mapper/$dms_dname2 0'"
	/sbin/dmsetup create $sdms_dname --table "0 $mindevsize striped 2 $stripe_chunk_secs /dev/mapper/$dms_dname1 0 /dev/mapper/$dms_dname2 0"
	echo 'DMS Created OK!'
	echo Linear Size: `/sbin/blockdev --getsize $sdms_dev`
fi
sync;sync;sync

#check if everything is loaded ok, else exit...
sync;sync;sync
dms_loaded=`/sbin/lsmod | grep mirror_sync | wc -l`

if [ $dms_loaded -eq 0 ] || [ ! -b $sdms_dev ] ; then
	echo "Could not load Striped DMS device! Aborting..."
	exit -1
fi
echo 'DONE!'

# DMSETUP CMD INFO:
# =================

# dmsetup targets
# -> shows the loaded device mapper targets... mirror_sync should be in there if loaded!

# Setup of 'normal' mirror:
# dmsetup create dms --table '0 400430 mirror core 2 64 nosync 2 /dev/sdb 0 /dev/sdc 0'

# Setup of mirror_sync:
# dmsetup create dms --table '0 400430 mirror_sync core 2 64 nosync 2 /dev/sdb 0 /dev/sdc 0'

# Removal of dms device
# dmsetup remove dms

# Useful commands for dms device
# dmsetup table dms
# dmsetup ls
# dmsetup info
# dmsetup targets

