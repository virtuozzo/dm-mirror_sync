/**
 * Device mapper synchronous mirroring driver code.
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

/* Include some original dm header files */
#include "dm.h"
#include "dm-bio-record.h"

#include <linux/version.h>
#include <linux/types.h>
#include <linux/ctype.h>
#include <linux/init.h>
#include <linux/mempool.h>
#include <linux/module.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/delay.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include <linux/dm-io.h>
#include <linux/dm-dirty-log.h>
#include <linux/dm-kcopyd.h>

#include "dms.h"			/* Local mirror_sync header file */

#define DM_MSG_PREFIX "mirror_sync"


void mirror_sync_emit_status(struct mirror_sync_set *ms, char *result, unsigned int maxlen);

/* All mirrors are equal, but this is used in some cases (inherited from the mirror module */
#define DEFAULT_MIRROR 0

/*----------------------------------------------------------------------------------
 * NOTE: modified bio_get_m() and bio_set_m() functions to provide bmi pointers!
 * 
 * -> this code continues to be 'yucky' and uses the bi_next field...
 *
 * CAUTION: do NOT touch the bio->bi_private! the dm code uses it for clone_bio() !
 *
 * CAUTION: we are indeed using the bi_next fields which could be DANGEROUS if the
 *          same bio is sent to the lower layer... BUT dm creates new bios for sending
 *          to lower layers, so we are safe...
 */

static struct dms_bio_map_info *bio_get_m(struct bio *bio)
{
	return (struct dms_bio_map_info *) bio->bi_next;
}

static void bio_set_m(struct bio *bio, struct dms_bio_map_info *bmi)
{
	bio->bi_next = (void *) bmi;
}

/*----------------------------------------------------------------------------------
 * CAUTION: these functions use the bi_private pointer, which can only be used for queueing bios
 * 			to handle read failures, not for pointer passing via dm_io()... */
static void bio_push_m_priv(struct bio *bio, struct dms_bio_map_info *bmi)
{
	bmi->bi_private = bio->bi_private;
	bio->bi_private = (void *) bmi;
}

static struct dms_bio_map_info *bio_pop_m_priv(struct bio *bio)
{
	struct dms_bio_map_info *bmi = bio->bi_private;

	assert_bug( bmi );
	bio->bi_private = bmi->bi_private;

	return bmi;
}

/*---------------------------------------------------------------------------------- */

static void wake(struct mirror_sync_set *ms)
{
	queue_work(ms->kmirror_syncd_wq, &ms->kmirror_syncd_work);
}


/*----------------------------------------------------------------- */

static int
mirror_is_alive( struct mirror *m )
{
	if ( test_bit(DM_RAID1_WRITE_ERROR, &(m->error_type)) ||
		 test_bit(DM_RAID1_SYNC_ERROR, &(m->error_type)) ||
		 test_bit(DM_RAID1_READ_ERROR, &(m->error_type)) ||
		 atomic_read(&m->error_count) )
		return 0; /* dead */
	else
		return 1; /* alive ! */
}

/*---------------------------------------------------------------------------------- */

/* Returns the LIVE mirror with the maximum weight in the set... */

struct mirror *get_mirror_weight_max_live( struct mirror_sync_set *ms )
{
	int i, maxi = -1, max = -1;
	struct mirror *mirr = NULL;

	assert_bug( ms->nr_mirrors >= 0 );
	assert_bug( ms->nr_mirrors <= MAX_MIRRORS );
	for (i = 0; i < ms->nr_mirrors; i++) {
		mirr = ms->mirror + i;
		if ( atomic_read( &ms->mirror_weights[i] ) > max &&
				mirror_is_alive(mirr)) { /* alive? */
			
			max = atomic_read( &ms->mirror_weights[i] );
			maxi = i;
		}
	}
	if (unlikely(maxi < 0))
		return NULL;

	assert_bug( maxi >= 0 && maxi < MAX_MIRRORS && maxi < ms->nr_mirrors );
	atomic_set( &ms->mirror_weight_max_live, maxi );
	return ms->mirror + maxi;
}

/*-----------------------------------------------------------------
 * Reads
 *---------------------------------------------------------------*/
/* Switch to next dev, via round-robin, after MIN_READS reads */
//#define MIN_READS 128
#define MIN_READS 8

/* choose_read_mirror
 * @ms: the mirror set
 * @sector: logical sector no for read
 *
 * This function is used for read balancing according to the policy set AND/OR
 * for redirecting reads to working mirrors on failures.
 *
 * Returns: chosen LIVE mirror, or NULL on failure of all mirrors
 */
static struct mirror *choose_read_mirror(struct mirror_sync_set *ms, sector_t sector)
{
	struct mirror *start_mirror, *curr_mirror, *ret =  ms->default_mirror;

	switch( atomic_read( &ms->rdpolicy ) ) {
		
	case DMS_LOGICAL_PARTITION:
	/* -------------------------------------------------*/
	{
		int lic = atomic_read( &ms->lp_io_chunk ) * 2; /* read stripe chunk in KBytes -> sectors */
		int mm;

		assert( lic > 0 && !(lic % 8) );

		mm = (sector / (long long) lic) % ms->nr_mirrors;

		DMSDEBUG("sector: %lld - lic: %d -> mirror: %d\n", (long long) sector, lic/2, mm );

		ret = ms->mirror + mm; /* get the mirror index */

		/* check if mirror has errors & deal with it... */
		if (unlikely(!mirror_is_alive(ret))) {
		
			/* NOTE: on error, we switch to next-available-live mirror policy */
			curr_mirror = start_mirror = ms->mirror + mm;
			do {
				if (likely(mirror_is_alive(ret)))
					break;
	
				if (curr_mirror-- == ms->mirror)
					curr_mirror += ms->nr_mirrors;

				ret = curr_mirror;
			} while (ret != start_mirror);

			/*
			 * We've rejected every mirror.
			 * Confirm the default_mirror can be used.
			 */
			if (!mirror_is_alive(ret))
			      ret = NULL;
		}
	}
	/* -------------------------------------------------*/
	break;
	case DMS_ROUND_ROBIN:
	/* -------------------------------------------------*/
		/* Can get called in interrupt from mirror_sync_end_io(). */
		spin_lock(&ms->choose_lock);

		/*
		 * Perform MIN_READS on each working mirror then
		 * advance to the next one.  start_mirror stores
		 * the first we tried, so we know when we're done.
		 */
		ret = start_mirror = ms->read_mirror;
		do {
			if ( mirror_is_alive(ret) &&
				   !atomic_dec_and_test(&ms->rr_ios))
				goto use_mirror;

			/* NOTE: on error, we switch to next-available-live mirror policy */
			atomic_set(&ms->rr_ios, atomic_read(&ms->rr_ios_set));

			if (ms->read_mirror-- == ms->mirror)
				ms->read_mirror += ms->nr_mirrors;

			ret = ms->read_mirror;
		} while (ret != start_mirror);

		/*
		 * FAILURE: We've rejected every mirror due to failures.
		 * Confirm the start_mirror can be used.
		 */
		if (!mirror_is_alive(ret))
			ret = NULL;

use_mirror:
		spin_unlock(&ms->choose_lock);
	/* -------------------------------------------------*/
	break;

	case DMS_CUSTOM_WEIGHTED:
	/* -------------------------------------------------*/
	{
		int maxi = atomic_read( &ms->mirror_weight_max_live );

		assert_bug( ms->nr_mirrors < MAX_MIRRORS );
		assert_bug( maxi >= 0 && maxi < ms->nr_mirrors );

		/* get the pointer to the mirror with the current max weight => changes on failure/reconfig*/
		ret = ms->mirror + maxi;

		/* check if mirror has errors & deal with it... */
		if (!mirror_is_alive(ret)) {

			curr_mirror = start_mirror = get_mirror_weight_max_live(ms);
			if ( ! curr_mirror ) /* no live mirror found ! */
				ret = NULL;
				break;

			ret = curr_mirror;
			do {
				if (mirror_is_alive(ret))
					break;

				curr_mirror = get_mirror_weight_max_live(ms); /* re-calc */

				ret = curr_mirror;
			} while (ret != start_mirror);

			/*
			 * We've rejected every mirror.
			 * Confirm the default_mirror can be used.
			 */
			if (unlikely(!mirror_is_alive(ret)))
			      ret = NULL;
		}
	}
	/* -------------------------------------------------*/
	break;
	}

	return ret;
}
/*----------------------------------------------------------------- */

static struct mirror *get_valid_mirror(struct mirror_sync_set *ms)
{
	struct mirror *m;

	for (m = ms->mirror; m < ms->mirror + ms->nr_mirrors; m++)
		if ( mirror_is_alive(m) )
			return m;

	return NULL;
}

/*----------------------------------------------------------------- */

/* fail_mirror
 * @m: mirror device to fail
 * @error_type: one of the enum's, DM_RAID1_*_ERROR
 *
 * If errors are being handled, record the type of
 * error encountered for this device.  If this type
 * of error has already been recorded, we can return;
 * otherwise, we must signal userspace by triggering
 * an event. Additionally, if the device is the primary device,
 * we should choose a new primary.
 *
 * This function cannot block.
 */
#define DMS_MAX_ERRORS	2

static void fail_mirror(struct mirror *m, enum dm_raid1_error error_type)
{
	struct mirror_sync_set *ms = m->ms;
	struct mirror *new;

	/* error bit already set? */
	if (test_and_set_bit(error_type, &m->error_type))
		return;

	/* raise all failure flags for device... */
	set_bit( DM_RAID1_WRITE_ERROR, &m->error_type);
	set_bit( DM_RAID1_SYNC_ERROR, &m->error_type);
	set_bit( DM_RAID1_READ_ERROR, &m->error_type);

	if ( atomic_read(&m->error_count) < DMS_MAX_ERRORS ) {
		char b[BDEVNAME_SIZE];
		atomic_inc(&m->error_count);
		DMWARN("[%s] Mirror device %s (%s) is now OFFLINE!", ms->name,
				m->dev->name, bdevname(m->dev->bdev, b));
	}

	/*
	 * If the default mirror fails, change it.
	 */
	if (m == ms->default_mirror) {

		new = get_valid_mirror(ms);
		if (new)
			ms->default_mirror = new;
		else {
			unsigned int maxlen = 256;
			char buf[ maxlen ];

	  		DMWARN("[%s] All mirror devices have failed!", ms->name);

			memset( buf, 0, maxlen);
			mirror_sync_emit_status(ms, buf, maxlen);
	  		DMWARN("[%s] Mirror Info: %s", ms->name, buf);
		}
	}

	schedule_work(&ms->trigger_event);
}

/*----------------------------------------------------------------- */

static int mirror_sync_available(struct mirror_sync_set *ms)
{
	/* Returns 1 if a live mirror is available... or 0 if all are down... */
	return get_valid_mirror(ms) ? 1 : 0;
}

/*----------------------------------------------------------------- */

/*
 * remap a buffer to a particular mirror.
 */
static sector_t map_sector(struct mirror *m, struct bio *bio)
{
	if (unlikely(!bio->bi_iter.bi_size))
		return 0;
	return m->offset + dm_target_offset(m->ms->ti, bio->bi_iter.bi_sector);
}

static void map_bio(struct mirror *m, struct bio *bio)
{
	assert_bug( m ); // major bug trap...
	bio->bi_bdev = m->dev->bdev;
	bio->bi_iter.bi_sector = map_sector(m, bio);
}

static void map_region(struct dm_io_region *io, struct mirror *m,
		       struct bio *bio)
{
	assert_bug( m ); // major bug trap...
	io->bdev = m->dev->bdev;
	io->sector = map_sector(m, bio);
	io->count = bio_sectors(bio); // change in 3.10
}


