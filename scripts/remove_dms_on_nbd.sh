#!/bin/bash

# Original script code...
# Author: Michail Flouris (C) 2012

# CAUTION: this is ONLY a shortcut for the specific TEST VM SETUP OVER NBD!!

# CAUTION: THIS TEST OVERWRITES THE WHOLE DEVICES used !

# => MUST CONFIGURE DESIRED DEVICES HERE!!
#DEV=(sdb sdc) # 2 devs
DEV=(sdb sdc sdd) # 3 devs
#DEV=(sdb sdc sdd sde) # 4 devs
#DEV=(sdb sdc sdd sde sdf) # 5 devs
#DEV=(sdb sdc sdd sde sdf sdg) # 6 devs
sync;sync;sync

# Choose which version of nbd to use: the original or the statically-compiled
# nbd server & client from ramdisk_builder?
use_orig_nbd=true
if $use_orig_nbd ; then
	#nbd_bindir=../nbd_orig_bin
	nbd_bindir=/home/flouris/projects/onapp/nbd_orig_bin
	echo "Using ORIGINAL nbd binaries: $nbd_bindir"
else
	#nbd_bindir=../nbd_bin
	nbd_bindir=/home/flouris/projects/onapp/nbd_bin
	echo "Using OUR CUSTOM nbd binaries: $nbd_bindir"
fi

nbd_server=$nbd_bindir/nbd-server
nbd_client=$nbd_bindir/nbd-client
if [ ! -f $nbd_server ] || [ ! -e $nbd_client ] ; then
	echo "Could not find nbd binaries ! Aborting..."
	exit -1
fi

#check if everything is loaded ok, else exit...
dms_name=mirror_sync
dms_module=dm-$dms_name

sync;sync;sync
dms_device=/dev/mapper/dms
dms_loaded=`/sbin/lsmod | grep $dms_name | wc -l`

if [ $dms_loaded -eq 0 ] || [ ! -b $dms_device ] ; then
	echo "Could not find loaded DMS device! Aborting..."
else
	/sbin/dmsetup remove dms
	echo 'DMSETUP REMOVE OK!'
fi

sync;sync;sync
for idx in ${!DEV[*]}; do
	nbddev[$idx]="nbd$idx"
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

dms_loaded=`/sbin/lsmod | grep $dms_name | wc -l`
if [ $dms_loaded -ne 0 ] ; then
	#/sbin/rmmod $dms_module.ko
	make rmm
fi

/sbin/modprobe -r nbd

echo 'ALL DONE!'
