#!/bin/bash

# Author: Michail Flouris (C) 2012

# CAUTION: this is ONLY a shortcut for the specific TEST VM SETUP!!
sync;sync;sync

dms_dname1=dms1
dms_dname2=dms2

sdms_dname=sdms

/sbin/dmsetup remove $sdms_dname

echo 'DM Linear REMOVE OK!'

/sbin/dmsetup remove $dms_dname1
/sbin/dmsetup remove $dms_dname2
echo 'DMS REMOVE OK!'

# NOTE: we DO NOT remove the dm-linear module... since we don't modify it...

#/sbin/rmmod dm-mirror_sync.ko
make rmm
echo 'ALL DONE!'

