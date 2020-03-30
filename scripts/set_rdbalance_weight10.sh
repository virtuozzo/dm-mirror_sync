#!/bin/bash

# Author: Michail Flouris (C) 2012

#default dms name value...
dms_name=dms

if [ $# -gt 1 ] ; then  # Must have up to ONE command-line arg
	echo "Usage: $0 [dms device name]"
	exit -1
elif [ $# -eq 1 ] ; then  # Must have up to ONE command-line arg
	dms_name=$1
fi

dms_device=/dev/mapper/$dms_name
sync;sync;sync

if [ -b $dms_device ] ; then
	/sbin/dmsetup message $dms_name 0 'io_balance weighted dev_weight 50'
	/sbin/dmsetup message $dms_name 0 'io_cmd set_weight 0 10'
	/sbin/dmsetup message $dms_name 0 'io_cmd set_weight 1 100'
	/sbin/dmsetup status $dms_name
	echo 'DMSETUP RECONFIG WEIGHTED 0-1 + STATUS OK!'
else
	echo "Could not find device $dms_device ! Aborting..."
fi