static void trigger_event(struct work_struct *work)
{
	struct mirror_sync_set *ms =  container_of(work, struct mirror_sync_set, trigger_event);

	dm_table_event(ms->ti->table);
}

/*-----------------------------------------------------------------
 *  I/O handler functions (reads/writes/etc.)
 *---------------------------------------------------------------*/

/*----------------------------------------------------------------- */
#ifdef DEBUG_WRITE_TO_SINGLE_MIRROR
// supports only a SINGLE write case... debug leftover...
static void dispatch_bio( struct dms_bio_map_info *bmi, struct bio *bio, int rw )
{
	if (rw == WRITE) {
		DMSDEBUG("dispatch_bio() WRITE enter...\n");
		// FIXME: this goes to the default mirror for now...
		map_bio( bmi->bmi_m, bio);
		generic_make_request(bio);
		//DMSDEBUG("dispatch_bio() WRITE exit...\n");
		return;
	}

	/* All about the reads now */
	DMSDEBUG("dispatch_bio() READ enter...\n");
	read_async_bio( bmi, bio);
	DMSDEBUG("dispatch_bio() READ exit...\n");
}
#endif

/* ----------------------------------------------------------------
 * Queue bio function -> queues failed bios for retry in thread...
 */

static void queue_bio(struct mirror_sync_set *ms, struct bio *bio, int rw)
{
	unsigned long flags;
	int should_wake = 0;
	struct bio_list *bl;

	bl = &ms->read_failures;
	spin_lock_irqsave(&ms->lock, flags);
	should_wake = !(bl->head);
	bio_list_add(bl, bio);
	spin_unlock_irqrestore(&ms->lock, flags);

	if (should_wake)
		wake(ms);
}

/*-----------------------------------------------------------------
 * CAUTION: AFTER ALL async I/O we MUST call unplug! Else the I/O will not
 *          proceed at the speed of the timeout (3ms) per call...
 *          -> WARNING: unplug code changed in kernel v.3.7-3.8 onwards... */

static void write_callback(unsigned long error, void *context)
{
	struct bio *bio = (struct bio *) context;
	struct dms_bio_map_info *bmi = NULL;
	struct mirror_sync_set *ms;
	int ret = 0;

	DMSDEBUG("write_callback() enter...\n");

	bmi = bio_get_m(bio);
	assert( bmi ); /* bug trap... */
	ms = bmi->bmi_ms;

	if (unlikely(error)) {

		unsigned int i, nr_live, nr_failed = 0;

		/*
		 * If the bio is discard, return an error, but do not
		 * degrade the array.
		 */
		if (bio->bi_rw & REQ_DISCARD) {
			bio_set_m(bio, NULL);
			bio_endio(bio, -EOPNOTSUPP);
			return;
		}

		/* NOTE: there can be one or more errors, and they are returned in the "error" bitmap!
		 *       -> check the bits to see which mirror failed!
		 *
		 * CAUTION: extra tricky detail: in the returned error bitmap we must EXCLUDE the
		 * mirrors that are dead! so do not include dead ones in the test_bit() ! */
		nr_live = bmi->nr_live;
		assert( nr_live > 0 && nr_live <= ms->nr_mirrors );

		for (i = 0; i < nr_live; i++)
			if ( test_bit(i, &error)) {
				/* ATTENTION: on error, the event to user-space for the failure
				 * will be triggered by fail_mirror()! */
				DMSDEBUG("write_callback() MIRROR %d of %d LIVE FAILED...\n", i, nr_live );
				fail_mirror( bmi->bmi_wm[i], DM_RAID1_WRITE_ERROR);
				nr_failed++;
			}

		/* did anyone survive? */
		ret = (nr_live - nr_failed) ? 0 : -EIO;

		if ( ret != 0 && atomic_read( &ms->supress_err_messages ) < MAX_ERR_MESSAGES ) {
			DMERR("[%s] All mirror devices dead, failing I/O write", ms->name);
			atomic_inc( &ms->supress_err_messages );
		}
	}

	bio_set_m(bio, NULL);
	bio_endio(bio, ret);
	DMSDEBUG("write_callback() after endbio()... exiting\n");
}

/*----------------------------------------------------------------- */

/* Low-level write issuer to ALL live mirrors... Does not deal with error handling
 * here, the caller should have set the proper dm_per_bio_data for retries & faults ... */

static int write_async_bios( struct dms_bio_map_info *bmi, struct bio *bio)
{
	unsigned int i, nr_live = 0;
	struct mirror *m;
	struct mirror_sync_set *ms = bmi->bmi_ms;
	struct dm_io_region io[ms->nr_mirrors], *dest = io;
	struct dm_io_request io_req = {
		.bi_rw = WRITE | (bio->bi_rw & WRITE_FLUSH_FUA),
		.mem.type = DM_IO_BIO,
		.mem.ptr.bio = bio,
		.notify.fn = write_callback,
		.notify.context = bio,
		.client = ms->io_client,
	};

	assert_bug(bmi);
	if (bio->bi_rw & REQ_DISCARD) {
		io_req.bi_rw |= REQ_DISCARD;
		io_req.mem.type = DM_IO_KMEM;
		io_req.mem.ptr.addr = NULL;
	}

#ifdef ALWAYS_SEND_TO_ALL_MIRRORS // DEBUG ONLY !
	/* ------------------------------------------
	 * SENDING TO ALL MIRRORS, EVEN FAULTY ONES! */
	for (i = 0, m = ms->mirror; i < ms->nr_mirrors; i++, m++) {
		map_region(dest++, m, bio);
		bmi->bmi_wm[i] = m;
	}
	bmi->nr_live = ms->nr_mirrors;
#else
	/* ------------------------------------------
	 * SENDING TO ALL *LIVE* MIRRORS! */

	for (i = 0, m = ms->mirror; i < ms->nr_mirrors; i++, m++) {

		//sprintf( lvn[i], "%s", bdevname(m->dev->bdev, b) );

		if ( likely(mirror_is_alive(m)) ) {

			//lv[i] = 1;

			map_region(dest++, m, bio);

			bmi->bmi_wm[nr_live] = m;
			nr_live++;

		}
	}
	/*DMSDEBUGX("DMS REQ [2]: WR Addr: %lld Size: %d - LIVE: %d - %d-%d-%d %s-%s-%s\n",
				(unsigned long long)bio->bi_iter.bi_sector << 9, bio->bi_iter.bi_size, nr_live,
				lv[0], lv[1], lv[2], lvn[0], lvn[1], lvn[2] ); */

	if ( ! nr_live )
		return 0; /* all mirrors dead ! */

	bmi->nr_live = nr_live;
#endif

	/*
	 * We must store reference info to the operation
	 * and the mirror set for use in write_callback().
	 */
	bio_set_m(bio, bmi);

#ifndef DISABLE_UNPLUGS // Linux-3.8 specific
	{
	struct blk_plug plug;

	blk_start_plug(&plug);
#endif

#ifdef ALWAYS_SEND_TO_ALL_MIRRORS // DEBUG ONLY !
	BUG_ON(dm_io(&io_req, ms->nr_mirrors, io, NULL));
#else
	BUG_ON(dm_io(&io_req, nr_live, io, NULL));
#endif

#ifndef DISABLE_UNPLUGS // Linux-3.8 specific
	blk_finish_plug(&plug); /* ESSENTIAL for speed... */
	}
#endif

	DMSDEBUG("write_async_bios (2) call...\n");

	return 1;
}

/*----------------------------------------------------------------- */

/* Async callback for the reads... */
static void read_callback(unsigned long error, void *context)
{
	struct bio *bio = (struct bio *)context;
	struct dms_bio_map_info *bmi = NULL;
	struct mirror *m;
	int ret = 0;

	bmi = bio_get_m(bio);
	assert( bmi ); /* bug trap... */
	m = bmi->bmi_m;

	DMSDEBUG("read_callback() enter (Dev: %s)...\n", m->dev->name);

	if (unlikely(error)) { /* READ ERROR HANDLING! */

		if ((error == -EOPNOTSUPP) ||
			((error == -EWOULDBLOCK) && (bio->bi_rw & REQ_RAHEAD)) ) {

			DMERR("[%s] Mirror device %s: failing I/O Read (Error: %ld)",
						m->ms->name, m->dev->name, error );
			ret = error;
			goto out;
		}

		DMWARN("[%s] Mirror device %s: Read I/O failure [Addr: %lld Size: %d] ...handling it",
				m->ms->name, m->dev->name, (unsigned long long)bio->bi_iter.bi_sector << 9, bio->bi_iter.bi_size);

		/* ATTENTION: the event to user-space for the failure
		 * will be triggered by fail_mirror()! */
		fail_mirror(m, DM_RAID1_READ_ERROR);

		/* Is there another mirror available? (i.e. live) */
		if ( likely(mirror_sync_available(m->ms)) ) {

#ifdef ABORT_IO_ON_FIRST_ERROR
			/* -------------------------------------------------------
			 * this is debug code to fail all IO on first failure... */
			DMERR("[%s] Read on device failed... NOT trying different device, aborting!", m->ms->name);
			bio_set_m(bio, NULL);
			bio_endio(bio, -EIO);
#else
			/* -------------------------------------------------------
			 * ...and here we try to deal with failures by using a live mirror */
			struct dm_bio_details *bd = &bmi->bmi_bd;

			DMWARN("[%s] Read failure [Addr: %lld Size: %d] - Trying different mirror",
					m->ms->name, (unsigned long long)bio->bi_iter.bi_sector << 9, bio->bi_iter.bi_size);
			dm_bio_restore(bd, bio);

			/* ATTENTION: we MUST keep the pointer live in bio, but we CANNOT use bio_set_m(bio, NULL); !
			 *            -> so call the special bio_push_m_priv(bio, bmi); to push/pop the bi_private */
			bio_push_m_priv(bio, bmi);
			
			DMSDEBUG("read_callback (Dev: %s): queueing read IO on thread!\n", m->dev->name );
			queue_bio(m->ms, bio, bio_rw(bio));
			return;

#endif
		} else {

			/* NO LIVE MIRROR FOUND!! */
			if ( atomic_read( &m->ms->supress_err_messages ) < MAX_ERR_MESSAGES ) {
				DMERR("[%s] READ_CB: All mirror devices dead, failing I/O read", m->ms->name);
				atomic_inc( &m->ms->supress_err_messages );
			}

			ret = -EIO;
		}
	}

out:
	bio_set_m(bio, NULL);
	bio_endio(bio, ret);
	DMSDEBUG("read_callback (Dev: %s): exiting, bio_endio() done!\n", m->dev->name);
}

/*----------------------------------------------------------------- */

/* Asynchronous read I/O call. */
static void read_async_bio(struct dms_bio_map_info *bmi, struct bio *bio)
{
	struct dm_io_region io;
	struct mirror *m = bmi->bmi_m;
	struct dm_io_request io_req = {
		.bi_rw = READ,
		.mem.type = DM_IO_BIO,
		.mem.ptr.bio = bio,
		.notify.fn = read_callback,
		.notify.context = bio,
		.client = m->ms->io_client,
	};

	assert_bug(bmi);
	map_region(&io, m, bio);
	bio_set_m(bio, bmi);

#ifdef DISABLE_UNPLUGS // Linux-3.8 specific
	BUG_ON(dm_io(&io_req, 1, &io, NULL));
#else
	{
		struct blk_plug plug;

		blk_start_plug(&plug);
		BUG_ON(dm_io(&io_req, 1, &io, NULL));
		blk_finish_plug(&plug); /* ESSENTIAL for speed... */
	}
#endif
}

/* ----------------------------------------------------------------
 * Mirror mapping function -> All the I/O action goes through here!
 */
