#!/bin/bash

# Author: Michail Flouris (C) 2012

# CAUTION: this is ONLY a shortcut for the specific TEST VM SETUP!!

dms_devname=dms
ldms_devname="l$dms_devname"

dms_device="/dev/mapper/$dms_devname"
ldms_device="/dev/mapper/$ldms_devname"

if [ ! -b $ldms_device ] || [ ! -b $dms_device ]; then
	echo "Device(s) $ldms_device and/or $dms_device does not exist!"
	exit -1
fi

dmssize=`/sbin/blockdev --getsz $dms_device`
echo DMS Size: $dmssize

ldmssize=`/sbin/blockdev --getsz $ldms_device`
echo Linear Size: $ldmssize

if [ -z $dmssize ] || [ -z $ldmssize ]; then
	echo "Could not read device $ldms_devname and/or $dms_devname size!"
	exit -1
fi

# l /dev/mapper/
# /sbin/dmsetup table /dev/mapper/$ldms_devname 
# /sbin/dmsetup deps /dev/mapper/$ldms_devname 

# /sbin/dmsetup table /dev/mapper/$dms_devname 
# /sbin/dmsetup deps /dev/mapper/$dms_devname 

# /sbin/dmsetup remove dms - DOES NOT WORK if linear is on top!

echo -n '[INITIAL] DM Linear STATUS:'
/sbin/dmsetup status $ldms_devname
echo -n '[INITIAL] DMS STATUS:'
/sbin/dmsetup status $dms_devname

#dread_policy="io_balance round_robin ios 64"
#dread_policy="io_balance weighted dev_weight 50"
dread_policy="io_balance logical_part io_chunk 512"
/sbin/dmsetup message $dms_devname 0 "$dread_policy"
echo 'DMS CHANGE READ POLICY OK!'

sync;sync;sync
/sbin/dmsetup suspend $ldms_devname 
echo 'DM Linear SUSPEND OK!'
/sbin/dmsetup suspend $dms_devname
echo 'DMS SUSPEND OK!'
sleep 5

echo -n '[BEFORE] DM Linear STATUS:'
/sbin/dmsetup status $ldms_devname
echo -n '[BEFORE] DMS STATUS:'
/sbin/dmsetup status $dms_devname

echo 'Reloading DMS with NEW table!'
#dms_devs="2 /dev/sdb 0 /dev/sdc 0"
#dms_devs="2 /dev/sde 0 /dev/sdf 0"
#dms_devs="3 /dev/sdb 0 /dev/sdc 0 /dev/sdd 0"
#dms_devs="3 /dev/sdb 0 /dev/sdc 0 /dev/sde 0"
dms_devs="3 /dev/sde 0 /dev/sdf 0 /dev/sdg 0"
/sbin/dmsetup reload $dms_devname --table "0 $dmssize mirror_sync core 2 64 nosync $dms_devs"
echo 'DMS RELOAD OK!'
sync;sync;sync
sleep 1

echo 'Resuming DM Linear...'
/sbin/dmsetup resume $ldms_devname
echo 'DM Linear RESUME OK!'
/sbin/dmsetup resume $dms_devname
echo 'DMS RESUME OK!'

echo -n '[BEFORE] DM Linear STATUS:'
/sbin/dmsetup status $ldms_devname
echo -n '[BEFORE] DMS STATUS:'
/sbin/dmsetup status $dms_devname

#/sbin/dmsetup remove $ldms_devname
#echo 'DM Linear REMOVE OK!'

#/sbin/dmsetup remove $dms_devname
#echo 'DMS REMOVE OK!'

sync;sync;sync
echo 'ALL DONE!'

