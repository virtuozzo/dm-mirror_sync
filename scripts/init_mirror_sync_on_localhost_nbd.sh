#!/bin/bash

# Original script code...
# Author: Michail Flouris (C) 2012

# => MUST CONFIGURE DESIRED DEVICES HERE!!
#DEV=(sdb sdc) # 2 devs
DEV=(sdb sdc sdd) # 3 devs
#DEV=(sdb sdc sdd sde) # 4 devs
#DEV=(sdb sdc sdd sde sdf) # 5 devs
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
#$nbd_client -c -p localhost 4321 /dev/nbd0 -b 4096
#$nbd_client localhost 4321 /dev/nbd0 -b 4096

for idx in ${!DEV[*]}; do
	echo Starting nbd-client on device $idx: /dev/${nbddev[$idx]} - Port: ${nbdport[$idx]}
	if [ $idx == '1' ]; then
		echo 'STARTING OK CLIENT!!'
		$nbd_client localhost ${nbdport[$idx]} /dev/${nbddev[$idx]} -b 4096
	else
		$nbd_client localhost ${nbdport[$idx]} /dev/${nbddev[$idx]} -b 4096
	fi
	#$nbd_client localhost ${nbdport[$idx]} /dev/${nbddev[$idx]} -b 4096
done
echo 'NBD CLIENT SETUP OK!'
sync;sync;sync

dms_devname=dms
dms_device=/dev/mapper/$dms_devname
if [ ! -b $dms_device ] ; then
	echo "Creating DMS device & map..."
	/sbin/dmsetup create $dms_devname --table "0 $mindevsize $dms_name core 2 64 nosync $nbd_devs"
	echo 'DMSETUP Created OK!'
	echo Size: `/sbin/blockdev --getsize $dms_device`
	sleep 1
fi
sync;sync;sync

#check if everything is loaded ok, else exit...
sync;sync;sync
dms_loaded=`/sbin/lsmod | grep mirror_sync | wc -l`

if [ $dms_loaded -eq 0 ] || [ ! -b $dms_device ] ; then
	echo "Could not load DMS device! Aborting..."
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