static int mirror_sync_map(struct dm_target *ti, struct bio *bio)
{
	int rw = bio_rw(bio);
	struct mirror *m;
	struct mirror_sync_set *ms = ti->private;
	struct dms_bio_map_info *bmi = dm_per_bio_data(bio, sizeof(struct dms_bio_map_info));
	struct dm_bio_details *bd = NULL;
#ifdef DEBUGMSG
	struct mapped_device *md;

	/* NOTE: this is only for debugging... */
	md = dm_table_get_md(ti->table);
	DMSDEBUG("mirror_sync_map() enter (Dev: %s)...\n", dm_device_name(md));
#endif
	if (rw == READA) // read-ahead...
		return -EWOULDBLOCK;

	if (likely(bmi)) {
		/* without this, an I/O operation is not recoverable by sending to a different mirror */
		bd = &bmi->bmi_bd;
		bd->bi_bdev = NULL;
		dm_bio_record(bd, bio);
		bmi->bmi_m = ms->default_mirror; /* use default by default ;) */
		bmi->bmi_ms = ms;
	} else {
		/* Cannot happen, since dms_bio_map_info_pool_alloc() waits until memory is available... */
		DMSDEBUG("BUG!! mirror_sync_map could NOT allocate bmi!!\n");
		panic("dms: could not allocate mem");
	}

	/* Handling writes... fwd them and get a callback at mirror_sync_end_io() */
	if (rw == WRITE) {
#ifdef DEBUGMSG
		//DMSDEBUG("mirror_sync_map WRITE call...\n");
		DMSDEBUG("[%s] DMS REQ: WRITE Addr: %lld Size: %d\n", dm_device_name(md),
		   				(unsigned long long)bio->bi_iter.bi_sector << 9, bio->bi_iter.bi_size);
#endif

		atomic_inc( &ms->write_ios_total );

#ifdef DEBUG_WRITE_TO_SINGLE_MIRROR
		/* INFO: the dispatch_bio writes to ONE mirror only... [DEBUG ONLY] */
		m = ms->default_mirror;
		bmi->bmi_ms = ms;
		bmi->bmi_m = ms->default_mirror;
		dispatch_bio( bmi, bio, rw);
#else
		/* NOTE: we use write_async_bios() to send write to ALL MIRRORS! */
		if ( ! write_async_bios(bmi, bio) )
			goto write_all_dead;
#endif

	   	atomic_inc( &ms->write_ios_pending );

		return 0;
	}

	/* All about the reads now */
#ifdef DEBUGMSG
	//DMSDEBUG("mirror_sync_map READ call...\n");
	DMSDEBUG("[%s] DMS REQ: READ Addr: %lld Size: %d\n", dm_device_name(md),
				(unsigned long long)bio->bi_iter.bi_sector << 9, bio->bi_iter.bi_size);
#endif
	atomic_inc( &ms->read_ios_total );

	/*
	 * Load-balance reads by the chosen policy to improve performance...
	 *
	 * In case they fail, queue them to another live mirror
	 * in the mirror_sync_end_io() function.
	 */
   	atomic_inc( &ms->read_ios_pending );
	m = choose_read_mirror(ms, bio->bi_iter.bi_sector);

	/* A live mirror was found... */
	if (likely(m)) {

#ifdef DEBUGMSG
		char b[BDEVNAME_SIZE];
		DMSDEBUG("[%s] mirror_sync_map READ MIRROR CHOSEN OK Dev: %s (%s)...\n",
					dm_device_name(md), m->dev->name, bdevname(m->dev->bdev, b));
#endif

		/* if we have bmi struct, set the pointer for retries... */
		assert_bug(bmi);
		bmi->bmi_m = m;
		map_bio(m, bio);

		read_async_bio(bmi, bio);

		return 0;

	} else {

	   	atomic_dec( &ms->read_ios_pending );

		/* NO LIVE MIRROR FOUND!! */
write_all_dead:

		if ( atomic_read( &ms->supress_err_messages ) < MAX_ERR_MESSAGES ) {
			DMERR("[%s] All mirror devices dead, failing I/O", ms->name );
			atomic_inc( &ms->supress_err_messages );
		}

		return -EIO;
	}
}

/*----------------------------------------------------------------- */

/* NOTE: the mirror_sync_end_io handler is called after the async
 *       read/write_callback() functions... */

static int mirror_sync_end_io(struct dm_target *ti, struct bio *bio, int error)
{
	struct mirror_sync_set *ms = (struct mirror_sync_set *) ti->private;

	DMSDEBUG_CALL("mirror_sync_end_io called...\n");

	/*
	 * CAUTION: do NOT touch the bio->bi_private! the dm code uses it for clone_bio() !
	 */

	/* Update our pending I/O counters... */
	if ( bio_rw(bio) == WRITE)
	   	atomic_dec( &ms->write_ios_pending );
	else
		atomic_dec( &ms->read_ios_pending );

	/* We don't actually need to do anything else in here because ALL I/Os
	 * must have gone through the read/write_callbacks!!
	 */

	return error;
}

/*----------------------------------------------------------------- */

static void mirror_sync_presuspend(struct dm_target *ti)
{
	struct mirror_sync_set *ms = (struct mirror_sync_set *) ti->private;

	DMSDEBUG_CALL("mirror_sync_presuspend called...\n");
	atomic_set(&ms->suspend, 1);

	assert_bug( ms->reconfig_idx < curr_ms_instances );

	/*
	 * We don't need to finish any recovery work, because that process
	 * is handled offline for us... just need to flush any read retries...
	 */
	flush_workqueue(ms->kmirror_syncd_wq);
}

/*----------------------------------------------------------------- */

static void do_read_failures(struct mirror_sync_set *ms, struct bio_list *read_failures)
{
	int rw;
	struct bio *bio;
	struct mirror *m;
	struct dms_bio_map_info *bmi = NULL;
	struct dm_bio_details *bd = NULL;

	DMSDEBUG_CALL("do_read_failures() ENTERING...\n");

	while ((bio = bio_list_pop(read_failures))) {

		DMSDEBUG("do_read_failures() GOT BIO...\n");
		rw = bio_rw(bio);

		/* NOTE: we re-use the already allocated bmi, that was saved in the bio struct... */
		bmi = bio_pop_m_priv(bio);
		assert( bmi ); /* bug trap... */

		if (likely(bmi)) {
			/* without this, an I/O operation is not recoverable by sending to a different mirror */
			bd = &bmi->bmi_bd;
			assert( bmi->bmi_ms == ms );
		} else {
			/* BUG ALERT: NULL bmi pointer! cannot continue, failing I/O */
			DMERR("[%s] do_read_failures(): NULL bmi pointer, failing I/O read", ms->name);
			bio_endio(bio, -EFAULT);
			continue;
		}

		DMSDEBUG("do_read_failures() READ call...\n");
		assert_bug( rw == READ ); // BUG TRAP: ONLY QUEUEING READS FOR NOW...

		/*
		 * We can ALWAYS retry the read on another device because they are always in sync.
		 */
		m = choose_read_mirror(ms, bio->bi_iter.bi_sector);

		/* CAUTION: shortcuts do not always work... */
		//if (unlikely(m && !mirror_is_alive(m)))
		//	m = NULL;

		/* A live mirror was found... */
		if (likely(m)) {
#ifdef DEBUGMSG
			char b[BDEVNAME_SIZE];
			DMSDEBUG("do_read_failures() found live mirror: %s (%s)...\n", m->dev->name, bdevname(m->dev->bdev, b));
#endif
			assert_bug(bmi);
			bmi->bmi_m = m;
			map_bio(m, bio);
			//DMSDEBUG("do_read_failures() sending read I/O to %s (%s)...\n", m->dev->name, bdevname(m->dev->bdev, b));
			read_async_bio( bmi, bio);

			DMSDEBUG("do_read_failures() sent read I/O to %s (%s)...\n", m->dev->name, bdevname(m->dev->bdev, b));

		} else {

			/* If all mirrors have failed, we give up and forget the bio... */
			DMSDEBUG("do_read_failures() NO LIVE MIRROR FOUND...\n");

			if ( atomic_read( &ms->supress_err_messages ) < MAX_ERR_MESSAGES ) {
				DMERR("[%s] do_read_failures(): All mirror devices dead, failing I/O read", ms->name);
				atomic_inc( &ms->supress_err_messages );
			}

			bio_endio(bio, -EIO);
			DMSDEBUG("do_read_failures(): bio_endio(bio) DONE\n");
		}
	}
}

/*-----------------------------------------------------------------
 * kmirror_syncd
 *
 * => a helper function to allow retrying of read_failures...
 *---------------------------------------------------------------*/
static void main_mirror_syncd(struct work_struct *work)
{
	struct mirror_sync_set *ms =  container_of(work, struct mirror_sync_set, kmirror_syncd_work);
	struct bio_list read_failures;
	unsigned long flags;

	/* atomically get the currently pending list... and then reset the global list... */
	spin_lock_irqsave(&ms->lock, flags);
	read_failures = ms->read_failures;
	bio_list_init(&ms->read_failures);
	spin_unlock_irqrestore(&ms->lock, flags);

	do_read_failures(ms, &read_failures);

	/* No need to unplug here, do_read_failures() has already done it... */
}

/*----------------------------------------------------------------- */

static void mirror_sync_postsuspend(struct dm_target *ti)
{
	struct mirror_sync_set *ms = (struct mirror_sync_set *) ti->private;

	DMSDEBUG_CALL("mirror_sync_postsuspend called...\n");
	assert( atomic_read(&ms->suspend) == 1); // should already be suspended...

	assert_bug( ms->reconfig_idx < curr_ms_instances );
}


static void mirror_sync_resume(struct dm_target *ti)
{
	struct mirror_sync_set *ms = (struct mirror_sync_set *) ti->private;

	/* NOTE: this assertion is wrong, because resume is called also at device init...
	assert( atomic_read(&ms->suspend) == 1);
	 */

	assert_bug( ms->reconfig_idx < curr_ms_instances );

	atomic_set(&ms->suspend, 0); /* lower suspend flag... */

	DMSDEBUG_CALL("mirror_sync_resume called...\n");
}

/*----------------------------------------------------------------- */

#ifdef ENABLE_CHECK_MIRROR_CMDS
static void
bi_complete(struct bio *bio, int error)
{
	complete((struct completion*)bio->bi_private);
}

/* Do a synchronous I/O operation on a block device
 * NOTE: this initiates a new I/O operation here, not one forwarded from higher system layers */

static int
dms_sync_block_io(struct block_device *bdev, unsigned long long baddr_bytes,
							unsigned long bsize, struct page **pages, int rw)
{
	struct bio *bio = bio_alloc(GFP_NOIO, 1);
	struct completion event;
	int i, ret, npages = bsize / PAGE_SIZE;

	if (!bio) {
		DMERR("dms_sync_block_io():: bio_alloc() failed!");
		return 0;
	}

	assert_return((baddr_bytes % 512 == 0 && baddr_bytes % PAGE_SIZE == 0), 0);
	bio->bi_rw = READ | REQ_SYNC;
	bio->bi_vcnt = 0;
	bio->bi_iter.bi_idx = 0;
	bio->bi_phys_segments = 0;
	bio->bi_iter.bi_size = 0;
	bio->bi_bdev = bdev;
	bio->bi_iter.bi_sector = (sector_t) (baddr_bytes >> 9);
	for (i = 0; i < npages; i++) {
		int bap;
		if ( (bap = bio_add_page(bio, pages[i], PAGE_SIZE, 0)) == 0 ) {
			DMERR("dms_sync_block_io():: bio_add_page() failure [i=%d, bap=%d]...", i, bap);
			return 0;
		}
		//DMSDEBUG_CALL("Added page buf: %d page: %p bap: %d!\n", i, pages[i], bap );
	}
	init_completion(&event);
	bio->bi_private = &event;
	bio->bi_end_io = bi_complete;
	generic_make_request(bio);
	wait_for_completion(&event);

	ret = test_bit(BIO_UPTODATE, &bio->bi_flags);
	bio_put(bio);
	return ret;
}

