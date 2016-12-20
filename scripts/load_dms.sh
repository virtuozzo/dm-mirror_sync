#!/bin/bash

# Author: Michail Flouris (C) 2012

# CAUTION: this is ONLY a shortcut for the specific TEST VM SETUP!!
sync;sync;sync

#/sbin/insmod dm-mirror_sync.ko
make ins || exit -1
echo 'MODULE LOADED!'

