#!/bin/bash

# Author: Michail Flouris (C) 2012

# => MUST CONFIGURE DESIRED DEVICES HERE!! (NEED EXACTLY 4 DEVS!)
DEV=(sdb sdc sdd sde) # 4 devs

mindevsize=0
for idx in ${!DEV[*]}; do
	dname[$idx]=${DEV[$idx]}
	#bsize[$idx]=`/sbin/blockdev --getsz /dev/${dname[$idx]}`
	bsize[$idx]=`/sbin/blockdev --getsize /dev/${dname[$idx]}`
	devid[$idx]=`cat /sys/block/${dname[$idx]}/dev`

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

dms_devs="${#DEV[@]}"
for idx in ${!DEV[*]}; do
	echo Device $idx: /dev/${dname[$idx]} ID: ${devno[$idx]} - Size: ${bsize[$idx]}
	dms_devs+=" /dev/${dname[$idx]} 0"
done
#echo "dms_devs= $dms_devs"
echo "mindevsize= $mindevsize"

if [ ${#DEV[@]} -ne 4 ] ; then
	echo "Need EXACTLY 4 devices!"
	exit -1
fi

dms_name=mirror_sync
dms_module=dm-$dms_name
if [ ! -e */$dms_module.ko ] ; then
	echo "Cannot find $dms_module.ko !"
	echo "Perhaps you should run make?"
	exit -1
fi

dms_loaded=`/sbin/lsmod | grep $dms_name | wc -l`

if [ $dms_loaded -eq 0 ] ; then
	echo "Loading DMS module..."
	#/sbin/insmod $dms_module.ko
	make ins || exit -1
	echo "DMS module ($dms_module) loaded!"
fi
sync;sync;sync

dms_devpair1="2 /dev/${dname[0]} 0 /dev/${dname[1]} 0"
dms_devpair2="2 /dev/${dname[2]} 0 /dev/${dname[3]} 0"

dms_dname1=dms1
dms_dev1=/dev/mapper/$dms_dname1
dms_dname2=dms2
dms_dev2=/dev/mapper/$dms_dname2

sdms_dname="sdms"
sdms_dev=/dev/mapper/$sdms_dname
stripe_chunk_kb=128
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
	echo "/sbin/dmsetup create $sdms_dname --table '0 $mindevsize striped 2 $stripe_chunk_kb /dev/mapper/$dms_dname1 0 /dev/mapper/$dms_dname2 0'"
	/sbin/dmsetup create $sdms_dname --table "0 $mindevsize striped 2 $stripe_chunk_kb /dev/mapper/$dms_dname1 0 /dev/mapper/$dms_dname2 0"
	echo 'DMS Created OK!'
	echo Linear Size: `/sbin/blockdev --getsize $sdms_dev`
fi
sync;sync;sync

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