typedef struct _mirror_check_t {
	unsigned int live;
	unsigned int nr_pages;
	struct mirror *m;
	struct page **pagebufs;
} mirror_check_t;

/*----------------------------------------------------------------- */

int
alloc_check_io_buffers(struct mirror_sync_set *ms, mirror_check_t *mc, int bsize)
{
	unsigned int i, j;
	struct mirror *m;

	memset( (char *)mc, 0, ms->nr_mirrors * sizeof(mirror_check_t) );

	/* Allocate page buffers for reading data from live mirrors */
	for (j = 0, m = ms->mirror; j < ms->nr_mirrors; j++, m++) {

		if ( mirror_is_alive(m) ) {

			mc[j].live = 1;
			mc[j].nr_pages = bsize / PAGE_SIZE;
			mc[j].m = m;

			mc[j].pagebufs = kzalloc( mc[j].nr_pages * sizeof(struct page *), GFP_KERNEL);
			if ( !mc[j].pagebufs ) {
				DMERR("Failed to allocate memory for pagebufs! [j=%d] Exiting...", j);
				goto fail_memalloc;
			}	

			for (i = 0; i < mc[j].nr_pages; i++) {
				struct page *page;

				if (!(page = alloc_page(GFP_KERNEL))) {
					DMERR("Failed to allocate memory page for pagebufs! [j=%d] Exiting...", j);
					goto fail_memalloc;
				}
				mc[j].pagebufs[i] = page;
			}

		} else {
			mc[j].live = 0;
		}
	}

	return 1;

fail_memalloc:
	if (j < ms->nr_mirrors) {
		int k;

		for (k = 0, m = ms->mirror; k <= j; k++, m++) {

			if ( mc[k].live && mc[k].pagebufs ) {

				for (i = 0; i < mc[k].nr_pages; i++) {
					if ( mc[k].pagebufs[i] )
						__free_page( mc[k].pagebufs[i] );
					else
						DMERR("page buffer %d,%d was NULL?!", k, i);
				}

				kfree( mc[k].pagebufs );
			}
		}
	}

	return 0;
}


/* NOTE: assuming that all (not part of) buffers have been allocated successfully! */

int
free_check_io_buffers(struct mirror_sync_set *ms, mirror_check_t *mc)
{
	unsigned int i, j;
	struct mirror *m;

	/* Allocate page buffers for reading data from live mirrors */
	for (j = 0, m = ms->mirror; j < ms->nr_mirrors; j++, m++) {

		if ( mc[j].live && mc[j].pagebufs ) {

			for (i = 0; i < mc[j].nr_pages; i++) {
				if ( mc[j].pagebufs[i] )
					__free_page( mc[j].pagebufs[i] );
				else
					DMWARN("[%s] Error: page buffer %d,%d was NULL?!", ms->name, j, i);
				}

				kfree( mc[j].pagebufs );
		}
	}

	return 1;
}

int
compare_check_all_io_buffers(struct mirror_sync_set *ms, mirror_check_t *mc)
{
	unsigned int i, j, nr_live = 0;
	mirror_check_t livemc[ms->nr_mirrors];

	memset( (char *)livemc, 0, ms->nr_mirrors * sizeof(mirror_check_t) );

	/* Get only live mirrors */
	for (j = 0; j < ms->nr_mirrors; j++) {

		if ( mirror_is_alive(mc[j].m) && mc[j].live && mc[j].pagebufs ) {

			livemc[nr_live].live = 1;
			livemc[nr_live].m = mc[j].m;
			livemc[nr_live].nr_pages = mc[j].nr_pages;
			livemc[nr_live].pagebufs = mc[j].pagebufs;
			nr_live++;
		}
	}

	if (nr_live < 2) {
		DMERR("[%s] Found %d live mirrors (less than 2)... cannot compare!",
				ms->name, nr_live);
		return 0;
	}

	/* Compare page buffers filled with data from live mirrors */
	for (j = 0; j < nr_live-1; j++) {

		assert_bug( livemc[j].nr_pages == livemc[j+1].nr_pages );

		for (i = 0; i < livemc[j].nr_pages; i++) {

			char *pg1 = page_address(livemc[j].pagebufs[i]);
			char *pg2 = page_address(livemc[j+1].pagebufs[i]);

			//DMSDEBUG_CALL("Comparing page bufs: %d, pg1: %p pg2: %p !\n", i, pg1, pg2);

			if ( memcmp( pg1, pg2, PAGE_SIZE ) ) {
				char b1[BDEVNAME_SIZE],b2[BDEVNAME_SIZE];

				DMERR("[%s] Different page buffer %d between mirrors %s (%s) and %s (%s) !",
						ms->name, i, livemc[j].m->dev->name, bdevname(livemc[j].m->dev->bdev, b1),
						livemc[j+1].m->dev->name, bdevname(livemc[j+1].m->dev->bdev, b2));
				return 0;
			}
		}
	}

	/* all passed ok */
	return 1;
}

/*----------------------------------------------------------------- */


int
check_all_mirror_data( struct mirror_sync_set *ms, unsigned long long maxlen,
						unsigned bsize, long long *error_baddr, int throttle)
{
	unsigned int i, nr_live = 0, bsize_secs;
	struct mirror *m;
	unsigned long long baddr_bytes, baddr_secs;
	mirror_check_t mc[ms->nr_mirrors];
#ifdef DEBUGMSG
	char b[BDEVNAME_SIZE];
#endif

	DMSDEBUG_CALL("Check_all_mirror_data: ENTER maxlen: %lld\n", maxlen);

	// FIXME: add check for max_bio_sectors... we want single bio
	if ( bsize < PAGE_SIZE || bsize > 256*1024 || bsize % PAGE_SIZE != 0 ||
		bsize % 512 != 0 || (bsize > PAGE_SIZE && bsize / PAGE_SIZE % 2 != 0) ) {
		DMERR("[%s] Invalid block size: must be between 4KiB - 256KiB, aligned to 4KiB",
				ms->name);
		return 0;
	}

	// FIXME: limit bsize to PAGE_SIZE for now... need to fix dms_sync_block_io to
	//        support larger blocks...
	bsize = PAGE_SIZE;
	DMWARN("[%s] Limiting block size to %d (only size supported currently)", ms->name, bsize);

	/* Count live mirrors... we need at least 2 to compare... */
	for (i = 0, m = ms->mirror; i < ms->nr_mirrors; i++, m++)
		nr_live++;

	if (nr_live < 2) {
		DMERR("[%s] Found %d live mirrors (less than 2)... cannot compare!", ms->name, nr_live);
		return 0;
	}

	/* Allocate page buffers for reading data from live mirrors */
	if ( !alloc_check_io_buffers(ms, mc, bsize) )
		return 0;

	// FIXME: scanning just first 131072 sectors for now...
	maxlen = maxlen > 131072 ? 131072 : maxlen;
	bsize_secs = bsize >> 9;
	for (baddr_secs = 0; baddr_secs < maxlen; baddr_secs += bsize_secs) {

		baddr_bytes = baddr_secs * 512;

		for (i = 0, m = ms->mirror; i < ms->nr_mirrors; i++, m++) {

			if ( mirror_is_alive(m) && mc[i].live ) {

				DMSDEBUG_CALL("Reading block %lld from mirror device %s (%s)!\n",
					baddr_secs, m->dev->name, bdevname(m->dev->bdev, b));

				assert_bug( mc[i].pagebufs );

				if ( !dms_sync_block_io( m->dev->bdev, baddr_bytes, bsize, mc[i].pagebufs, READ) ) {
					DMERR("[%s] Mirror Check All: read I/O failure! [block addr: %lld, bsize: %d]",
							ms->name, baddr_bytes, bsize);
					*error_baddr = baddr_secs;
					goto check_data_error;
				}

			} else {

				DMSDEBUG_CALL("Skip checking block %lld from DEAD mirror device %s (%s)!\n",
					baddr_secs, m->dev->name, bdevname(m->dev->bdev, b));
			}
		}

		/* OK, now compare to previously read live buffers... */
		if ( !compare_check_all_io_buffers(ms, mc) ) {
			DMERR("[%s] Mirror Check All: Data inconsistency found at sector addr %lld [bsize: %d]",
					ms->name, baddr_secs, bsize);
			*error_baddr = baddr_secs;
			goto check_data_error;
		}

		if ( baddr_secs && baddr_secs % 4096 == 0 )
			DMINFO("[%s] Mirror Check: Done Checking block %6lld of %lld",
					ms->name, baddr_secs, maxlen );

		if ( throttle )
			schedule();
	}

	if ( !free_check_io_buffers(ms, mc) )
		return 0;

	return 1; /*OK*/

check_data_error:
	free_check_io_buffers(ms, mc);
	return 0;
}

/*----------------------------------------------------------------- */


/* NOTE: baddr_secs and maxlen sizes are in 512 sectors! */
int
check_mirror_data_block( struct mirror_sync_set *ms, unsigned long long maxlen,
						 unsigned long long baddr_secs, unsigned bsize )
{
	unsigned int i, nr_live = 0, bsize_secs;
	struct mirror *m;
	unsigned long long baddr_bytes = baddr_secs * 512;
	mirror_check_t mc[ms->nr_mirrors];
#ifdef DEBUGMSG
	char b[BDEVNAME_SIZE];
#endif

	DMSDEBUG_CALL("Check_mirror_data_block: ENTER maxlen: %lld\n", maxlen);

	// FIXME: add check for max_bio_sectors... we want single bio
	if ( bsize < PAGE_SIZE || bsize > 256*1024 || bsize % PAGE_SIZE != 0 ||
		bsize % 512 != 0 || (bsize > PAGE_SIZE && bsize / PAGE_SIZE % 2 != 0) ) {
		DMERR("[%s] Invalid block size: must be between 4KiB - 256KiB, aligned to 4KiB",
				ms->name );
		return 0;
	}

	// FIXME: limit bsize to PAGE_SIZE for now... need to fix dms_sync_block_io to
	//        support larger blocks...
	bsize = PAGE_SIZE;
	DMWARN("[%s] Limiting block size to %d (only size supported currently)", ms->name, bsize);

	bsize_secs = bsize >> 9;
	if ( baddr_secs && (baddr_secs < PAGE_SIZE || baddr_secs % (PAGE_SIZE/512) != 0 ||
		 baddr_secs > maxlen || baddr_secs + bsize_secs > maxlen) ) {
		DMERR("[%s] Invalid block address: address %lld + %u sectors must be up to dev size: %lld sec",
				ms->name, baddr_secs, bsize_secs, maxlen);
		return 0;
	}

	/* Count live mirrors... we need at least 2 to compare... */
	for (i = 0, m = ms->mirror; i < ms->nr_mirrors; i++, m++)
		nr_live++;

	if (nr_live < 2) {
		DMERR("[%s] Found %d live mirrors (less than 2)... cannot compare!",
				ms->name, nr_live);
		return 0;
	}

	/* Allocate page buffers for reading data from live mirrors */
	if ( !alloc_check_io_buffers(ms, mc, bsize) )
		return 0;
		
	for (i = 0, m = ms->mirror; i < ms->nr_mirrors; i++, m++) {

		if ( mirror_is_alive(m) && mc[i].live ) {

			DMSDEBUG_CALL("Reading block at sec: %lld from mirror dev %s (%s)!\n",
							baddr_secs, m->dev->name, bdevname(m->dev->bdev, b));

			assert_bug( mc[i].pagebufs );

			if ( !dms_sync_block_io( m->dev->bdev, baddr_bytes, bsize, mc[i].pagebufs, READ) ) {
				DMERR("[%s] Mirror Check Block: read I/O failure! [block addr: %lld, bsize: %d]",
						ms->name, baddr_secs, bsize);
				goto check_data_error;
			}

		} else {

			DMSDEBUG_CALL("Skip checking block at sec: %lld from DEAD mirror device %s (%s)!\n",
							baddr_secs, m->dev->name, bdevname(m->dev->bdev, b));
		}
	}

	/* OK, now compare to previously read live buffers... */
	if ( !compare_check_all_io_buffers(ms, mc) ) {
		DMERR("[%s] Mirror Check Block: Inconsistency found for block at sector %lld [bsize: %d]",
				ms->name, baddr_secs, bsize);
		goto check_data_error;
	}

	if ( !free_check_io_buffers(ms, mc) )
		return 0;

	return 1; /*OK*/

check_data_error:
	free_check_io_buffers(ms, mc);
	return 0;
}
#endif

