#!/bin/bash

# Original script code...
# Author: Michail Flouris (C) 2012

# CAUTION: this is ONLY a shortcut for the specific TEST VM SETUP OVER NBD!!

# CAUTION: The path names in this test may need changes on another host!

# CAUTION: THIS TEST OVERWRITES THE WHOLE DEVICES USED!

# => MUST CONFIGURE DESIRED DEVICES HERE!!
#DEV=(sdb sdc) # 2 devs
#DEV=(sdb sdc sdd) # 3 devs
#DEV=(sdb sdc sdd sde) # 4 devs
DEV=(sdb sdc sdd sde sdf) # 5 devs
#DEV=(sdb sdc sdd sde sdf sdg) # 6 devs

base_nbd_port=4320
mindevsize=0
for idx in ${!DEV[*]}; do
	dname[$idx]=${DEV[$idx]}
	#bsize[$idx]=`/sbin/blockdev --getsz /dev/${dname[$idx]}`
	bsize[$idx]=`/sbin/blockdev --getsize /dev/${dname[$idx]}`
	devid[$idx]=`cat /sys/block/${dname[$idx]}/dev`
	nbddev[$idx]="nbd$idx"
	nbdport[$idx]=$(($base_nbd_port+$idx))

	if [ $idx == '0' ]; then
		mindevsize=$((${bsize[$idx]}))
	elif [ $mindevsize -gt $((${bsize[$idx]})) ]; then
		mindevsize=$((${bsize[$idx]}))
	fi
	#echo "$idx : mindevsize= $mindevsize"
done

devcount="${#DEV[@]}"
nbd_devs="${#DEV[@]}"
for idx in ${!DEV[*]}; do
	echo Device $idx: /dev/${dname[$idx]} ID: ${devno[$idx]} - Size: ${bsize[$idx]}
	nbd_devs+=" /dev/${nbddev[$idx]} 0"
done
echo "nbd_devs= $nbd_devs"
echo "mindevsize= $mindevsize"

# ATTENTION: MUST Configure correctly the binaries of nbd to use:
# the original or the statically-compiled nbd server & client from ramdisk_builder?
use_orig_nbd=true
if $use_orig_nbd ; then
	#nbd_bindir=../nbd_orig_bin
	nbd_bindir=/home/flouris/projects/onapp/nbd_orig_bin
	echo "Using ORIGINAL nbd binaries: $nbd_bindir"
else
	#nbd_bindir=../nbd_bin
	nbd_bindir=/home/flouris/projects/onapp/nbd_bin
	# The following are just for error handling tests with the nbd that fails...
	# FIXME: remove these??
	nbd_ok_bindir=/home/flouris/projects/onapp/nbd_orig_bin
	nbd_ok_server=$nbd_ok_bindir/nbd-server
	nbd_ok_client=$nbd_ok_bindir/nbd-client
	echo "Using OUR CUSTOM nbd binaries: $nbd_bindir"
fi

nbd_server=$nbd_bindir/nbd-server
nbd_client=$nbd_bindir/nbd-client
if [ ! -f $nbd_server ] || [ ! -e $nbd_client ] ; then
	echo "Could not find nbd binaries ! Aborting..."
	exit -1
fi

ddcmd="/bin/dd oflag=direct"
dtcmd=/bin/dt

# CONFIGURE THIS FOR SELECTING TESTS
run_basic_dd_io_tests=true
run_status_read_tests=false
run_read_policy_tests=false		# CAUTION: takes a few minutes
run_dt_tests=true

# CAUTION: overrides all previous run_*_tests values...
run_all_tests=true

sync;sync;sync
/sbin/modprobe nbd

#/sbin/insmod dm-mirror_sync.ko
make ins || exit -1
echo 'MODULE LOADED!'

#setup the nbd server & client...
for idx in ${!DEV[*]}; do
	echo Starting nbd-server on device $idx: /dev/${dname[$idx]} - Port: ${nbdport[$idx]}
	# FIXME: remove that??
	if [ $idx == '1' ]; then
		echo 'STARTING OK SERVER!!'
		#$nbd_ok_server ${nbdport[$idx]} /dev/${dname[$idx]}
		$nbd_server ${nbdport[$idx]} /dev/${dname[$idx]}
	else
		$nbd_server ${nbdport[$idx]} /dev/${dname[$idx]}
	fi
	#$nbd_server ${nbdport[$idx]} /dev/${dname[$idx]}
done
echo 'NBD SERVER SETUP OK!'

# CAUTION: do NOT use -c it hides any nbd errors!
#$nbd_client -c -p localhost 4321 /dev/nbd0 -b 4096
#$nbd_client localhost 4321 /dev/nbd0 -b 4096

for idx in ${!DEV[*]}; do
	echo Starting nbd-client on device $idx: /dev/${nbddev[$idx]} - Port: ${nbdport[$idx]}
	if [ $idx == '1' ]; then
		echo 'STARTING OK CLIENT!!'
		#$nbd_ok_client localhost ${nbdport[$idx]} /dev/${nbddev[$idx]} -b 4096
		$nbd_client localhost ${nbdport[$idx]} /dev/${nbddev[$idx]} -b 4096
	else
		$nbd_client localhost ${nbdport[$idx]} /dev/${nbddev[$idx]} -b 4096
	fi
	#$nbd_client localhost ${nbdport[$idx]} /dev/${nbddev[$idx]} -b 4096
done
echo 'NBD CLIENT SETUP OK!'
sync;sync;sync

/sbin/dmsetup create dms --table "0 $mindevsize mirror_sync core 2 64 nosync $nbd_devs"
echo 'DMSETUP CREATE OK!'
sleep 1
sync;sync;sync

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
#ls -l /sys/block/nbd0/dev
#cat /sys/block/nbd1/dev

if $run_all_tests || $run_basic_dd_io_tests ; then
# -----------------------------------------------------------------------
	echo 'WILL START DD WRITES!'
	sync;sync;sync
	#$ddcmd if=/dev/zero of=$dms_device bs=4096 count=7
	$ddcmd if=/dev/zero of=$dms_device bs=4096 count=1k
	#$ddcmd if=/dev/zero of=$dms_device bs=1M count=100
	#$ddcmd if=/dev/zero of=$dms_device bs=4096 count=80K
	echo 'DONE WITH DD WRITES!'
	sync;sync;sync
	#ps gaux | grep -i nbd

	sleep 2
	echo 'WILL START DD READS!'
	sync;sync;sync
	#$ddcmd of=/dev/null if=$dms_device bs=4096 count=10
	#$ddcmd of=/dev/null if=$dms_device bs=4096 count=10 skip=252
	$ddcmd of=/dev/null if=$dms_device bs=4096 count=8k
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
	$dtcmd of=$dms_device bs=4k pattern=iot flags=fsync
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

sync;sync;sync
/sbin/dmsetup status dms

/sbin/dmsetup remove dms
echo 'DMSETUP REMOVE OK!'

sync;sync;sync
for idx in ${!DEV[*]}; do
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

sync;sync;sync
#/sbin/rmmod dm-mirror_sync.ko
make rmm

/sbin/modprobe -r nbd

echo 'ALL DONE!'

