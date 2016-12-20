# dm-mirror_sync

Device-mapper synchronous mirroring and read load-balancing driver
-------------------------------------------------------------------

Old (backwards-compatible to default dm-mirror) loading:

% /sbin/dmsetup create dms --table '0 4405248 mirror_sync core 2 64 nosync 2 /dev/sdc 0 /dev/sdd 0'
% /sbin/dmsetup status dms
0 4405248 mirror_sync 2 RR,ios=8 0,8:32,A 1,8:48,A 
==> Live_Devs: 2, IO_Count: TRD: 0 ORD: 0 TWR: 0 OWR: 0
% /sbin/dmsetup remove dms

=> NOTE: default policy is round-robin with 8 ios.


Example loading lines for initializing read policies:

1. Round-robin (default policy)

% /sbin/dmsetup create dms --table '0 4405248 mirror_sync round_robin 1 16 2 /dev/sdc 0 /dev/sdd 0'
% /sbin/dmsetup status dms
0 4405248 mirror_sync 2 RR,ios=16 0,8:32,A 1,8:48,A 
==> Live_Devs: 2, IO_Count: TRD: 0 ORD: 0 TWR: 0 OWR: 0
% /sbin/dmsetup remove dms

2. Logical Partitioning with specified chunk size (KB)

% /sbin/dmsetup create dms --table '0 4405248 mirror_sync logical_part 1 256 2 /dev/sdc 0 /dev/sdd 0'
% /sbin/dmsetup status dms
0 4405248 mirror_sync 2 LP,c=256kb 0,8:32,A 1,8:48,A 
==> Live_Devs: 2, IO_Count: TRD: 0 ORD: 0 TWR: 0 OWR: 0
% /sbin/dmsetup remove dms

% /sbin/dmsetup create dms --table '0 4405248 mirror_sync logical_part 1 4096 2 /dev/sdc 0 /dev/sdd 0'
% /sbin/dmsetup status dms
0 4405248 mirror_sync 2 LP,c=4096kb 0,8:32,A 1,8:48,A 
==> Live_Devs: 2, IO_Count: TRD: 0 ORD: 0 TWR: 0 OWR: 0
% /sbin/dmsetup remove dms


3. Weighted with specified weights and max weight size on one device

% /sbin/dmsetup create dms --table '0 4405248 mirror_sync weighted 3 10 0 100 2 /dev/sdc 0 /dev/sdd 0'
% /sbin/dmsetup status dms
0 4405248 mirror_sync 2 CW,wml=0,w[0]=100,w[1]=10 0,8:32,A 1,8:48,A 
==> Live_Devs: 2, IO_Count: TRD: 0 ORD: 0 TWR: 0 OWR: 0
% /sbin/dmsetup remove dms

Check out the scripts for more info and examples on loading / unloading the driver and tweaking read balancing policies on the fly.