/*----------------------------------------------------------------- */

/* Set read policy & parameters via the message interface. */
static int mirror_sync_message(struct dm_target *ti, unsigned argc, char **argv)
{
	unsigned value;
	unsigned long long llvalue;
	struct mirror_sync_set *ms = ti->private;
	struct mapped_device *md;
	char dummy;

	DMSDEBUG_CALL("mirror_sync_message called...\n");

	/* INFO: valid message forms [ALWAYS 4 args - use 0 for unused values]:
	 * 1. io_balance <policy_name> <policy_param_name> <value>
	 * 2. io_cmd <command_type> <cmd_arg1> <cmd_arg2>
	 *
	 * io_cmd could be:
	 *    1. set_weight <dev number in array> <weight for device>
	 *    2. check_data_mirror_all <data unit> <block size (bytes)>
	 *    3. check_data_mirror_block <block address (sectors)> <block size (bytes)>
	 *
	 * Valid <policy_name> values: round_robin, logical_part, weighted
	 *
	 * Valid policy_param_name values: ios, io_chunk,
	 */
	if (argc != 4 || 
	    ( strncmp(argv[0], "io_balance", strlen(argv[0])) &&
		  strncmp(argv[0], "io_cmd", strlen(argv[0])) ) ) {

		DMERR("[%s] Invalid command or argument number (need 4 args)", ms->name);
		return -EINVAL;
	}

	if ( ms->nr_mirrors <= 0 || ms->nr_mirrors > MAX_MIRRORS ) {
		DMERR("[%s] Invalid number of mirrors configured: %d", ms->name, ms->nr_mirrors );
		return -EINVAL;
	}

	/* handle io_cmd type messages */
	if ( strncmp(argv[0], "io_cmd", strlen(argv[0])) == 0 ) {
		/* -------------------------------------------------------- */
		DMSDEBUG("HANDLE io_cmd message...\n");

		if ( strncmp(argv[1], "set_weight", strlen(argv[1])) == 0 ) {
			/* ---------------------------------------------------- */
			int i, devno = -1, maxi = -1, max = -1;
			struct mirror *mirr = NULL;

			DMSDEBUG("HANDLE io_cmd set_weight message...\n");
			assert_bug( ms->nr_mirrors > 0 );
			assert_bug( ms->nr_mirrors <= MAX_MIRRORS );

			if (sscanf(argv[2], "%u%c", &devno, &dummy) != 1 || devno < 0 ||
						devno >= ms->nr_mirrors) {
				DMERR("[%s] Invalid device number (arg 3): has to between 0 - %d",
						ms->name, ms->nr_mirrors );
				return -EINVAL;
			}
			if (sscanf(argv[3], "%u%c", &value, &dummy) != 1 || value < 1 || value > 100 ) {
				DMERR("[%s] Invalid device weights: must be between 1 - 100", ms->name);
				return -EINVAL;
			}

			/* CAUTION: dm_table_get_md() code has changed since 2.6.18! no dm_put() neede after it! */
			md = dm_table_get_md(ti->table);
			DMINFO("[%s] Setting weight of device %d in \"%s\" to %u",
					ms->name, devno, dm_device_name(md), value);

			atomic_set( &ms->mirror_weights[devno], value );

			/* check if we must re-evaluate the maximum... */
			mirr = ms->mirror + devno;
			maxi = atomic_read(&ms->mirror_weight_max_live);
			assert_bug( maxi >= 0 && maxi < MAX_MIRRORS && maxi < ms->nr_mirrors );
			max = atomic_read( &ms->mirror_weights[ maxi ] );

			for (i = 0; i < ms->nr_mirrors; i++) {

				mirr = ms->mirror + i;
				value = atomic_read( &ms->mirror_weights[i] );

				if ( mirror_is_alive(mirr) && value > max ) { /* alive? */
					maxi = i;
					max = value;
				}
			}
			assert_bug( maxi >= 0 && maxi < MAX_MIRRORS && maxi < ms->nr_mirrors );
			atomic_set(&ms->mirror_weight_max_live, maxi );

			/* -------------------------------------------------------- */
#ifdef ENABLE_CHECK_MIRROR_CMDS
		/* Data checking commands:
		 *
		 * check_data_mirror_all <check range> <data unit/block address> <block size (bytes)>
		 *
		 * Valid <scope/range> values: "alldata", "oneblock"
		 *
		 * If <scope/range> has value "alldata", then next arg is: <data unit>
		 *
		 * If <scope/range> has value "oneblock", then next arg is: <block address>
		 */
		} else if ( !strncmp(argv[1], "check_data_mirror_all", strlen(argv[1])) ) {
			/* ---------------------------------------------------- */

			long long error_baddr = -1;
			unsigned bsize = 0;

			DMSDEBUG("HANDLING \"check_data_mirror_all block BSIZE\" message...\n");

			if ( strncmp(argv[2], "block", strlen(argv[2])) ) {
				DMERR("[%s] Invalid data unit (should be \"block\")", ms->name);
				return -EINVAL;
			}

			/* NOTE: block size parameter is in bytes! */
			if (sscanf(argv[3], "%u%c", &value, &dummy) != 1 || value < 4096 ||
				value > 512*1024 || value % 4096 != 0 ) {
				DMERR("[%s] Invalid block size: must be between 4KiB - 512KiB, aligned to 4KiB",
						ms->name);
				return -EINVAL;
			}
			bsize = value;

			/* NOTE: ti-len size is in 512 sectors! */
			if ( !check_all_mirror_data( ms, ti->len * 512LL, bsize, &error_baddr, 1/* throttle=on */ ) ) {
				md = dm_table_get_md(ti->table);
				DMERR("[%s] Check_mirror_data for device \"%s\": failed at block %lld !",
							ms->name, dm_device_name(md), error_baddr);
				return -EFAULT;
			}

			/* CAUTION: dm_table_get_md() code has changed since 2.6.18! no dm_put() needed after it! */
			md = dm_table_get_md(ti->table);
			DMINFO("[%s] Check_mirror_data for device \"%s\": SUCCESS! [All live mirror data consistent]",
			        ms->name, dm_device_name(md));

			/* ---------------------------------------------------- */
		} else if ( !strncmp(argv[1], "check_data_mirror_block", strlen(argv[1])) ) {
			/* ---------------------------------------------------- */
			unsigned long long baddr;
			int bsize = 0;

			DMSDEBUG("HANDLING \"check_data_mirror_block BLOCKNO BSIZE\" message...\n");

			/* NOTE: block address value is in 512 SECTORS and ti-len size is also in 512 sectors! */
			if (sscanf(argv[2], "%llu%c", &llvalue, &dummy) != 1 || llvalue < 0 || llvalue >= ti->len ) {
				DMERR("[%s] Invalid block address: must be between 0 and device size!", ms->name);
				return -EINVAL;
			}
			baddr = llvalue;

			/* NOTE: block size parameter is in bytes! */
			if (sscanf(argv[3], "%u%c", &value, &dummy) != 1 || value < 4096 ||
				value > 512*1024 || value % 4096 != 0 ) {
				DMERR("[%s] Invalid block size: must be between 4KiB and 512KiB, aligned to 4KiB", ms->name);
				return -EINVAL;
			}
			bsize = value;

			/* NOTE: baddr and ti-len size are measured in 512 sectors! */
			if ( !check_mirror_data_block( ms, ti->len, baddr, bsize) ) {
				md = dm_table_get_md(ti->table);
				DMERR("[%s] Check_mirror_data_block for device \"%s\": failed for block at sector %lld !",
						ms->name, dm_device_name(md), baddr);
				return -EFAULT;
			}

			md = dm_table_get_md(ti->table);
			DMINFO("[%s] Check_mirror_data_block on dev \"%s\": OK [Block addr: %lld bsize %d consistent]",
			        ms->name, dm_device_name(md), baddr, bsize);
#endif

			/* ---------------------------------------------------- */
		} else /* unknown command */
			/* ---------------------------------------------------- */
			return -EINVAL;

		/* -------------------------------------------------------- */
		/* I/O balancing tuning */
	} else if (strncmp(argv[0], "io_balance", strlen(argv[0])) == 0) {

		if ( strncmp(argv[1], "round_robin", strlen(argv[1])) == 0 ) {
			/* ---------------------------------------------------- */
			DMSDEBUG("HANDLE io_balance round_robin message...\n");
			if ( strncmp(argv[2], "ios", strlen(argv[2])) )
				return -EINVAL;

			if (sscanf(argv[3], "%u%c", &value, &dummy) != 1 || value < 2 ||
						value > 1024*1024*1024) {
				DMERR("[%s] Round robin read ios have to be 2 up to 1M", ms->name);
				return -EINVAL;
			}

			md = dm_table_get_md(ti->table);
			DMINFO("[%s] Setting round robin read ios for \"%s\" to %u",
					ms->name, dm_device_name(md), value);
			if ( atomic_read(&ms->rdpolicy) != DMS_ROUND_ROBIN )
				DMINFO("[%s] Switching read policy for \"%s\" to round robin",
					ms->name, dm_device_name(md) );

			atomic_set(&ms->rr_ios_set, value);
			atomic_set(&ms->rr_ios, value);
			atomic_set(&ms->rdpolicy, DMS_ROUND_ROBIN);
			/* ---------------------------------------------------- */
		} else if ( strncmp(argv[1], "logical_part", strlen(argv[1])) == 0 ) {
			/* ---------------------------------------------------- */
			DMSDEBUG("HANDLE io_balance logical_part message...\n");

			if ( strncmp(argv[2], "io_chunk", strlen(argv[2])) ) {
				if ( strlen(argv[2]) < 30 )
					DMERR("[%s] Invalid logical_part parameter: %s", ms->name, argv[2]);
				return -EINVAL;
			}
			if (sscanf(argv[3], "%u%c", &value, &dummy) != 1 || value < 128 ||
						value % 8 ) {
				DMERR("[%s] Logical partitioning chunks have to be >= 128 & power of 2", ms->name);
				return -EINVAL;
			}

			md = dm_table_get_md(ti->table);
			DMINFO("[%s] Setting logical partitioning chunk for \"%s\" to %u KiB",
					ms->name, dm_device_name(md), value);
			if ( atomic_read(&ms->rdpolicy) != DMS_LOGICAL_PARTITION )
				DMINFO("[%s] Switching read policy for \"%s\" to logical partitioning",
					ms->name, dm_device_name(md) );

			atomic_set(&ms->lp_io_chunk, value);
			atomic_set(&ms->rdpolicy, DMS_LOGICAL_PARTITION);
			/* ---------------------------------------------------- */
		} else if ( !strncmp(argv[1], "weighted", strlen(argv[1])) ) {
			/* ---------------------------------------------------- */
			int i, maxi = -1, max = -1;
			struct mirror *mirr = NULL;

			DMSDEBUG("HANDLE io_balance weighted message...\n");
			assert_bug( ms->nr_mirrors > 0 );
			assert_bug( ms->nr_mirrors <= MAX_MIRRORS );

			/* this is the default device weight to set for devices... */
			if ( strncmp(argv[2], "dev_weight", strlen(argv[2])) )
				return -EINVAL;

			if (sscanf(argv[3], "%u%c", &value, &dummy) != 1 || value < 1 || value > 100 ) {
				DMERR("[%s] Invalid device weights: must be between 1 - 100", ms->name);
				return -EINVAL;
			}

			md = dm_table_get_md(ti->table);
			DMINFO("[%s] Setting default device weights for \"%s\" to %u",
					ms->name, dm_device_name(md), value);
			if ( atomic_read(&ms->rdpolicy) != DMS_CUSTOM_WEIGHTED )
				DMINFO("[%s] Switching read policy for \"%s\" to weighted",
					ms->name, dm_device_name(md) );

			/* ATTENTION: we only set the weights if they are unititialized! (i.e. 0) */
			for (i = 0; i < ms->nr_mirrors; i++) {
					
				mirr = ms->mirror + i;
				atomic_set( &ms->mirror_weights[i], value );

				if ( mirror_is_alive(mirr) ) { /* alive? */
					maxi = i;
					max = value;

				} else if ( maxi < 0 ) { /* must init maxi, if uninitialized */
					maxi = i;
					max = value;
				}
			}
			assert_bug( maxi >= 0 && maxi < MAX_MIRRORS && maxi < ms->nr_mirrors );
			atomic_set(&ms->mirror_weight_max_live, maxi );
			atomic_set(&ms->rdpolicy, DMS_CUSTOM_WEIGHTED);
			/* ---------------------------------------------------- */
		} else {
			/* ---------------------------------------------------- */
			if ( strlen(argv[1]) < 30 )
				DMERR("[%s] Invalid io_balance parameter: %s", ms->name, argv[1]);
			return -EINVAL;
		}

		/* -------------------------------------------------------- */
	} else {	/* Unknown I/O command */

		if ( strlen(argv[0]) < 30 )
			DMERR("[%s] Invalid command: %s", ms->name, argv[0]);
		return -EINVAL;
	}

	return 0;
}

