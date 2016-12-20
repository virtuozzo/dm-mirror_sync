#!/bin/bash

# Author: Michail Flouris (C) 2012

# CAUTION: this is ONLY a shortcut for the specific TEST VM SETUP!!
sync;sync;sync

dms_devname=dms

/sbin/dmsetup remove $dms_devname
echo 'DMSETUP REMOVE OK!'

#/sbin/rmmod dm-mirror_sync.ko
make rmm
echo 'ALL DONE!'

