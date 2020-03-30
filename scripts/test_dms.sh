#!/bin/bash

# Original script code for device mapper mirror_sync device setup & testing...
# Author: Michail Flouris (C) 2012

# CAUTION: this is ONLY a shortcut for the specific TEST VM SETUP!!

# CAUTION: THIS TEST OVERWRITES THE WHOLE DEVICES USED!

# => MUST CONFIGURE DESIRED DEVICES HERE!!
#DEV=(sdb sdc) # 2 devs
#DEV=(sdb sdc sdd) # 3 devs
#DEV=(sdb sdc sdd sde) # 4 devs
DEV=(sdb sdc sdd sde sdf) # 5 devs
#DEV=(sdb sdc sdd sde sdf sdg) # 6 devs

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
	#echo "$idx : mindevsize= $mindevsize"
done

dms_devs="${#DEV[@]}"
for idx in ${!DEV[*]}; do
	echo Device $idx: /dev/${dname[$idx]} ID: ${devno[$idx]} - Size: ${bsize[$idx]}
	dms_devs+=" /dev/${dname[$idx]} 0"
done
#echo "dms_devs= $dms_devs"
echo "mindevsize= $mindevsize"

# CONFIGURE THIS FOR SELECTING TESTS
run_basic_dd_io_tests=true
run_status_read_tests=false
run_read_policy_tests=false		# CAUTION: takes a few minutes
run_dt_tests=true

# CAUTION: overrides all previous run_*_tests values...
run_all_tests=true

ddcmd="/bin/dd oflag=direct"
dtcmd=/bin/dt

sync;sync;sync
#/sbin/insmod dm-mirror_sync.ko
make ins || exit -1
echo 'MODULE LOADED!'

/sbin/dmsetup create dms --table "0 $mindevsize mirror_sync core 2 64 nosync $dms_devs"
echo 'DMSETUP CREATE OK!'

#/sbin/dmsetup ls
#ls -la /dev/mapper/*

#/sbin/dmsetup targets
#/sbin/dmsetup info

#check if everything is loaded ok, else exit...
sync;sync;sync
dms_device=/dev/mapper/dms
dms_loaded=`/sbin/lsmod | grep mirror_sync | wc -l`

if [ $dms_loaded -eq 0 ] || [ ! -b $dms_device ] ; then
	echo "Could not load DMS device! Aborting..."
	exit -1
fi

if $run_all_tests || $run_basic_dd_io_tests ; then
# -----------------------------------------------------------------------
	echo 'WILL START DD WRITES!'
	sync;sync;sync
	$ddcmd if=/dev/zero of=$dms_device bs=4096 count=1
	#$ddcmd if=/dev/zero of=$dms_device bs=4096 count=1k
	#$ddcmd if=/dev/zero of=$dms_device bs=1M count=100
	#$ddcmd if=/dev/zero of=$dms_device bs=4096 count=80K
	echo 'DONE WITH DD WRITES!'
	sync;sync;sync

	sleep 1
	echo 'WILL START DD READS!'
	sync;sync;sync
	$ddcmd of=/dev/null if=$dms_device bs=4096 count=10
	#$ddcmd of=/dev/null if=$dms_device bs=4096 count=10 skip=252
	#$ddcmd of=/dev/null if=$dms_device bs=4096 count=8k
	#$ddcmd of=/dev/null if=$dms_device bs=4096 count=80K
	echo 'DONE WITH DD READS!'
	sync;sync;sync
# -----------------------------------------------------------------------
fi

if $run_all_tests || $run_status_read_tests; then
# -----------------------------------------------------------------------
	#/sbin/dmsetup info dms
	#echo 'DMSETUP INFO OK!'

	/sbin/dmsetup status dms
	echo 'DMSETUP STATUS OK!'

	/sbin/dmsetup message dms 0 'io_balance round_robin ios 64'
	/sbin/dmsetup status dms
	echo 'DMSETUP STATUS RR OK!'

	/sbin/dmsetup message dms 0 'io_balance logical_part io_chunk 512'
	/sbin/dmsetup status dms
	echo 'DMSETUP STATUS LP OK!'

	/sbin/dmsetup message dms 0 'io_balance weighted dev_weight 50'
	/sbin/dmsetup status dms
	echo 'DMSETUP STATUS CW OK!'

	/sbin/dmsetup message dms 0 'io_cmd set_weight 0 10'
	/sbin/dmsetup message dms 0 'io_cmd set_weight 1 100'
	/sbin/dmsetup status dms
	echo 'DMSETUP STATUS CW OK!'

	/sbin/dmsetup message dms 0 'io_balance logical_part io_chunk 1024'
# -----------------------------------------------------------------------
fi

if $run_all_tests || $run_dt_tests; then
# -----------------------------------------------------------------------
	echo 'Starting DT Tests...'
	$dtcmd of=$dms_device bs=4k pattern=iot flags=fsync
	echo 'DT TESTS DONE!'
# -----------------------------------------------------------------------
fi

if $run_all_tests || $run_read_policy_tests; then
# -----------------------------------------------------------------------
	# change policy to round robin, updating ios size
	/sbin/dmsetup message dms 0 'io_balance round_robin ios 64'
	echo 'DMSETUP TUNE RR IOS OK!'
	$ddcmd of=/dev/null if=$dms_device bs=4096 count=60K
	sync;sync;sync
	sleep 2

	# change policy to logical partitioning, updating chunk size
	/sbin/dmsetup message dms 0 'io_balance logical_part io_chunk 512'
	echo 'DMSETUP CHANGE TO LOGICAL PARTITIONING POLICY OK!'
	$ddcmd of=/dev/null if=$dms_device bs=4096 count=60K
	sync;sync;sync
	sleep 2

	# change policy to weighted, updating default weight size
	/sbin/dmsetup message dms 0 'io_balance weighted dev_weight 50'
	echo 'DMSETUP CHANGE TO WEIGHTED POLICY OK!'
	$ddcmd of=/dev/null if=$dms_device bs=4096 count=60K
	sync;sync;sync
	sleep 2

	# reset device weights [DEV 0 PRIO]
	echo 'DMSETUP WILL RESET DEV WEIGHTS [DEV0 PRIO]!'
	/sbin/dmsetup message dms 0 'io_cmd set_weight 0 100'
	/sbin/dmsetup message dms 0 'io_cmd set_weight 1 10'
	echo 'DMSETUP RESET DEV WEIGHTS [DEV0 PRIO] OK!'
	$ddcmd of=/dev/null if=$dms_device bs=4096 count=60K
	sync;sync;sync
	sleep 2

	# reset device weights [DEV 1 PRIO]
	echo 'DMSETUP WILL RESET DEV WEIGHTS [DEV1 PRIO]!'
	/sbin/dmsetup message dms 0 'io_cmd set_weight 0 10'
	/sbin/dmsetup message dms 0 'io_cmd set_weight 1 100'
	echo 'DMSETUP RESET DEV WEIGHTS [DEV1 PRIO] OK!'
	$ddcmd of=/dev/null if=$dms_device bs=4096 count=60K
	sync;sync;sync
# -----------------------------------------------------------------------
fi

# print out the I/O counters...
/sbin/dmsetup status dms

/sbin/dmsetup remove dms
echo 'DMSETUP REMOVE OK!'

#/sbin/rmmod dm-mirror_sync.ko
make rmm

echo 'ALL DONE!'