/*----------------------------------------------------------------- */

/*
 * device_status_char
 * @m: mirror device/leg we want the status of
 *
 * We return one character representing the most severe error
 * we have encountered.
 *    A => Alive - No failures
 *    D => Dead - A write failure occurred leaving mirror out-of-sync
 *    S => Sync - A sychronization failure occurred, mirror out-of-sync
 *    R => Read - A read failure occurred, mirror data unaffected
 *
 * Returns: <char>
 */
static char device_status_char(struct mirror *m)
{
	if ( mirror_is_alive(m) )
		return 'A';

	/* FIXME: modify these states, according to out failure modes???
	 *        -> also add recovery codes?? */

	return (test_bit(DM_RAID1_WRITE_ERROR, &(m->error_type))) ? 'D' : 'U';
	/* return (test_bit(DM_RAID1_WRITE_ERROR, &(m->error_type))) ? 'D' :
		(test_bit(DM_RAID1_SYNC_ERROR, &(m->error_type))) ? 'S' :
		(test_bit(DM_RAID1_READ_ERROR, &(m->error_type))) ? 'R' : 'U'; */
}

/*----------------------------------------------------------------- */

static char *ms_info(struct mirror_sync_set *ms, char *info, int maxlen)
{
	/* output read policy info... */
	memset( info, 0, maxlen );
	switch( atomic_read( &ms->rdpolicy ) ) {
	case DMS_LOGICAL_PARTITION:
		sprintf(info,"LP,c=%dkb", (int)atomic_read(&ms->lp_io_chunk) );
	break;
	case DMS_ROUND_ROBIN:
		sprintf(info,"RR,ios=%d", atomic_read(&ms->rr_ios_set));
	break;
	case DMS_CUSTOM_WEIGHTED:
	{
		int i, sz = 0;

		sz = sprintf(info,"CW,wml=%d", atomic_read(&ms->mirror_weight_max_live) );

		for (i = 0; i < ms->nr_mirrors; i++)
			sz += sprintf(info+sz,",w[%d]=%d", i, atomic_read( &ms->mirror_weights[i] ) );
	}
	break;
	}
	return info;
}

/*---------------------------------------------------------------------------------- */

/* Emits status info about mirror_sync_set and all mirrors... */

#define MAX_MIRR_STATUS_LEN	128

void mirror_sync_emit_status(struct mirror_sync_set *ms,
			 char *result, unsigned int maxlen)
{
	unsigned int m, sz = 0, ld = 0;
	char buffer[MAX_MIRR_STATUS_LEN];

	DMEMIT("%d %s ", ms->nr_mirrors, ms_info(ms,buffer,MAX_MIRR_STATUS_LEN) );
	for (m = 0; m < ms->nr_mirrors; m++) {
		DMEMIT("%d,%s,%c ", m, ms->mirror[m].dev->name,
							device_status_char(&(ms->mirror[m])) );
		if ( mirror_is_alive(&(ms->mirror[m])) ) /* alive? */
			ld++;
	}

	DMEMIT("\n==> Live_Devs: %d, IO_Count: TRD: %d ORD: %d TWR: %d OWR: %d", ld,
		atomic_read( &ms->read_ios_total ), atomic_read( &ms->read_ios_pending ),
		atomic_read( &ms->write_ios_total ), atomic_read( &ms->write_ios_pending) );
}

/*----------------------------------------------------------------- */

/* Returns status information about the mirror set... */

static void mirror_sync_status(struct dm_target *ti, status_type_t type,
			 unsigned status_flags, char *result, unsigned int maxlen)
{
	unsigned int m, sz = 0;
	struct mirror_sync_set *ms = (struct mirror_sync_set *) ti->private;

	DMSDEBUG("mirror_sync_status called...\n");

	switch (type) {
	case STATUSTYPE_INFO:
		DMSDEBUG("mirror_sync_status STATUSTYPE_INFO...\n");
		DMEMIT("DMS L38-310 [Build: %s %s] ", __DATE__, __TIME__);
		mirror_sync_emit_status(ms, result, maxlen);
		break;

	case STATUSTYPE_TABLE:
		DMSDEBUG("mirror_sync_status STATUSTYPE_TABLE...\n");
		DMEMIT("%d", ms->nr_mirrors);
		for (m = 0; m < ms->nr_mirrors; m++)
			DMEMIT(" %s %llu", ms->mirror[m].dev->name,
				(unsigned long long)ms->mirror[m].offset);
		break;
	}
}

/*-----------------------------------------------------------------
 * Target functions
 *---------------------------------------------------------------*/
static struct mirror_sync_set *alloc_mirror_sync_set(unsigned int nr_mirrors,
										struct dm_target *ti )
{
	int i;
	size_t len;
	struct mirror_sync_set *ms = NULL;

	if (array_too_big(sizeof(*ms), sizeof(ms->mirror[0]), nr_mirrors))
		return NULL;

	len = sizeof(*ms) + (sizeof(ms->mirror[0]) * nr_mirrors);

	ms = kmalloc(len, GFP_KERNEL);
	if (!ms) {
		ti->error = "Cannot allocate mirror context";
		return NULL;
	}

	memset(ms, 0, len);
	spin_lock_init(&ms->lock);

	ms->ti = ti;
	ms->nr_mirrors = nr_mirrors;
	atomic_set(&ms->suspend, 0); /* init suspend flag to 0 */

	spin_lock_init(&ms->choose_lock);
	ms->read_mirror = &ms->mirror[DEFAULT_MIRROR];
	ms->default_mirror = &ms->mirror[DEFAULT_MIRROR];

	ms->io_client = dm_io_client_create();
	if (IS_ERR(ms->io_client)) {
		ti->error = "Error creating dm_io client";
		kfree(ms);
 		return NULL;
	}

	/* Default policy & params set at init time, can be reconfigured later via message cmd... */
	atomic_set( &ms->rdpolicy, DMS_ROUND_ROBIN ); /* default read policy */
	//atomic_set( &ms->rdpolicy, DMS_LOGICAL_PARTITION ); /* default read policy */
	//atomic_set( &ms->rdpolicy, DMS_CUSTOM_WEIGHTED ); /* default read policy */
	atomic_set( &ms->lp_io_chunk, 1024 );	/* 1024 KiB default stripe */
	atomic_set( &ms->rr_ios_set, MIN_READS);
	atomic_set( &ms->rr_ios, MIN_READS);

	/* initialize mirror weights [for custom weighted balancing scheme]. */
	assert_bug( ms->nr_mirrors <= MAX_MIRRORS );
	for (i = 0; i < MAX_MIRRORS; i++)
		atomic_set( &ms->mirror_weights[i], 0 ); /* init to 0 == uninitialized */
	/* points to the mirror with the current max weight => changes on failure/reconfig*/

	atomic_set( &ms->mirror_weight_max_live, 0 );
	get_mirror_weight_max_live( ms ); /* re-calc mirror_weight_max_live */

	atomic_set( &ms->supress_err_messages, 0 );

	/* initialize IO counters... */
	atomic_set( &ms->read_ios_total, 0 );
	atomic_set( &ms->read_ios_pending, 0 );
	atomic_set( &ms->write_ios_total, 0 );
	atomic_set( &ms->write_ios_pending, 0 );

	/* this is the list of bios for retrying read failures... */
	bio_list_init(&ms->read_failures);

	return ms;
}

/*----------------------------------------------------------------- */

static void free_context(struct mirror_sync_set *ms, struct dm_target *ti,
			 unsigned int m)
{
	while (m--)
		dm_put_device(ti, ms->mirror[m].dev);

	dm_io_client_destroy(ms->io_client);
	kfree(ms);
}

/*----------------------------------------------------------------- */

static int get_mirror(struct mirror_sync_set *ms, struct dm_target *ti,
		      unsigned int mirror, char **argv)
{
	unsigned long long offset;
	char dummy;

	if (sscanf(argv[1], "%llu%c", &offset, &dummy) != 1) {
		ti->error = "Invalid offset";
		return -EINVAL;
	}

	DMSDEBUG("getmirror: %s off:%lld len:%ld mode:%d\n", argv[0], offset, (unsigned long)ti->len,
					dm_table_get_mode(ti->table) );
	if (dm_get_device(ti, argv[0], dm_table_get_mode(ti->table),
			  &ms->mirror[mirror].dev)) {
		ti->error = "Device lookup failure";
		return -ENXIO;
	}

	ms->mirror[mirror].offset = offset;
	atomic_set(&(ms->mirror[mirror].error_count), 0);
	ms->mirror[mirror].error_type = 0;
	ms->mirror[mirror].ms = ms;

	return 0;
}

/*----------------------------------------------------------------- */

typedef struct read_policy_params {
	int	oldparams;
	dms_read_policy policy;
	unsigned rparg[3];
} read_policy_params_t;

