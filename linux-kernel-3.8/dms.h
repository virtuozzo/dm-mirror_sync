/**
 * Device mapper synchronous mirroring driver header file.
 *
 * Copyright (C) 2012-2016 OnApp Ltd.
 *
 * Author: Michail Flouris <michail.flouris@onapp.com>
 *
 * This file is part of the device mapper synchronous mirror module.
 * 
 * The dm-mirror_sync driver is free software: you can redistribute 
 * it and/or modify it under the terms of the GNU General Public 
 * License as published by the Free Software Foundation, either 
 * version 2 of the License, or (at your option) any later version.
 * 
 * Some open source application is distributed in the hope that it will 
 * be useful, but WITHOUT ANY WARRANTY; without even the implied warranty 
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with Foobar.  If not, see <http://www.gnu.org/licenses/>.
 */

/* --------------------------------------------------------------
 *   CONFIGURABLE OPTIONS
 * -------------------------------------------------------------- */

/* define only for debugging... */
#undef ABORT_IO_ON_FIRST_ERROR

/* --------------------------------------------------------------
 *   NON-CONFIGURABLE OPTIONS - FRAGILE !
 * -------------------------------------------------------------- */

#ifndef DEBUGMSG
//#define DEBUGMSG	/* CAUTION: enables VERBOSE debugging messages, decreases performance */
#undef DEBUGMSG
#endif
#ifndef ASSERTS
#define ASSERTS		/* enables assertions, may decrease performance a little */
#endif

/* shortcut for kernel printing... */
#define kprint(x...) printk( KERN_ALERT x )
#define NOOP	do {} while (0)

#ifdef DEBUGMSG
#define DMSDEBUG(x...) printk( KERN_ALERT x )
#define DMSDEBUG_CALL(x...) printk( KERN_ALERT x )
#else
#define DMSDEBUG(x...)	NOOP /* disabled */
#define DMSDEBUG_CALL(x...) NOOP
#endif

/* CAUTION: use for ultra-targeted debugging or ultra-verbosity */
#define DMSDEBUGX(x...) NOOP
//#define DMSDEBUGX(x...) printk( KERN_ALERT x )

/* CAUTION: assert() and assert_bug() MUST BE USED ONLY FOR DEBUGGING CHECKS !! */
#ifdef ASSERTS
#define assert(x) if (unlikely(!(x))) { printk( KERN_ALERT "ASSERT: %s failed @ %s(): line %d\n", \
						#x, __FUNCTION__,__LINE__); }
/*#define assert(x) if (unlikely(!(x))) { printk( KERN_ALERT "ASSERT: %s failed at %40s::%d @ %s()\n", \
						#x, __FILE__,__LINE__, __FUNCTION__); } */

#define assert_return(x,r) if (unlikely(!(x))) { \
        printk( KERN_ALERT "RETURN ASSERT: %s failed @ %s(): line %d\n", #x, __FUNCTION__,__LINE__); \
        return r; }

/* CAUTION: This is a show-stopper... use carefully!! */
#define assert_bug(x) if (unlikely(!(x))) { \
        printk( KERN_ALERT "$$$ BUG ASSERT: %s failed @ %s(): line %d\n", #x, __FUNCTION__,__LINE__); \
        * ((char *) 0) = 0; }
//==============================================
#else
/* CAUTION: disabling ALL assertions... */
#define assert(x)		NOOP
#define assert_bug(x)	NOOP
//==============================================
#endif

/* DEBUG DEFS - DO NOT TOUCH UNLESS YOU'RE LOOKING FOR TROUBLE! */
#undef ALWAYS_SEND_TO_ALL_MIRRORS
#undef ABORT_IO_ON_FIRST_ERROR
#undef DEBUG_WRITE_TO_SINGLE_MIRROR

#undef DISABLE_UNPLUGS /* enable only for debugging... */

#define ENABLE_CHECK_MIRROR_CMDS /* enables check_mirror data support */

#define MAX_MIRRORS	8

