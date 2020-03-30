#!/bin/bash

# Author: Michail Flouris (C) 2012

# CAUTION: THIS TEST OVERWRITES THE WHOLE DEVICES used !

# => MUST CONFIGURE DESIRED DEVICES HERE!!
NBDDEV=(nbd1 nbd2 nbd3 nbd4) # 4 devs
sync;sync;sync

# Choose which version of nbd to use: the original or the statically-compiled
# nbd server & client from ramdisk_builder?
use_orig_nbd=false
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
if [ ! -e $nbd_client ] ; then
	echo "Could not find nbd binaries ! Aborting..."
	exit -1
fi

#check if everything is loaded ok, else exit...
dms_name=mirror_sync
dms_module=dm-$dms_name

dms_dname1=dms1
dms_dname2=dms2
sdms_dname=sdms

sync;sync;sync
dms_device=/dev/mapper/$sdms_dname
dms_loaded=`/sbin/lsmod | grep $dms_name | wc -l`

if [ $dms_loaded -eq 0 ] || [ ! -b $dms_device ] ; then
	echo "Could not find loaded DMS device! Aborting..."
else
	/sbin/dmsetup remove $sdms_dname
	echo 'DM Linear REMOVE OK!'

	/sbin/dmsetup remove $dms_dname1
	/sbin/dmsetup remove $dms_dname2
	echo 'DMS REMOVE OK!'
fi

sync;sync;sync
for idx in ${!NBDDEV[*]}; do
	nbddev[$idx]="nbd$idx"
	$nbd_client -d /dev/${nbddev[$idx]}
done
sync;sync;sync
echo 'STOPPED NBD-CLIENTS!'
sleep 1

sync;sync;sync
killall -HUP nbd-client
killall -HUP nbd-server
echo 'KILLED NBD-SERVERS!'
sleep 1

dms_loaded=`/sbin/lsmod | grep $dms_name | wc -l`
if [ $dms_loaded -ne 0 ] ; then
	#/sbin/rmmod $dms_module.ko
	make rmm
fi

nbd_mod_dir=/home/flouris/projects/onapp/nbd_kernel_mod
sync;sync;sync
#/sbin/modprobe -r nbd
(cd $nbd_mod_dir ; make rmm )

echo 'ALL DONE!'