static int process_input_args(struct dm_target *ti,
					  unsigned int argc, char **argv,
					  unsigned int *args_used,
					  read_policy_params_t *rp )
{
	unsigned int param_count, value;
	char dummy;

	/* NOTE: this code consumes the parameters "core 2 64 nosync",
	 *       which are unused, but kept for backward compatibility
	 *       with the original dm-mirror module...
	 *
	 * Michail: added our own arguments (e.g. read policy & params) */
	if (argc < 2) {
		ti->error = "Insufficient mirror_sync arguments";
		return 0;
	}

	if (sscanf(argv[1], "%u%c", &param_count, &dummy) != 1) {
		ti->error = "Invalid mirror_sync argument count";
		return 0;
	}
	memset( (char *)rp, 0, sizeof(read_policy_params_t) );

	*args_used = 2 + param_count;

	if ( strlen(argv[0]) == 4 && strncmp(argv[0], "core", 4) == 0 ) {
		
		/* enter old "compatibility mode"... just ignore arguments... */
		if ( param_count != 2 ) {
			ti->error = "Invalid mirror_sync core arguments";
			return 0;
		}
		rp->oldparams = 1;

		/* else enter new shiny param modes... */
	} else if ( strlen(argv[0]) == strlen("round_robin") &&
				!strncmp(argv[0], "round_robin", strlen(argv[0])) ) {

		if ( param_count != 1 ) {
			ti->error = "Invalid mirror_sync round_robin arguments (need 1 arg for read I/Os)";
			return 0;
		}
		DMINFO("Round-robin policy param: %s read I/Os", argv[2] );
		if (sscanf(argv[2], "%u%c", &value, &dummy) != 1 || value < 2 || value > 1024*1024*1024) {
			ti->error = "Invalid round_robin read I/Os (have to be >= 2, max 1M)";
			return 0;
		}

		/* return the selected policy + parameters... */
		rp->oldparams = 0;
		rp->policy = DMS_ROUND_ROBIN;
		rp->rparg[0] = value;

		*args_used = 2 + param_count;

	} else if ( strlen(argv[0]) == strlen("logical_part") &&
				!strncmp(argv[0], "logical_part", strlen(argv[0])) ) {

		if ( param_count != 1 ) {
			ti->error = "Invalid mirror_sync logical_part argument (need 1 arg for partitioning chunks)";
			return 0;
		}
		DMINFO("Logical Partition policy param: Partitioning chunk: %s", argv[2] );
		if (sscanf(argv[2], "%u%c", &value, &dummy) != 1 || value < 128 || value % 8 ) {
			ti->error = "Invalid logical partitioning chunks (have to be >= 128 & power of 2)";
			return 0;
		}

		/* return the selected policy + parameters... */
		rp->oldparams = 0;
		rp->policy = DMS_LOGICAL_PARTITION;
		rp->rparg[0] = value;

		*args_used = 2 + param_count;

	} else if ( strlen(argv[0]) == strlen("weighted") &&
				!strncmp(argv[0], "weighted", strlen(argv[0])) ) {

		unsigned allweights = 0, devx = 0, weightx = 0;

		if ( param_count != 3 ) {
			ti->error = "Invalid mirror_sync weighted arguments (need 3 args for avg weight, dev idx to set X weight, weight X value)";
			return 0;
		}
		DMINFO("Weighted policy params: Default weight: %s, on dev %s using weight value: %s",
				argv[2], argv[3], argv[4] );

		if ( (sscanf(argv[2], "%u%c", &allweights, &dummy) != 1 || allweights < 1 || allweights > 100) ||
			 (sscanf(argv[4], "%u%c", &weightx, &dummy) != 1 || weightx < 1 || weightx > 100) ) {

			ti->error = "Invalid device weights: must be between 1 - 100";
			return 0;
		}
		if (sscanf(argv[5], "%u%c", &value, &dummy) != 1 || value > 16 ||
			sscanf(argv[3], "%u%c", &devx, &dummy) != 1 || devx >= value ) {
			ti->error = "Invalid weight x device index (have to be >= 0 & up to number of mirror devices)";
			return 0;
		}

		/* return the selected policy + parameters... */
		rp->oldparams = 0;
		rp->policy = DMS_CUSTOM_WEIGHTED;
		rp->rparg[0] = allweights;
		rp->rparg[1] = devx;
		rp->rparg[2] = weightx;

		*args_used = 2 + param_count;

	} else {

		ti->error = "Invalid mirror_sync arguments";
		return 0;
	}

	if (argc < *args_used) {
		ti->error = "Insufficient mirror_sync arguments";
		return 0;
	}

	return 1; /* fake ret value */
}

/*----------------------------------------------------------------- */

char *
get_all_devs_string( struct mirror_sync_set *ms, int *dslen )
{
	char b[BDEVNAME_SIZE], *devstr;
	struct mirror *m;
	int i, slen, sz;

	slen = 8;
	for (i = 0, m = ms->mirror; i < ms->nr_mirrors; i++, m++) {
		slen += 20;
		if ( m )
			slen += strlen( bdevname(m->dev->bdev, b) );
	}
	*dslen = slen;

	devstr = kzalloc(slen, GFP_KERNEL);
	sz = sprintf( devstr,"Devs: ");

	for (i = 0, m = ms->mirror; i < ms->nr_mirrors; i++, m++)
		sz += sprintf( devstr+sz, "%s(%s), ", m->dev->name, bdevname(m->dev->bdev, b) );

	return devstr;
}

/*----------------------------------------------------------------- */

/*
 * Enable/disable discard support on mirror set depending on
 * discard properties of underlying mirror_sync members.
 */
static void configure_discard_support(struct dm_target *ti, struct mirror_sync_set *ms)
{
#ifdef SUPPORT_DISCARDS /* TRIM support? */
	int i;
	struct mirror *m;

	/* Assume discards not supported until after checks below. */
	ti->discards_supported = false;

	for (i = 0, m = ms->mirror; i < ms->nr_mirrors; i++, m++) {
		struct request_queue *q;

		if (!m->dev->bdev)
			continue;

		q = bdev_get_queue(m->dev->bdev);
		if (!q || !blk_queue_discard(q))
			return;
	}

	/* All mirror_sync members properly support discards */
	ti->discards_supported = true;

	/* mirroring requires bio splitting, */
	ti->split_discard_bios = 1;
	ti->num_discard_bios = 1;
#else
	ti->discards_supported = false;
#endif
}

/*----------------------------------------------------------------- */

/* on reconfig we PRESERVE some data from the PREVIOUS mirror set instance!
 * (e.g. I/O counters, suspend flag, read policy stuff, etc. */
void
preserve_ms_params_on_reconfig( int new_ms_idx, char *devname )
{
	struct mirror_sync_set *newms, *oldms;
	int i, oidx = -1;

	/* sanity check... */
	assert_bug( new_ms_idx >= 0 && new_ms_idx < curr_ms_instances );
	assert_bug( strncmp(reconf_ms[new_ms_idx].devname, devname, strlen(devname)) == 0);
	newms = reconf_ms[new_ms_idx].current_ms;

	/* search for ANOTHER live ms with the same device name!
	 * -> if found: reconfig in progress! */
	for (i = 0; i < curr_ms_instances; i++)
		if ( i != new_ms_idx && atomic_read( &reconf_ms[i].in_use ) > 0 &&
	    	strncmp( devname, reconf_ms[i].devname, strlen(devname) ) == 0 ) {
				oidx = i;
				break;
		}

	DMSDEBUG("preserve_ms_params_on_reconfig=> new_idx:%d old_idx:%d !!\n",
				new_ms_idx, oidx );

	if ( oidx >= 0 ) { /* found something... */
		char *ods, *nds;
		int odslen, ndslen;

		DMSDEBUG("preserve_ms_params_on_reconfig=> RECONFIG DETECTED !!\n");
		oldms = reconf_ms[oidx].current_ms;

		/* reconfig detected, need to PRESERVE some data from the PREVIOUS mirror set! */
		assert_bug( oldms && newms );
		//assert( atomic_read(&oldms->suspend) == 1 );

		if ( newms->nr_mirrors != oldms->nr_mirrors ) /* SAME number of mirrors?? */
			DMWARN("[%s] Detected RECONFIG to DIFFERENT number of mirror devs: %d -> %d",
					oldms->name, oldms->nr_mirrors, newms->nr_mirrors );

		ods = get_all_devs_string( oldms, &odslen ); /* NOTE: allocs mem for returned string! */
		nds = get_all_devs_string( newms, &ndslen );
		if ( ods && nds )
			DMINFO("[%s] RECONFIG: %d %s %s-> %d %s %s",
				oldms->name, oldms->nr_mirrors, oldms->name, ods,
				newms->nr_mirrors, newms->name, nds );
		kfree( ods ); /* have to free string now */
		kfree( nds );

		/* ATTENTION: Device ordering on reconfig is NOT identical!
		 *            Read policy values are not copied over on reconfig! */
		atomic_set( &newms->suspend, atomic_read(&oldms->suspend) );
		atomic_set( &newms->rr_ios_set, atomic_read(&oldms->rr_ios_set));

		{ /* Check if the error counters and bits have been reset! */
			struct mirror *m;
			
			for (i = 0, m = newms->mirror; i < newms->nr_mirrors; i++, m++) {
				assert( m->error_type == 0 );
				assert( atomic_read(&m->error_count) == 0 );
			}
		}

		get_mirror_weight_max_live( newms ); /* re-calc mirror_weight_max_live */

		atomic_set( &newms->supress_err_messages, 0 ); /* clear error messages on reconfig... */

		/* preserve IO counters... */
		atomic_set( &newms->read_ios_total, atomic_read(&oldms->read_ios_total));
		atomic_set( &newms->read_ios_pending, atomic_read(&oldms->read_ios_pending));
		atomic_set( &newms->write_ios_total, atomic_read(&oldms->write_ios_total));
		atomic_set( &newms->write_ios_pending, atomic_read(&oldms->write_ios_pending));
	}
}

/*----------------------------------------------------------------- */

/*
 * Construct a mirror sync mapping:
 * #mirrors [mirror_sync_path offset]{2,}
 *
 * Michail: we do not have a log, BUT to preserve backwards compatibility
 *          with dm-mirror we pretend that we really use the log arguments ...
 *
 * Example mirror_sync creation, table has with 2 devices:
 * dmsetup create dms --table '0 4000430 mirror_sync core 2 64 nosync 2 /dev/sdb 0 /dev/sdc 0'
 *
 */
