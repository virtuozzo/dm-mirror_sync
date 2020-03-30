#!/bin/bash

# Original script code for measuring a block device with xdd
# Author: Michail Flouris (C) 2012

# CAUTION: THIS TEST OVERWRITES THE WHOLE BLOCK DEVICE USED!

if [ $# -ne 2 ] ; then  # Must have TWO command-line args
    echo "Usage: $0 <block /dev/ice> <dir for results>"
    exit -1
fi

#GLOBAL SETTINGS (MUST SET THESE RIGHT !!)
benchdir=bench
xddbin=xdd.linux
#total_mb=16384 # 16GBytes of data by default
total_mb=1024 # 1GByte of data by default
numpasses=2

# Determine the result directory path !
cwddir=`pwd`
blkdev=$1
result_dir=$2
dname=`echo $blkdev | cut -c6-`
if [ `echo $blkdev | cut -c1-5` != '/dev/' ] ; then
	echo "Device name does not start with /dev/ ! Aborting..."
	exit -1
fi
if [ ! -b $blkdev ] ; then
	echo "Block device $blkdev does not exist! Aborting..."
	exit -1
fi
if [ ! -e $result_dir ] ; then
    echo "Directory $result_dir does not exist! Aborting..."
    exit -1
fi
if [ ! -f $benchdir/$xddbin ] ; then
    echo "Benchmark executable $benchdir/$xddbin does not exist! Aborting..."
    exit -1
fi
if [ ! -f /usr/bin/time ] ; then
    echo "'/usr/bin/time' does not exist! Please install it..."
    exit -1
fi

bsize=$(( `/sbin/blockdev --getsize $blkdev` ))
if [ `echo $blkdev | grep mapper` ]; then
	dname=`echo $blkdev | cut -c13-`
	devid=`cat /sys/block/dm-0/dev` # FIXME: not always dm-0 ??
else
	devid=`cat /sys/block/$dname/dev`
fi
bsize_mb=$(( ( $bsize / 2048 ) - 8 )) # blksize is in sectors...
echo "Dev: $blkdev bsize=$bsize dname=$dname devid=$devid"

# IMPORTANT: check if the device size is at least equal to $total_mb !
if [ $total_mb -ge $bsize_mb ]; then
	echo "WARNING! Device is smaller than $total_mb MBytes!"
	echo -n "Do you want to continue testing with $bsize_mb MBytes (max dev size)? (y/n):"
	read stop
	if [ "$stop" == "y" ]; then
		total_mb=$(( $bsize_mb - 10 ))
		echo "OK, Total MB is now $bsize_mb ..."
	else
		echo "Aborting..."
		exit 1
	fi
fi

# MUST SET THESE AS GLOBAL !
benchdesc="INVALID"
describe_run="NONE"
xddcmd=""
run_timestamp=`date +%F@%T`
max_timelimit=120

blocksize=4096 # 4 KB min block size
blksinmb=$(( 1024 * 1024 / $blocksize ))
randrange=$(( $total_mb * $blksinmb ))

#____________________________________________________________________________________
# the actual command lines for the various workloads / phases

function get_xdd_cmd
{
if [ $# -ne 4 ] ; then  # Must have 4 args
    echo "Function get_xdd_cmd requires 4 arguments!"
    exit -1
fi
workload=$1
reqsize=$2
kbsize=$3
queuedepth=$4

#set to null to catch any bugs
xddcmd=''

# SEQ WRITES
if [ $workload == 'seqwr' ] ; then
	benchdesc="write.seq.$total_mb"
	#describe_run="SEQUENTIAL writes, BlockSize: 4KB, ReqSize: $kbsize KB, Total Data: $total_mb MB"
	describe_run="SEQUENTIAL writes, ReqSize: $kbsize KB, Total Data: $total_mb MB"
	xddcmd="$benchdir/$xddbin -op write -targets 1 $blkdev -blocksize 4096 -reqsize $reqsize -mbytes $total_mb -passes $numpasses -queuedepth $queuedepth -timelimit $max_timelimit -dio -verbose"

# SEQ READS
elif [ $workload == 'seqrd' ] ; then
	benchdesc="read.seq.$total_mb"
	#describe_run="SEQUENTIAL reads, BlockSize: 4KB, ReqSize: $kbsize KB, Total Data: $total_mb MB"
	describe_run="SEQUENTIAL reads, ReqSize: $kbsize KB, Total Data: $total_mb MB"
	xddcmd="$benchdir/$xddbin -op read -targets 1 $blkdev -blocksize 4096 -reqsize $reqsize -mbytes $total_mb -passes $numpasses -queuedepth $queuedepth -timelimit $max_timelimit -dio -verbose"

# SEQ READ/WRITE MIX
elif [ $workload == 'seqmix' ] ; then
	benchdesc="rwmix.seq.$total_mb"
	#describe_run="SEQUENTIAL rw-mix, BlockSize: 4KB, ReqSize: $kbsize KB, Total Data: $total_mb MB"
	describe_run="SEQUENTIAL rw-mix, ReqSize: $kbsize KB, Total Data: $total_mb MB"
	xddcmd="$benchdir/$xddbin -rwratio 70 -targets 1 $blkdev -blocksize 4096 -reqsize $reqsize -mbytes $total_mb -passes $numpasses -queuedepth $queuedepth -timelimit $max_timelimit -dio -verbose"

##############################
#   RANDOM I/O MEASUREMENTS
##############################

# RANDOM WRITES
elif [ $workload == 'randwr' ] ; then
	benchdesc="write.rand.$total_mb"
	#describe_run="RANDOM writes, BlockSize: 4KB, ReqSize: $kbsize KB, Total Data: $total_mb MB"
	describe_run="RANDOM writes, ReqSize: $kbsize KB, Total Data: $total_mb MB"
	xddcmd="$benchdir/$xddbin -op write -targets 1 $blkdev -blocksize 4096 -seek random -seek range $randrange -reqsize $reqsize -mbytes $total_mb -passes $numpasses -randomize -queuedepth $queuedepth -timelimit $max_timelimit -dio -verbose"

# RANDOM READS
elif [ $workload == 'randrd' ] ; then
	benchdesc="read.rand.$total_mb"
	#describe_run="RANDOM reads, BlockSize: 4KB, ReqSize: $kbsize KB, Total Data: $total_mb MB"
	describe_run="RANDOM reads, ReqSize: $kbsize KB, Total Data: $total_mb MB"
	xddcmd="$benchdir/$xddbin -op read -targets 1 $blkdev -blocksize 4096 -seek random -seek range $randrange -reqsize $reqsize -mbytes $total_mb -passes $numpasses -randomize -queuedepth $queuedepth -timelimit $max_timelimit -dio -verbose"

# RANDOM READ/WRITE MIX
elif [ $workload == 'randmix' ] ; then
	benchdesc="rwmix.rand.$total_mb"
	#describe_run="RANDOM rw-mix, BlockSize: 4KB, ReqSize: $kbsize KB, Total Data: $total_mb MB"
	describe_run="RANDOM rw-mix, ReqSize: $kbsize KB, Total Data: $total_mb MB"
	xddcmd="$benchdir/$xddbin -rwratio 70 -targets 1 $blkdev -blocksize 4096 -seek random -seek range $randrange -reqsize $reqsize -mbytes $total_mb -passes $numpasses -randomize -queuedepth $queuedepth -timelimit $max_timelimit -dio -verbose"
else
	echo "Unknown workload: $workload !"
	exit -1
fi
}


#____________________________________________________________________________________
# This starts all the xdd runs...
start_xdd_runs()
{
# ATTENTION: xdd cannot handle deep queuedepths effectively!
#queuedepths=( 1 2 4 8 16 )
queuedepths=( 1 )

# six basic workloads (must be supported by get_xdd_cmd
#workloads=( 'seqwr' 'seqrd' 'seqmix' 'randwr' 'randrd' 'randmix' )
workloads=( 'seqwr' 'seqrd' 'seqmix' 'randwr' 'randrd' 'randmix' )
#workloads=( 'seqwr' 'seqrd' 'seqmix' )
#workloads=( 'randwr' 'randrd' 'randmix' )

echo "INIT XDD!"
for qd in ${queuedepths[@]} ; do

	queuedepth=$(( $qd ))

	for wkl in ${workloads[@]} ; do

		# current workload / phase
		workload=$wkl

		# range of request sizes
		rqsizes=( 1 2 4 8 16 32 64 128 256 )

		for rqs in ${rqsizes[@]} ; do

			reqsize=$(( $rqs ))
			kbsize=$(( 4 * $reqsize ))

			# get the next xdd run (inc workload ptr too !)
			get_xdd_cmd $workload $reqsize $kbsize $queuedepth
			#echo "RUN XDDCMD: $xddcmd"

			if [ -z "$xddcmd" ]; then
			  echo "Command undefined in function get_xdd_cmd() !"
			  exit -1
			fi

			sync;sync;sync
			echo "# ================================================================================"
			echo "# ===>>> Date: `date`"
			echo "# Running XDD command on `hostname` Dev: $blkdev"
			echo "# ===>>> [ QD: $queuedepth ] >> $describe_run <<<==="

			#result_file="$result_dir/result.xdd.$run_timestamp.$benchdesc.QD$queuedepth.txt"
			result_file="$result_dir/result.xdd.$run_timestamp.$benchdesc.txt"
			touch $result_file
	
			echo "# =========================================" >> $result_file
			echo "# START ON: `date`" >> $result_file
			echo "# CMD: $xddcmd" >> $result_file
			#echo "CMD: $xddcmd"
			/usr/bin/time $xddcmd >> $result_file
			echo "# FINISH ON: `date`" >> $result_file
			echo "# =========================================" >> $result_file
	
			echo "# ================================================================================"
			echo "# >>> Block Size $kbsize KB done !"
			sync;sync;sync
			sleep 10
		done

		echo "# ================================================================================"
		echo "# >>> ALL Done for workload $wkl (Date: `date`)"
	done

	echo "# ================================================================================"
	echo "# >>> ALL Done for Queuedepth $qd... (Date: `date`)"
done
}

start_xdd_runs

echo "DONE!"
