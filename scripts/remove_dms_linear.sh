#!/bin/bash

# Author: Michail Flouris (C) 2012

# CAUTION: this is ONLY a shortcut for the specific TEST VM SETUP!!
sync;sync;sync

dms_devname=dms
ldms_devname="l$dms_devname"

/sbin/dmsetup remove $ldms_devname
echo 'DM Linear REMOVE OK!'

/sbin/dmsetup remove $dms_devname
echo 'DMS REMOVE OK!'

# NOTE: we DO NOT remove the dm-linear module... since we don't modify it...

#/sbin/rmmod dm-mirror_sync.ko
make rmm
echo 'ALL DONE!'