#define MAX_DMS_INSTANCES 2048

#define MAX_ERR_MESSAGES 20

/*-----------------------------------------------------------------
 * Mirror set structures.
 *---------------------------------------------------------------*/
struct mirror_sync_set;

/* IDs of the implemented read policies */
typedef enum _dms_read_policy {
	DMS_ROUND_ROBIN,
	DMS_CUSTOM_WEIGHTED,
	DMS_LOGICAL_PARTITION
} dms_read_policy;

enum dm_raid1_error {
	DM_RAID1_WRITE_ERROR,
	DM_RAID1_SYNC_ERROR,
	DM_RAID1_READ_ERROR
};

struct mirror {
	atomic_t error_count;  /* Error counter to flag mirror failure */
	volatile unsigned long error_type;
	struct mirror_sync_set *ms;
	struct dm_dev *dev;
	sector_t offset;
};

#define DEVNAME_MAXLEN 16

struct mirror_sync_set {
	struct dm_target *ti;

	spinlock_t lock;	/* protects the lists and mirror_sync_set config */
	struct bio_list read_failures;

	struct dm_io_client *io_client;

	atomic_t suspend; /* flag set for suspend... */

	struct mirror *default_mirror;	/* Default mirror */

	unsigned int nr_mirrors;		/* number of mirrors */

	/* Read balancing policy fields
	 * Policies supported: 1. Round robin 2. Logical partitioning */
	spinlock_t choose_lock; /* need this lock because choose_mirror() can be called from callbacks */
	atomic_t rdpolicy;		/* ID of the current policy... */
	atomic_t lp_io_chunk;	/* Adjustable io chunk size in KBytes [for logical partitioning scheme]. */
	atomic_t rr_ios_set;	/* Adjustable default ios [for round-robin scheme]. */
	atomic_t rr_ios;		/* Current read ios counter [for round-robin scheme]. */
	struct mirror *read_mirror; /* Last mirror read [for round-robin scheme]. */
	atomic_t mirror_weights[MAX_MIRRORS];	/* Adjustable mirror weights [for custom weighted scheme]. */
	atomic_t mirror_weight_max_live;		/* Current live mirror with max weight [for custom weighted scheme]. */

	struct workqueue_struct *kmirror_syncd_wq;
	struct work_struct kmirror_syncd_work;

	atomic_t supress_err_messages;		/* Counter/flag of printing I/O error messages. */

	/* Total & Outstanding I/O counters */
	atomic_t read_ios_total;
	atomic_t read_ios_pending;
	atomic_t write_ios_total;
	atomic_t write_ios_pending;

	unsigned long timer_pending;

	struct work_struct trigger_event;	/* to trigger event work queue */

	unsigned long reconfig_idx;	/* index in reconfig space for suspend/resume parameter passing */

	unsigned errmsg_last_time;	/* time store for suppressing error messages... */

	char name[ DEVNAME_MAXLEN ];

	struct mirror mirror[0];	/* CAUTION: this field MUST BE the LAST ONE in this struct! */
};

static struct kmem_cache *_dms_mirror_sync_record_cache;

struct dms_bio_map_info {
	struct mirror *bmi_m;
	struct mirror_sync_set *bmi_ms;
	void * bi_private;
	unsigned int nr_live;
	struct mirror *bmi_wm[MAX_MIRRORS];
	struct dm_bio_details bmi_bd;
};

#if 0
static spinlock_t dms_pool_lock;	/* protects the mempool from side-effects :) */
static mempool_t *dms_bio_map_info_pool = NULL;
#endif

/* we need a way to pass parameters over to the new ms at reconfig time */
struct reconfig_ms_set {
	atomic_t in_use;
	struct mirror_sync_set *current_ms;
	char devname[ DEVNAME_MAXLEN ];
} __attribute__((packed));

/* Current instances of mirror_sync devices... */
int curr_ms_instances = MAX_DMS_INSTANCES;

static struct reconfig_ms_set *reconf_ms = NULL;