static int mirror_sync_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	int i, r;
	unsigned int nr_mirrors, m, args_used;
	struct mirror_sync_set *ms;
	struct mapped_device *md;
	read_policy_params_t rp;
	char mdname[ DEVNAME_MAXLEN ];
	char dummy;

	DMSDEBUG_CALL("mirror_sync_ctr called...\n");

	/* NOTE: the parameters "core 2 64 nosync" are unused, but consumed in process_input_args()...
	 * If used with our own parameters, e.g. for the read priority policy... we set those... */
	if ( !process_input_args(ti, argc, argv, &args_used, &rp) )
		return -EINVAL;

	argv += args_used;
	argc -= args_used;

	if (!argc || sscanf(argv[0], "%u%c", &nr_mirrors, &dummy) != 1 ||
	    nr_mirrors < 2 || nr_mirrors > DM_KCOPYD_MAX_REGIONS + 1 ||
		nr_mirrors > MAX_MIRRORS ) {
		ti->error = "Invalid number of mirrors";
		return -EINVAL;
	}

	argv++, argc--;

	if (argc != nr_mirrors * 2) {
		ti->error = "Wrong number of mirror arguments";
		return -EINVAL;
	}

	ms = alloc_mirror_sync_set(nr_mirrors, ti);
	if (!ms)
		return -ENOMEM;

	/* Get the mirror parameter sets */
	for (m = 0; m < nr_mirrors; m++) {
		r = get_mirror(ms, ti, m, argv);
		if (r) {
			free_context(ms, ti, m);
			return r;
		}
		argv += 2;
		argc -= 2;
	}

	ti->private = ms;
	r = dm_set_target_max_io_len(ti, 1 << 13); /* sectors == 4 MB... used to be dm_rh_get_region_size(ms->rh); */
	if (r)
		return -EINVAL;
	ti->num_flush_bios = 1;
	ti->num_discard_bios = 1;
	/* CAUTION: need the following for dm_per_bio_data()! */
	ti->per_bio_data_size = sizeof(struct dms_bio_map_info); // Linux-3.x specific
	ti->discard_zeroes_data_unsupported = true;

	/* CAUTION: dm_table_get_md() code has changed since 2.6.18! no dm_put() needed after it! */
	md = dm_table_get_md(ti->table);

	/* check the name of the device... this is actually the major:minor device
	 * name in the kernel and should not exceed 10 chars... */
	if ( strlen(dm_device_name(md)) >= DEVNAME_MAXLEN ) {
		ti->error = "Internal error: DM-Device name too long!";
		return -EINVAL;
	}
	/* copy the device name locally... */
	memset( mdname, 0, DEVNAME_MAXLEN );
	memcpy( mdname, dm_device_name(md), strlen( dm_device_name(md) ) );

	memset( ms->name, 0, DEVNAME_MAXLEN );
	memcpy( ms->name, mdname, strlen( mdname ) );

	/* find an unused reconfig space & store its index in ms... */
	ms->reconfig_idx = curr_ms_instances + 1;
	for (i = 0; i < curr_ms_instances; i++) {

		if ( atomic_read( &reconf_ms[i].in_use ) > 0 )
			continue;
		if ( atomic_inc_return( &reconf_ms[i].in_use ) == 1 ) {
			assert( reconf_ms[i].current_ms == NULL );
			ms->reconfig_idx = i;
			break;
		} else /* someone beat us into this spot... */
			atomic_dec( &reconf_ms[i].in_use );
	}
	/* handle the case where all slots are full! */
	if (i >= curr_ms_instances) {
		ti->error = "Too many mirror_sync instances loaded!";
		// FIXME : grow the array of instances!
		return -EINVAL;
	}

	/* store the latest mirror set contructed, we need this for passing reconfig params */
	assert_bug( ms->reconfig_idx < curr_ms_instances );
	assert_bug( atomic_read( &reconf_ms[ ms->reconfig_idx ].in_use ) > 0 );
	reconf_ms[ ms->reconfig_idx ].current_ms = ms;
	memset( reconf_ms[ ms->reconfig_idx ].devname, 0, DEVNAME_MAXLEN );

	DMWARN("[%s] DMS Device INIT: Number of mirrors: %d", mdname, nr_mirrors );
	memcpy( reconf_ms[ ms->reconfig_idx ].devname, mdname, strlen( mdname ) );

	/* on reconfig we PRESERVE some data from the PREVIOUS mirror set instance!
	 * (e.g. I/O counters, suspend flag, read policy stuff, etc. */
	preserve_ms_params_on_reconfig( ms->reconfig_idx, reconf_ms[ ms->reconfig_idx ].devname );

	ms->kmirror_syncd_wq = create_singlethread_workqueue("kmirror_syncd");
	if (!ms->kmirror_syncd_wq) {
		DMERR("[%s] Error: Couldn't start kmirror_syncd", ms->name);
		free_context(ms, ti, ms->nr_mirrors);
		return -ENOMEM;
	}
	INIT_WORK(&ms->kmirror_syncd_work, main_mirror_syncd);
	//init_timer(&ms->timer);
	ms->timer_pending = 0;
	INIT_WORK(&ms->trigger_event, trigger_event);

	/*
	 * Disable/enable discard support on mirror set.
	 */
	configure_discard_support(ti, ms);

	/* finally, set any read policy, if chosen in the startup arguments... */
	if ( !rp.oldparams ) {

		int i, maxi = -1, max = -1;
		struct mirror *mirr = NULL;

		switch (rp.policy) { /* check the argument processing */
		case DMS_ROUND_ROBIN:
			md = dm_table_get_md(ti->table);
			DMINFO("[%s] Setting read policy for \"%s\" to round robin with ios= %u",
					ms->name, dm_device_name(md), rp.rparg[0] );

			assert( rp.rparg[0] >= 2 && rp.rparg[0] <= 1024*1024*1024 );
			atomic_set(&ms->rr_ios_set, rp.rparg[0]);
			atomic_set(&ms->rr_ios, rp.rparg[0]);
			atomic_set(&ms->rdpolicy, DMS_ROUND_ROBIN);

		break;
		case DMS_LOGICAL_PARTITION:

			md = dm_table_get_md(ti->table);
			DMINFO("[%s] Setting read policy for \"%s\" to logical partitioning with chunk= %u",
					ms->name, dm_device_name(md), rp.rparg[0] );

			atomic_set(&ms->lp_io_chunk, rp.rparg[0]);
			atomic_set(&ms->rdpolicy, DMS_LOGICAL_PARTITION);

		break;
		case DMS_CUSTOM_WEIGHTED:

			md = dm_table_get_md(ti->table);
			DMINFO("[%s] Setting read policy for \"%s\" to weighted with weights= %u",
					ms->name, dm_device_name(md), rp.rparg[0] );

			for (i = 0; i < ms->nr_mirrors; i++) {

				mirr = ms->mirror + i;
				atomic_set( &ms->mirror_weights[i], rp.rparg[0] );

				if ( mirror_is_alive(mirr) ) { /* alive? */
					maxi = i;
					max = atomic_read( &ms->mirror_weights[i] );
				}
			}

			/* set the weight value X for device specified... */
			if (rp.rparg[1] >= 0 && rp.rparg[1] < ms->nr_mirrors)
				atomic_set( &ms->mirror_weights[ rp.rparg[1] ], rp.rparg[2] );

			/* ok, now calculate the wml device... */
			for (i = 0; i < ms->nr_mirrors; i++) {

				mirr = ms->mirror + i;
				if ( mirror_is_alive(mirr) &&
					max < atomic_read( &ms->mirror_weights[i]) ) { /* alive? */
					maxi = i;
					max = atomic_read( &ms->mirror_weights[i]);
				}
			}
			assert( maxi >= 0 && maxi < MAX_MIRRORS && maxi < ms->nr_mirrors );
			atomic_set(&ms->mirror_weight_max_live, maxi );
			atomic_set(&ms->rdpolicy, DMS_CUSTOM_WEIGHTED);
		break;
		}
	}

	return 0;
}

/*----------------------------------------------------------------- */

static void mirror_sync_dtr(struct dm_target *ti)
{
	struct mirror_sync_set *ms = (struct mirror_sync_set *) ti->private;

	DMSDEBUG_CALL("mirror_sync_dtr called...\n");
	DMWARN("[%s] DMS Device EXIT.",
			reconf_ms[ ms->reconfig_idx ].devname);

	/* free the old reconfig slot... */
	assert_bug( ms->reconfig_idx < curr_ms_instances );
	reconf_ms[ ms->reconfig_idx ].current_ms = NULL;
	memset( reconf_ms[ ms->reconfig_idx ].devname, 0, DEVNAME_MAXLEN );
	atomic_set( &reconf_ms[ ms->reconfig_idx ].in_use, 0 );

	//del_timer_sync(&ms->timer);
	flush_workqueue(ms->kmirror_syncd_wq);
	flush_scheduled_work();
	destroy_workqueue(ms->kmirror_syncd_wq);

	free_context(ms, ti, ms->nr_mirrors);
}

/*----------------------------------------------------------------- */

static int mirror_sync_iterate_devices(struct dm_target *ti,
				  iterate_devices_callout_fn fn, void *data)
{
	struct mirror_sync_set *ms = (struct mirror_sync_set *) ti->private;
	int ret = 0;
	unsigned i;

	DMSDEBUG_CALL("mirror_sync_iterate_devices called...\n");
	for (i = 0; !ret && i < ms->nr_mirrors; i++)
		ret = fn(ti, ms->mirror[i].dev,
			 ms->mirror[i].offset, ti->len, data);

	return ret;
}

/*----------------------------------------------------------------- */

/* Some info about the device mapper target_type functions
 *
 * 1. Constructor:
 * 	  typedef int (*dm_ctr_fn) (struct dm_target *target,
 * 	  			unsigned int argc, char **argv);
 *
 *    In the constructor the target parameter will already have the
 *    table, type, begin and len fields filled in.
 *
 * 2. Destructor:
 *    typedef void (*dm_dtr_fn) (struct dm_target *ti);
 *
 *    The destructor doesn't need to free the dm_target, just
 *    anything hidden ti->private.
 *
 * 3. Map function:
 *    typedef int (*dm_map_fn) (struct dm_target *ti, struct bio *bio);
 *
 *    The map function must return:
 *    < 0: error
 *    = 0: The target will handle the io by resubmitting it later
 *    = 1: simple remap complete
 *    = 2: The target wants to push back the io
 *
 * 4. End_io function:
 *    typedef int (*dm_endio_fn) (struct dm_target *ti,
 *    			struct bio *bio, int error);
 *
 *    Returns:
 *    < 0 : error (currently ignored)
 *    0   : ended successfully
 *    1   : for some reason the io has still not completed (eg,
 *          multipath target might want to requeue a failed io).
 *    2   : The target wants to push back the io
 */

static struct target_type mirror_sync_target = {
	/* .features = ??*/		/* Features uint64_t */
	.name	 = "mirror_sync",
	.version = {1, 0, 3},
	.module	 = THIS_MODULE,
	.ctr	 = mirror_sync_ctr,	/* Contructor function */
	.dtr	 = mirror_sync_dtr,	/* Destructor function */
	.map	 = mirror_sync_map,	/* Map function */
	.end_io	 = mirror_sync_end_io,	/* End_io function */
	.presuspend = mirror_sync_presuspend,	/* Pre-suspend function */
	.postsuspend = mirror_sync_postsuspend,	/* Post-suspend function */
	.resume	 = mirror_sync_resume,	/* Resume function */
	.message = mirror_sync_message,	/* Message function */
	.status	 = mirror_sync_status,	/* Status function */
	.iterate_devices = mirror_sync_iterate_devices,
	// FIXME: NEED these functions in mirror_sync ?? now used only on striping...
	//.io_hints = mirror_sync_io_hints,
};

static int __init dm_mirror_sync_init(void)
{
	int i, r = -ENOMEM;

	assert_bug( MAX_MIRRORS > 1 );

	/* initialize the reconfig space */
	curr_ms_instances = MAX_DMS_INSTANCES;
	reconf_ms = kzalloc( curr_ms_instances * sizeof(struct reconfig_ms_set), GFP_KERNEL);
	if ( !reconf_ms ) {
		DMERR("[%s] Failed to allocate memory for reconf_ms", mirror_sync_target.name);
		return r;
	}

	for (i = 0; i < curr_ms_instances; i++) {
		atomic_set( &reconf_ms[i].in_use, 0 );
		reconf_ms[i].current_ms = NULL;
		memset( reconf_ms[i].devname, 0, DEVNAME_MAXLEN );
	}

	r = dm_register_target(&mirror_sync_target);
	if (r < 0) {
		DMERR("[%s] Failed to register mirror target", mirror_sync_target.name);
		goto bad_target;
	}

	printk(KERN_INFO "DMS L38-310 [Build: %s %s]: Loaded OK.\n", __DATE__, __TIME__);

	return 0;

bad_target:
	kfree( reconf_ms );
	return r;
}

static void __exit dm_mirror_sync_exit(void)
{
	printk(KERN_INFO "DMS L38-310 [Build: %s %s]: Exiting.\n", __DATE__, __TIME__);

	dm_unregister_target(&mirror_sync_target);

	kfree( reconf_ms );
}

/* Module hooks */
module_init(dm_mirror_sync_init);
module_exit(dm_mirror_sync_exit);

MODULE_AUTHOR("Michail Flouris <michail.flouris at onapp.com>");
MODULE_DESCRIPTION(
	"(C) Copyright OnApp Ltd. 2012-2013  All Rights Reserved.\n"
	DM_NAME " mirror target for synchronous, fail-over writes and tunable read policies "
);
MODULE_LICENSE("GPL");
