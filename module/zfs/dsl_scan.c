/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright (c) 2008, 2010, Oracle and/or its affiliates. All rights reserved.
 */

#include <sys/dsl_scan.h>
#include <sys/dsl_pool.h>
#include <sys/dsl_dataset.h>
#include <sys/dsl_prop.h>
#include <sys/dsl_dir.h>
#include <sys/dsl_synctask.h>
#include <sys/dnode.h>
#include <sys/dmu_tx.h>
#include <sys/dmu_objset.h>
#include <sys/arc.h>
#include <sys/zap.h>
#include <sys/zio.h>
#include <sys/zfs_context.h>
#include <sys/fs/zfs.h>
#include <sys/zfs_znode.h>
#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/zil_impl.h>
#include <sys/zio_checksum.h>
#include <sys/ddt.h>
#include <sys/sa.h>
#include <sys/sa_impl.h>
#ifdef _KERNEL
#include <sys/zfs_vfsops.h>
#endif

typedef int (scan_cb_t)(dsl_pool_t *, const blkptr_t *, const zbookmark_t *);

static scan_cb_t dsl_scan_defrag_cb;
static scan_cb_t dsl_scan_scrub_cb;
static scan_cb_t dsl_scan_remove_cb;
static dsl_syncfunc_t dsl_scan_cancel_sync;
static void dsl_scan_sync_state(dsl_scan_t *, dmu_tx_t *tx);

int zfs_scan_min_time_ms = 1000; /* min millisecs to scrub per txg */
int zfs_free_min_time_ms = 1000; /* min millisecs to free per txg */
int zfs_resilver_min_time_ms = 3000; /* min millisecs to resilver per txg */
boolean_t zfs_no_scrub_io = B_FALSE; /* set to disable scrub i/o */
boolean_t zfs_no_scrub_prefetch = B_FALSE; /* set to disable srub prefetching */
enum ddt_class zfs_scrub_ddt_class_max = DDT_CLASS_DUPLICATE;
int dsl_scan_delay_completion = B_FALSE; /* set to delay scan completion */

#define	DSL_SCAN_IS_SCRUB_RESILVER(scn) \
	((scn)->scn_phys.scn_func == POOL_SCAN_SCRUB || \
	(scn)->scn_phys.scn_func == POOL_SCAN_RESILVER)

extern int zfs_txg_timeout;

/* the order has to match pool_scan_type */
static scan_cb_t *scan_funcs[POOL_SCAN_FUNCS] = {
	NULL,
	dsl_scan_scrub_cb,	/* POOL_SCAN_SCRUB */
	dsl_scan_scrub_cb,	/* POOL_SCAN_RESILVER */
};

int
dsl_scan_init(dsl_pool_t *dp, uint64_t txg)
{
	int err;
	dsl_scan_t *scn;
	spa_t *spa = dp->dp_spa;
	uint64_t f;

	scn = dp->dp_scan = kmem_zalloc(sizeof (dsl_scan_t), KM_SLEEP);
	scn->scn_dp = dp;

	err = zap_lookup(dp->dp_meta_objset, DMU_POOL_DIRECTORY_OBJECT,
	    "scrub_func", sizeof (uint64_t), 1, &f);
	if (err == 0) {
		/*
		 * There was an old-style scrub in progress.  Restart a
		 * new-style scrub from the beginning.
		 */
		scn->scn_restart_txg = txg;
		zfs_dbgmsg("old-style scrub was in progress; "
		    "restarting new-style scrub in txg %llu",
		    scn->scn_restart_txg);

		/*
		 * Load the queue obj from the old location so that it
		 * can be freed by dsl_scan_done().
		 */
		(void) zap_lookup(dp->dp_meta_objset, DMU_POOL_DIRECTORY_OBJECT,
		    "scrub_queue", sizeof (uint64_t), 1,
		    &scn->scn_phys.scn_queue_obj);
	} else {
		err = zap_lookup(dp->dp_meta_objset, DMU_POOL_DIRECTORY_OBJECT,
		    DMU_POOL_SCAN, sizeof (uint64_t), SCAN_PHYS_NUMINTS,
		    &scn->scn_phys);
		if (err == ENOENT)
			return (0);
		else if (err)
			return (err);

		if (scn->scn_phys.scn_state == DSS_SCANNING &&
		    spa_prev_software_version(dp->dp_spa) < SPA_VERSION_SCAN) {
			/*
			 * A new-type scrub was in progress on an old
			 * pool, and the pool was accessed by old
			 * software.  Restart from the beginning, since
			 * the old software may have changed the pool in
			 * the meantime.
			 */
			scn->scn_restart_txg = txg;
			zfs_dbgmsg("new-style scrub was modified "
			    "by old software; restarting in txg %llu",
			    scn->scn_restart_txg);
		}
	}

	spa_scan_stat_init(spa);
	return (0);
}

void
dsl_scan_fini(dsl_pool_t *dp)
{
	if (dp->dp_scan) {
		kmem_free(dp->dp_scan, sizeof (dsl_scan_t));
		dp->dp_scan = NULL;
	}
}

/* ARGSUSED */
static int
dsl_scan_setup_check(void *arg1, void *arg2, dmu_tx_t *tx)
{
	dsl_scan_t *scn = arg1;

	if (scn->scn_phys.scn_state == DSS_SCANNING)
		return (EBUSY);

	return (0);
}

/* ARGSUSED */
static void
dsl_scan_setup_sync(void *arg1, void *arg2, dmu_tx_t *tx)
{
	dsl_scan_t *scn = arg1;
	pool_scan_func_t *funcp = arg2;
	dmu_object_type_t ot = 0;
	dsl_pool_t *dp = scn->scn_dp;
	spa_t *spa = dp->dp_spa;

	ASSERT(scn->scn_phys.scn_state != DSS_SCANNING);
	ASSERT(*funcp > POOL_SCAN_NONE && *funcp < POOL_SCAN_FUNCS);
	bzero(&scn->scn_phys, sizeof (scn->scn_phys));
	scn->scn_phys.scn_func = *funcp;
	scn->scn_phys.scn_state = DSS_SCANNING;
	scn->scn_phys.scn_min_txg = 0;
	scn->scn_phys.scn_max_txg = tx->tx_txg;
	scn->scn_phys.scn_ddt_class_max = DDT_CLASSES - 1; /* the entire DDT */
	scn->scn_phys.scn_start_time = gethrestime_sec();
	scn->scn_phys.scn_errors = 0;
	scn->scn_phys.scn_to_examine = spa->spa_root_vdev->vdev_stat.vs_alloc;
	scn->scn_restart_txg = 0;
	spa_scan_stat_init(spa);

	if (DSL_SCAN_IS_SCRUB_RESILVER(scn)) {
		scn->scn_phys.scn_ddt_class_max = zfs_scrub_ddt_class_max;

		/* rewrite all disk labels */
		vdev_config_dirty(spa->spa_root_vdev);

		if (vdev_resilver_needed(spa->spa_root_vdev,
		    &scn->scn_phys.scn_min_txg, &scn->scn_phys.scn_max_txg)) {
			spa_event_notify(spa, NULL, ESC_ZFS_RESILVER_START);
		} else {
			spa_event_notify(spa, NULL, ESC_ZFS_SCRUB_START);
		}

		spa->spa_scrub_started = B_TRUE;
		/*
		 * If this is an incremental scrub, limit the DDT scrub phase
		 * to just the auto-ditto class (for correctness); the rest
		 * of the scrub should go faster using top-down pruning.
		 */
		if (scn->scn_phys.scn_min_txg > TXG_INITIAL)
			scn->scn_phys.scn_ddt_class_max = DDT_CLASS_DITTO;

	}

	/* back to the generic stuff */

	if (dp->dp_blkstats == NULL) {
		dp->dp_blkstats =
		    kmem_alloc(sizeof (zfs_all_blkstats_t), KM_SLEEP);
	}
	bzero(dp->dp_blkstats, sizeof (zfs_all_blkstats_t));

	if (spa_version(spa) < SPA_VERSION_DSL_SCRUB)
		ot = DMU_OT_ZAP_OTHER;

	scn->scn_phys.scn_queue_obj = zap_create(dp->dp_meta_objset,
	    ot ? ot : DMU_OT_SCAN_QUEUE, DMU_OT_NONE, 0, tx);

	dsl_scan_sync_state(scn, tx);

	spa_history_log_internal(LOG_POOL_SCAN, spa, tx,
	    "func=%u mintxg=%llu maxtxg=%llu",
	    *funcp, scn->scn_phys.scn_min_txg, scn->scn_phys.scn_max_txg);
}

/* ARGSUSED */
static void
dsl_scan_done(dsl_scan_t *scn, boolean_t complete, dmu_tx_t *tx)
{
	static const char *old_names[] = {
		"scrub_bookmark",
		"scrub_ddt_bookmark",
		"scrub_ddt_class_max",
		"scrub_queue",
		"scrub_min_txg",
		"scrub_max_txg",
		"scrub_func",
		"scrub_errors",
		NULL
	};

	dsl_pool_t *dp = scn->scn_dp;
	spa_t *spa = dp->dp_spa;
	int i;

	/* Remove any remnants of an old-style scrub. */
	for (i = 0; old_names[i]; i++) {
		(void) zap_remove(dp->dp_meta_objset,
		    DMU_POOL_DIRECTORY_OBJECT, old_names[i], tx);
	}

	if (scn->scn_phys.scn_queue_obj != 0) {
		VERIFY(0 == dmu_object_free(dp->dp_meta_objset,
		    scn->scn_phys.scn_queue_obj, tx));
		scn->scn_phys.scn_queue_obj = 0;
	}

	/*
	 * If we were "restarted" from a stopped state, don't bother
	 * with anything else.
	 */
	if (scn->scn_phys.scn_state != DSS_SCANNING)
		return;

	if (complete)
		scn->scn_phys.scn_state = DSS_FINISHED;
	else
		scn->scn_phys.scn_state = DSS_CANCELED;

	spa_history_log_internal(LOG_POOL_SCAN_DONE, spa, tx,
	    "complete=%u", complete);

	if (DSL_SCAN_IS_SCRUB_RESILVER(scn)) {
		mutex_enter(&spa->spa_scrub_lock);
		while (spa->spa_scrub_inflight > 0) {
			cv_wait(&spa->spa_scrub_io_cv,
			    &spa->spa_scrub_lock);
		}
		mutex_exit(&spa->spa_scrub_lock);
		spa->spa_scrub_started = B_FALSE;
		spa->spa_scrub_active = B_FALSE;

		/*
		 * If the scrub/resilver completed, update all DTLs to
		 * reflect this.  Whether it succeeded or not, vacate
		 * all temporary scrub DTLs.
		 */
		vdev_dtl_reassess(spa->spa_root_vdev, tx->tx_txg,
		    complete ? scn->scn_phys.scn_max_txg : 0, B_TRUE);
		if (complete) {
			spa_event_notify(spa, NULL, scn->scn_phys.scn_min_txg ?
			    ESC_ZFS_RESILVER_FINISH : ESC_ZFS_SCRUB_FINISH);
		}
		spa_errlog_rotate(spa);

		/*
		 * We may have finished replacing a device.
		 * Let the async thread assess this and handle the detach.
		 */
		spa_async_request(spa, SPA_ASYNC_RESILVER_DONE);
	}

	scn->scn_phys.scn_end_time = gethrestime_sec();
}

/* ARGSUSED */
static int
dsl_scan_cancel_check(void *arg1, void *arg2, dmu_tx_t *tx)
{
	dsl_scan_t *scn = arg1;

	if (scn->scn_phys.scn_state != DSS_SCANNING)
		return (ENOENT);
	return (0);
}

/* ARGSUSED */
static void
dsl_scan_cancel_sync(void *arg1, void *arg2, dmu_tx_t *tx)
{
	dsl_scan_t *scn = arg1;

	dsl_scan_done(scn, B_FALSE, tx);
	dsl_scan_sync_state(scn, tx);
}

int
dsl_scan_cancel(dsl_pool_t *dp)
{
	boolean_t complete = B_FALSE;
	int err;

	err = dsl_sync_task_do(dp, dsl_scan_cancel_check,
	    dsl_scan_cancel_sync, dp->dp_scan, &complete, 3);
	return (err);
}

static void dsl_scan_visitbp(blkptr_t *bp,
    const zbookmark_t *zb, dnode_phys_t *dnp, arc_buf_t *pbuf,
    dsl_dataset_t *ds, dsl_scan_t *scn, dmu_objset_type_t ostype,
    dmu_tx_t *tx);
static void dsl_scan_visitdnode(dsl_scan_t *, dsl_dataset_t *ds,
    dmu_objset_type_t ostype,
    dnode_phys_t *dnp, arc_buf_t *buf, uint64_t object, dmu_tx_t *tx);

void
dsl_free(dsl_pool_t *dp, uint64_t txg, const blkptr_t *bp)
{
	zio_free(dp->dp_spa, txg, bp);
}

void
dsl_free_sync(zio_t *pio, dsl_pool_t *dp, uint64_t txg, const blkptr_t *bpp)
{
	ASSERT(dsl_pool_sync_context(dp));
	zio_nowait(zio_free_sync(pio, dp->dp_spa, txg, bpp, pio->io_flags));
}

int
dsl_read(zio_t *pio, spa_t *spa, const blkptr_t *bpp, arc_buf_t *pbuf,
    arc_done_func_t *done, void *private, int priority, int zio_flags,
    uint32_t *arc_flags, const zbookmark_t *zb)
{
	return (arc_read(pio, spa, bpp, pbuf, done, private,
	    priority, zio_flags, arc_flags, zb));
}

int
dsl_read_nolock(zio_t *pio, spa_t *spa, const blkptr_t *bpp,
    arc_done_func_t *done, void *private, int priority, int zio_flags,
    uint32_t *arc_flags, const zbookmark_t *zb)
{
	return (arc_read_nolock(pio, spa, bpp, done, private,
	    priority, zio_flags, arc_flags, zb));
}

static boolean_t
bookmark_is_zero(const zbookmark_t *zb)
{
	return (zb->zb_objset == 0 && zb->zb_object == 0 &&
	    zb->zb_level == 0 && zb->zb_blkid == 0);
}

/* dnp is the dnode for zb1->zb_object */
static boolean_t
bookmark_is_before(const dnode_phys_t *dnp, const zbookmark_t *zb1,
    const zbookmark_t *zb2)
{
	uint64_t zb1nextL0, zb2thisobj;

	ASSERT(zb1->zb_objset == zb2->zb_objset);
	ASSERT(zb2->zb_level == 0);

	/*
	 * A bookmark in the deadlist is considered to be after
	 * everything else.
	 */
	if (zb2->zb_object == DMU_DEADLIST_OBJECT)
		return (B_TRUE);

	/* The objset_phys_t isn't before anything. */
	if (dnp == NULL)
		return (B_FALSE);

	zb1nextL0 = (zb1->zb_blkid + 1) <<
	    ((zb1->zb_level) * (dnp->dn_indblkshift - SPA_BLKPTRSHIFT));

	zb2thisobj = zb2->zb_object ? zb2->zb_object :
	    zb2->zb_blkid << (DNODE_BLOCK_SHIFT - DNODE_SHIFT);

	if (zb1->zb_object == DMU_META_DNODE_OBJECT) {
		uint64_t nextobj = zb1nextL0 *
		    (dnp->dn_datablkszsec << SPA_MINBLOCKSHIFT) >> DNODE_SHIFT;
		return (nextobj <= zb2thisobj);
	}

	if (zb1->zb_object < zb2thisobj)
		return (B_TRUE);
	if (zb1->zb_object > zb2thisobj)
		return (B_FALSE);
	if (zb2->zb_object == DMU_META_DNODE_OBJECT)
		return (B_FALSE);
	return (zb1nextL0 <= zb2->zb_blkid);
}

static uint64_t
dsl_scan_ds_maxtxg(dsl_dataset_t *ds)
{
	uint64_t smt = ds->ds_dir->dd_pool->dp_scan->scn_phys.scn_max_txg;
	if (dsl_dataset_is_snapshot(ds))
		return (MIN(smt, ds->ds_phys->ds_creation_txg));
	return (smt);
}

static void
dsl_scan_sync_state(dsl_scan_t *scn, dmu_tx_t *tx)
{
	VERIFY(0 == zap_update(scn->scn_dp->dp_meta_objset,
	    DMU_POOL_DIRECTORY_OBJECT,
	    DMU_POOL_SCAN, sizeof (uint64_t), SCAN_PHYS_NUMINTS,
	    &scn->scn_phys, tx));
}

static boolean_t
dsl_scan_check_pause(dsl_scan_t *scn, const zbookmark_t *zb)
{
	uint64_t elapsed_nanosecs;
	int mintime;

	/* we never skip user/group accounting objects */
	if (zb && (int64_t)zb->zb_object < 0)
		return (B_FALSE);

	if (scn->scn_pausing)
		return (B_TRUE); /* we're already pausing */

	if (!bookmark_is_zero(&scn->scn_phys.scn_bookmark))
		return (B_FALSE); /* we're resuming */

	/* We only know how to resume from level-0 blocks. */
	if (zb && zb->zb_level != 0)
		return (B_FALSE);

	mintime = (scn->scn_phys.scn_func == POOL_SCAN_RESILVER) ?
	    zfs_resilver_min_time_ms : zfs_scan_min_time_ms;
	elapsed_nanosecs = gethrtime() - scn->scn_sync_start_time;
	if (elapsed_nanosecs / NANOSEC > zfs_txg_timeout ||
	    (elapsed_nanosecs / MICROSEC > mintime &&
	    txg_sync_waiting(scn->scn_dp)) ||
	    spa_shutting_down(scn->scn_dp->dp_spa)) {
		if (zb) {
			dprintf("pausing at bookmark %llx/%llx/%llx/%llx\n",
			    (longlong_t)zb->zb_objset,
			    (longlong_t)zb->zb_object,
			    (longlong_t)zb->zb_level,
			    (longlong_t)zb->zb_blkid);
			scn->scn_phys.scn_bookmark = *zb;
		}
		dprintf("pausing at DDT bookmark %llx/%llx/%llx/%llx\n",
		    (longlong_t)scn->scn_phys.scn_ddt_bookmark.ddb_class,
		    (longlong_t)scn->scn_phys.scn_ddt_bookmark.ddb_type,
		    (longlong_t)scn->scn_phys.scn_ddt_bookmark.ddb_checksum,
		    (longlong_t)scn->scn_phys.scn_ddt_bookmark.ddb_cursor);
		scn->scn_pausing = B_TRUE;
		return (B_TRUE);
	}
	return (B_FALSE);
}

typedef struct zil_scan_arg {
	dsl_pool_t	*zsa_dp;
	zil_header_t	*zsa_zh;
} zil_scan_arg_t;

/* ARGSUSED */
static int
dsl_scan_zil_block(zilog_t *zilog, blkptr_t *bp, void *arg, uint64_t claim_txg)
{
	zil_scan_arg_t *zsa = arg;
	dsl_pool_t *dp = zsa->zsa_dp;
	dsl_scan_t *scn = dp->dp_scan;
	zil_header_t *zh = zsa->zsa_zh;
	zbookmark_t zb;

	if (bp->blk_birth <= scn->scn_phys.scn_cur_min_txg)
		return (0);

	/*
	 * One block ("stubby") can be allocated a long time ago; we
	 * want to visit that one because it has been allocated
	 * (on-disk) even if it hasn't been claimed (even though for
	 * scrub there's nothing to do to it).
	 */
	if (claim_txg == 0 && bp->blk_birth >= spa_first_txg(dp->dp_spa))
		return (0);

	SET_BOOKMARK(&zb, zh->zh_log.blk_cksum.zc_word[ZIL_ZC_OBJSET],
	    ZB_ZIL_OBJECT, ZB_ZIL_LEVEL, bp->blk_cksum.zc_word[ZIL_ZC_SEQ]);

	VERIFY(0 == scan_funcs[scn->scn_phys.scn_func](dp, bp, &zb));
	return (0);
}

/* ARGSUSED */
static int
dsl_scan_zil_record(zilog_t *zilog, lr_t *lrc, void *arg, uint64_t claim_txg)
{
	if (lrc->lrc_txtype == TX_WRITE) {
		zil_scan_arg_t *zsa = arg;
		dsl_pool_t *dp = zsa->zsa_dp;
		dsl_scan_t *scn = dp->dp_scan;
		zil_header_t *zh = zsa->zsa_zh;
		lr_write_t *lr = (lr_write_t *)lrc;
		blkptr_t *bp = &lr->lr_blkptr;
		zbookmark_t zb;

		if (bp->blk_birth <= scn->scn_phys.scn_cur_min_txg)
			return (0);

		/*
		 * birth can be < claim_txg if this record's txg is
		 * already txg sync'ed (but this log block contains
		 * other records that are not synced)
		 */
		if (claim_txg == 0 || bp->blk_birth < claim_txg)
			return (0);

		SET_BOOKMARK(&zb, zh->zh_log.blk_cksum.zc_word[ZIL_ZC_OBJSET],
		    lr->lr_foid, ZB_ZIL_LEVEL,
		    lr->lr_offset / BP_GET_LSIZE(bp));

		VERIFY(0 == scan_funcs[scn->scn_phys.scn_func](dp, bp, &zb));
	}
	return (0);
}

static void
dsl_scan_zil(dsl_pool_t *dp, zil_header_t *zh)
{
	uint64_t claim_txg = zh->zh_claim_txg;
	zil_scan_arg_t zsa = { dp, zh };
	zilog_t *zilog;

	/*
	 * We only want to visit blocks that have been claimed but not yet
	 * replayed (or, in read-only mode, blocks that *would* be claimed).
	 */
	if (claim_txg == 0 && spa_writeable(dp->dp_spa))
		return;

	zilog = zil_alloc(dp->dp_meta_objset, zh);

	(void) zil_parse(zilog, dsl_scan_zil_block, dsl_scan_zil_record, &zsa,
	    claim_txg);

	zil_free(zilog);
}

/* ARGSUSED */
static void
dsl_scan_prefetch(dsl_scan_t *scn, arc_buf_t *buf, blkptr_t *bp,
    uint64_t objset, uint64_t object, uint64_t blkid)
{
	zbookmark_t czb;
	uint32_t flags = ARC_NOWAIT | ARC_PREFETCH;

	if (zfs_no_scrub_prefetch)
		return;

	if (BP_IS_HOLE(bp) || bp->blk_birth <= scn->scn_phys.scn_min_txg ||
	    (BP_GET_LEVEL(bp) == 0 && BP_GET_TYPE(bp) != DMU_OT_DNODE))
		return;

	SET_BOOKMARK(&czb, objset, object, BP_GET_LEVEL(bp), blkid);

	/*
	 * XXX need to make sure all of these arc_read() prefetches are
	 * done before setting xlateall (similar to dsl_read())
	 */
	(void) arc_read(scn->scn_zio_root, scn->scn_dp->dp_spa, bp,
	    buf, NULL, NULL, ZIO_PRIORITY_ASYNC_READ, ZIO_FLAG_CANFAIL,
	    &flags, &czb);
}

static boolean_t
dsl_scan_check_resume(dsl_scan_t *scn, const dnode_phys_t *dnp,
    const zbookmark_t *zb)
{
	/*
	 * We never skip over user/group accounting objects (obj<0)
	 */
	if (!bookmark_is_zero(&scn->scn_phys.scn_bookmark) &&
	    (int64_t)zb->zb_object >= 0) {
		/*
		 * If we already visited this bp & everything below (in
		 * a prior txg sync), don't bother doing it again.
		 */
		if (bookmark_is_before(dnp, zb, &scn->scn_phys.scn_bookmark))
			return (B_TRUE);

		/*
		 * If we found the block we're trying to resume from, or
		 * we went past it to a different object, zero it out to
		 * indicate that it's OK to start checking for pausing
		 * again.
		 */
		if (bcmp(zb, &scn->scn_phys.scn_bookmark, sizeof (*zb)) == 0 ||
		    zb->zb_object > scn->scn_phys.scn_bookmark.zb_object) {
			dprintf("resuming at %llx/%llx/%llx/%llx\n",
			    (longlong_t)zb->zb_objset,
			    (longlong_t)zb->zb_object,
			    (longlong_t)zb->zb_level,
			    (longlong_t)zb->zb_blkid);
			bzero(&scn->scn_phys.scn_bookmark, sizeof (*zb));
		}
	}
	return (B_FALSE);
}

/*
 * Return nonzero on i/o error.
 * Return new buf to write out in *bufp.
 */
static int
dsl_scan_recurse(dsl_scan_t *scn, dsl_dataset_t *ds, dmu_objset_type_t ostype,
    dnode_phys_t *dnp, const blkptr_t *bp,
    const zbookmark_t *zb, dmu_tx_t *tx, arc_buf_t **bufp)
{
	dsl_pool_t *dp = scn->scn_dp;
	int err;

	if (BP_GET_LEVEL(bp) > 0) {
		uint32_t flags = ARC_WAIT;
		int i;
		blkptr_t *cbp;
		int epb = BP_GET_LSIZE(bp) >> SPA_BLKPTRSHIFT;

		err = arc_read_nolock(NULL, dp->dp_spa, bp,
		    arc_getbuf_func, bufp,
		    ZIO_PRIORITY_ASYNC_READ, ZIO_FLAG_CANFAIL, &flags, zb);
		if (err) {
			scn->scn_phys.scn_errors++;
			return (err);
		}
		for (i = 0, cbp = (*bufp)->b_data; i < epb; i++, cbp++) {
			dsl_scan_prefetch(scn, *bufp, cbp, zb->zb_objset,
			    zb->zb_object, zb->zb_blkid * epb + i);
		}
		for (i = 0, cbp = (*bufp)->b_data; i < epb; i++, cbp++) {
			zbookmark_t czb;

			SET_BOOKMARK(&czb, zb->zb_objset, zb->zb_object,
			    zb->zb_level - 1,
			    zb->zb_blkid * epb + i);
			dsl_scan_visitbp(cbp, &czb, dnp,
			    *bufp, ds, scn, ostype, tx);
		}
	} else if (BP_GET_TYPE(bp) == DMU_OT_USERGROUP_USED) {
		uint32_t flags = ARC_WAIT;

		err = arc_read_nolock(NULL, dp->dp_spa, bp,
		    arc_getbuf_func, bufp,
		    ZIO_PRIORITY_ASYNC_READ, ZIO_FLAG_CANFAIL, &flags, zb);
		if (err) {
			scn->scn_phys.scn_errors++;
			return (err);
		}
	} else if (BP_GET_TYPE(bp) == DMU_OT_DNODE) {
		uint32_t flags = ARC_WAIT;
		dnode_phys_t *cdnp;
		int i, j;
		int epb = BP_GET_LSIZE(bp) >> DNODE_SHIFT;

		err = arc_read_nolock(NULL, dp->dp_spa, bp,
		    arc_getbuf_func, bufp,
		    ZIO_PRIORITY_ASYNC_READ, ZIO_FLAG_CANFAIL, &flags, zb);
		if (err) {
			scn->scn_phys.scn_errors++;
			return (err);
		}
		for (i = 0, cdnp = (*bufp)->b_data; i < epb; i++, cdnp++) {
			for (j = 0; j < cdnp->dn_nblkptr; j++) {
				blkptr_t *cbp = &cdnp->dn_blkptr[j];
				dsl_scan_prefetch(scn, *bufp, cbp,
				    zb->zb_objset, zb->zb_blkid * epb + i, j);
			}
		}
		for (i = 0, cdnp = (*bufp)->b_data; i < epb; i++, cdnp++) {
			dsl_scan_visitdnode(scn, ds, ostype,
			    cdnp, *bufp, zb->zb_blkid * epb + i, tx);
		}

	} else if (BP_GET_TYPE(bp) == DMU_OT_OBJSET) {
		uint32_t flags = ARC_WAIT;
		objset_phys_t *osp;

		err = arc_read_nolock(NULL, dp->dp_spa, bp,
		    arc_getbuf_func, bufp,
		    ZIO_PRIORITY_ASYNC_READ, ZIO_FLAG_CANFAIL, &flags, zb);
		if (err) {
			scn->scn_phys.scn_errors++;
			return (err);
		}

		osp = (*bufp)->b_data;

		if (DSL_SCAN_IS_SCRUB_RESILVER(scn))
			dsl_scan_zil(dp, &osp->os_zil_header);

		dsl_scan_visitdnode(scn, ds, osp->os_type,
		    &osp->os_meta_dnode, *bufp, DMU_META_DNODE_OBJECT, tx);

		if (OBJSET_BUF_HAS_USERUSED(*bufp)) {
			/*
			 * We also always visit user/group accounting
			 * objects, and never skip them, even if we are
			 * pausing.  This is necessary so that the space
			 * deltas from this txg get integrated.
			 */
			dsl_scan_visitdnode(scn, ds, osp->os_type,
			    &osp->os_groupused_dnode, *bufp,
			    DMU_GROUPUSED_OBJECT, tx);
			dsl_scan_visitdnode(scn, ds, osp->os_type,
			    &osp->os_userused_dnode, *bufp,
			    DMU_USERUSED_OBJECT, tx);
		}
	}

	return (0);
}

static void
dsl_scan_visitdnode(dsl_scan_t *scn, dsl_dataset_t *ds,
    dmu_objset_type_t ostype, dnode_phys_t *dnp, arc_buf_t *buf,
    uint64_t object, dmu_tx_t *tx)
{
	int j;

	for (j = 0; j < dnp->dn_nblkptr; j++) {
		zbookmark_t czb;

		SET_BOOKMARK(&czb, ds ? ds->ds_object : 0, object,
		    dnp->dn_nlevels - 1, j);
		dsl_scan_visitbp(&dnp->dn_blkptr[j],
		    &czb, dnp, buf, ds, scn, ostype, tx);
	}

	if (dnp->dn_flags & DNODE_FLAG_SPILL_BLKPTR) {
		zbookmark_t czb;
		SET_BOOKMARK(&czb, ds ? ds->ds_object : 0, object,
		    0, DMU_SPILL_BLKID);
		dsl_scan_visitbp(&dnp->dn_spill,
		    &czb, dnp, buf, ds, scn, ostype, tx);
	}
}

/*
 * The arguments are in this order because mdb can only print the
 * first 5; we want them to be useful.
 */
static void
dsl_scan_visitbp(blkptr_t *bp, const zbookmark_t *zb,
    dnode_phys_t *dnp, arc_buf_t *pbuf,
    dsl_dataset_t *ds, dsl_scan_t *scn, dmu_objset_type_t ostype,
    dmu_tx_t *tx)
{
	dsl_pool_t *dp = scn->scn_dp;
	arc_buf_t *buf = NULL;
	blkptr_t bp_toread = *bp;

	/* ASSERT(pbuf == NULL || arc_released(pbuf)); */

	if (dsl_scan_check_pause(scn, zb))
		return;

	if (dsl_scan_check_resume(scn, dnp, zb))
		return;

	if (bp->blk_birth == 0)
		return;

	scn->scn_visited_this_txg++;

	dprintf_bp(bp,
	    "visiting ds=%p/%llu zb=%llx/%llx/%llx/%llx buf=%p bp=%p",
	    ds, ds ? ds->ds_object : 0,
	    zb->zb_objset, zb->zb_object, zb->zb_level, zb->zb_blkid,
	    pbuf, bp);

	if (bp->blk_birth <= scn->scn_phys.scn_cur_min_txg)
		return;

	if (BP_GET_TYPE(bp) != DMU_OT_USERGROUP_USED) {
		/*
		 * For non-user-accounting blocks, we need to read the
		 * new bp (from a deleted snapshot, found in
		 * check_existing_xlation).  If we used the old bp,
		 * pointers inside this block from before we resumed
		 * would be untranslated.
		 *
		 * For user-accounting blocks, we need to read the old
		 * bp, because we will apply the entire space delta to
		 * it (original untranslated -> translations from
		 * deleted snap -> now).
		 */
		bp_toread = *bp;
	}

	if (dsl_scan_recurse(scn, ds, ostype, dnp, &bp_toread, zb, tx,
	    &buf) != 0)
		return;

	/*
	 * If dsl_scan_ddt() has aready visited this block, it will have
	 * already done any translations or scrubbing, so don't call the
	 * callback again.
	 */
	if (ddt_class_contains(dp->dp_spa,
	    scn->scn_phys.scn_ddt_class_max, bp)) {
		ASSERT(buf == NULL);
		return;
	}

	/*
	 * If this block is from the future (after cur_max_txg), then we
	 * are doing this on behalf of a deleted snapshot, and we will
	 * revisit the future block on the next pass of this dataset.
	 * Don't scan it now unless we need to because something
	 * under it was modified.
	 */
	if (bp->blk_birth <= scn->scn_phys.scn_cur_max_txg) {
		scan_funcs[scn->scn_phys.scn_func](dp, bp, zb);
	}
	if (buf)
		(void) arc_buf_remove_ref(buf, &buf);
}

static void
dsl_scan_visit_rootbp(dsl_scan_t *scn, dsl_dataset_t *ds, blkptr_t *bp,
    dmu_tx_t *tx)
{
	zbookmark_t zb;

	SET_BOOKMARK(&zb, ds ? ds->ds_object : DMU_META_OBJSET,
	    ZB_ROOT_OBJECT, ZB_ROOT_LEVEL, ZB_ROOT_BLKID);
	dsl_scan_visitbp(bp, &zb, NULL, NULL,
	    ds, scn, DMU_OST_NONE, tx);

	dprintf_ds(ds, "finished scan%s", "");
}

void
dsl_scan_ds_destroyed(dsl_dataset_t *ds, dmu_tx_t *tx)
{
	dsl_pool_t *dp = ds->ds_dir->dd_pool;
	dsl_scan_t *scn = dp->dp_scan;
	uint64_t mintxg;

	if (scn->scn_phys.scn_state != DSS_SCANNING)
		return;

	if (scn->scn_phys.scn_bookmark.zb_objset == ds->ds_object) {
		if (dsl_dataset_is_snapshot(ds)) {
			/* Note, scn_cur_{min,max}_txg stays the same. */
			scn->scn_phys.scn_bookmark.zb_objset =
			    ds->ds_phys->ds_next_snap_obj;
			zfs_dbgmsg("destroying ds %llu; currently traversing; "
			    "reset zb_objset to %llu",
			    (u_longlong_t)ds->ds_object,
			    (u_longlong_t)ds->ds_phys->ds_next_snap_obj);
			scn->scn_phys.scn_flags |= DSF_VISIT_DS_AGAIN;
		} else {
			SET_BOOKMARK(&scn->scn_phys.scn_bookmark,
			    ZB_DESTROYED_OBJSET, 0, 0, 0);
			zfs_dbgmsg("destroying ds %llu; currently traversing; "
			    "reset bookmark to -1,0,0,0",
			    (u_longlong_t)ds->ds_object);
		}
	} else if (zap_lookup_int_key(dp->dp_meta_objset,
	    scn->scn_phys.scn_queue_obj, ds->ds_object, &mintxg) == 0) {
		ASSERT3U(ds->ds_phys->ds_num_children, <=, 1);
		VERIFY3U(0, ==, zap_remove_int(dp->dp_meta_objset,
		    scn->scn_phys.scn_queue_obj, ds->ds_object, tx));
		if (dsl_dataset_is_snapshot(ds)) {
			/*
			 * We keep the same mintxg; it could be >
			 * ds_creation_txg if the previous snapshot was
			 * deleted too.
			 */
			VERIFY(zap_add_int_key(dp->dp_meta_objset,
			    scn->scn_phys.scn_queue_obj,
			    ds->ds_phys->ds_next_snap_obj, mintxg, tx) == 0);
			zfs_dbgmsg("destroying ds %llu; in queue; "
			    "replacing with %llu",
			    (u_longlong_t)ds->ds_object,
			    (u_longlong_t)ds->ds_phys->ds_next_snap_obj);
		} else {
			zfs_dbgmsg("destroying ds %llu; in queue; removing",
			    (u_longlong_t)ds->ds_object);
		}
	} else {
		zfs_dbgmsg("destroying ds %llu; ignoring",
		    (u_longlong_t)ds->ds_object);
	}

	/*
	 * dsl_scan_sync() should be called after this, and should sync
	 * out our changed state, but just to be safe, do it here.
	 */
	dsl_scan_sync_state(scn, tx);
}

void
dsl_scan_ds_snapshotted(dsl_dataset_t *ds, dmu_tx_t *tx)
{
	dsl_pool_t *dp = ds->ds_dir->dd_pool;
	dsl_scan_t *scn = dp->dp_scan;
	uint64_t mintxg;

	if (scn->scn_phys.scn_state != DSS_SCANNING)
		return;

	ASSERT(ds->ds_phys->ds_prev_snap_obj != 0);

	if (scn->scn_phys.scn_bookmark.zb_objset == ds->ds_object) {
		scn->scn_phys.scn_bookmark.zb_objset =
		    ds->ds_phys->ds_prev_snap_obj;
		zfs_dbgmsg("snapshotting ds %llu; currently traversing; "
		    "reset zb_objset to %llu",
		    (u_longlong_t)ds->ds_object,
		    (u_longlong_t)ds->ds_phys->ds_prev_snap_obj);
	} else if (zap_lookup_int_key(dp->dp_meta_objset,
	    scn->scn_phys.scn_queue_obj, ds->ds_object, &mintxg) == 0) {
		VERIFY3U(0, ==, zap_remove_int(dp->dp_meta_objset,
		    scn->scn_phys.scn_queue_obj, ds->ds_object, tx));
		VERIFY(zap_add_int_key(dp->dp_meta_objset,
		    scn->scn_phys.scn_queue_obj,
		    ds->ds_phys->ds_prev_snap_obj, mintxg, tx) == 0);
		zfs_dbgmsg("snapshotting ds %llu; in queue; "
		    "replacing with %llu",
		    (u_longlong_t)ds->ds_object,
		    (u_longlong_t)ds->ds_phys->ds_prev_snap_obj);
	}
	dsl_scan_sync_state(scn, tx);
}

void
dsl_scan_ds_clone_swapped(dsl_dataset_t *ds1, dsl_dataset_t *ds2, dmu_tx_t *tx)
{
	dsl_pool_t *dp = ds1->ds_dir->dd_pool;
	dsl_scan_t *scn = dp->dp_scan;
	uint64_t mintxg;

	if (scn->scn_phys.scn_state != DSS_SCANNING)
		return;

	if (scn->scn_phys.scn_bookmark.zb_objset == ds1->ds_object) {
		scn->scn_phys.scn_bookmark.zb_objset = ds2->ds_object;
		zfs_dbgmsg("clone_swap ds %llu; currently traversing; "
		    "reset zb_objset to %llu",
		    (u_longlong_t)ds1->ds_object,
		    (u_longlong_t)ds2->ds_object);
	} else if (scn->scn_phys.scn_bookmark.zb_objset == ds2->ds_object) {
		scn->scn_phys.scn_bookmark.zb_objset = ds1->ds_object;
		zfs_dbgmsg("clone_swap ds %llu; currently traversing; "
		    "reset zb_objset to %llu",
		    (u_longlong_t)ds2->ds_object,
		    (u_longlong_t)ds1->ds_object);
	}

	if (zap_lookup_int_key(dp->dp_meta_objset, scn->scn_phys.scn_queue_obj,
	    ds1->ds_object, &mintxg) == 0) {
		int err;

		ASSERT3U(mintxg, ==, ds1->ds_phys->ds_prev_snap_txg);
		ASSERT3U(mintxg, ==, ds2->ds_phys->ds_prev_snap_txg);
		VERIFY3U(0, ==, zap_remove_int(dp->dp_meta_objset,
		    scn->scn_phys.scn_queue_obj, ds1->ds_object, tx));
		err = zap_add_int_key(dp->dp_meta_objset,
		    scn->scn_phys.scn_queue_obj, ds2->ds_object, mintxg, tx);
		VERIFY(err == 0 || err == EEXIST);
		if (err == EEXIST) {
			/* Both were there to begin with */
			VERIFY(0 == zap_add_int_key(dp->dp_meta_objset,
			    scn->scn_phys.scn_queue_obj,
			    ds1->ds_object, mintxg, tx));
		}
		zfs_dbgmsg("clone_swap ds %llu; in queue; "
		    "replacing with %llu",
		    (u_longlong_t)ds1->ds_object,
		    (u_longlong_t)ds2->ds_object);
	} else if (zap_lookup_int_key(dp->dp_meta_objset,
	    scn->scn_phys.scn_queue_obj, ds2->ds_object, &mintxg) == 0) {
		ASSERT3U(mintxg, ==, ds1->ds_phys->ds_prev_snap_txg);
		ASSERT3U(mintxg, ==, ds2->ds_phys->ds_prev_snap_txg);
		VERIFY3U(0, ==, zap_remove_int(dp->dp_meta_objset,
		    scn->scn_phys.scn_queue_obj, ds2->ds_object, tx));
		VERIFY(0 == zap_add_int_key(dp->dp_meta_objset,
		    scn->scn_phys.scn_queue_obj, ds1->ds_object, mintxg, tx));
		zfs_dbgmsg("clone_swap ds %llu; in queue; "
		    "replacing with %llu",
		    (u_longlong_t)ds2->ds_object,
		    (u_longlong_t)ds1->ds_object);
	}

	dsl_scan_sync_state(scn, tx);
}

struct enqueue_clones_arg {
	dmu_tx_t *tx;
	uint64_t originobj;
};

/* ARGSUSED */
static int
enqueue_clones_cb(spa_t *spa, uint64_t dsobj, const char *dsname, void *arg)
{
	struct enqueue_clones_arg *eca = arg;
	dsl_dataset_t *ds;
	int err;
	dsl_pool_t *dp = spa->spa_dsl_pool;
	dsl_scan_t *scn = dp->dp_scan;

	err = dsl_dataset_hold_obj(dp, dsobj, FTAG, &ds);
	if (err)
		return (err);

	if (ds->ds_dir->dd_phys->dd_origin_obj == eca->originobj) {
		while (ds->ds_phys->ds_prev_snap_obj != eca->originobj) {
			dsl_dataset_t *prev;
			err = dsl_dataset_hold_obj(dp,
			    ds->ds_phys->ds_prev_snap_obj, FTAG, &prev);

			dsl_dataset_rele(ds, FTAG);
			if (err)
				return (err);
			ds = prev;
		}
		VERIFY(zap_add_int_key(dp->dp_meta_objset,
		    scn->scn_phys.scn_queue_obj, ds->ds_object,
		    ds->ds_phys->ds_prev_snap_txg, eca->tx) == 0);
	}
	dsl_dataset_rele(ds, FTAG);
	return (0);
}

static void
dsl_scan_visitds(dsl_scan_t *scn, uint64_t dsobj, dmu_tx_t *tx)
{
	dsl_pool_t *dp = scn->scn_dp;
	dsl_dataset_t *ds;

	VERIFY3U(0, ==, dsl_dataset_hold_obj(dp, dsobj, FTAG, &ds));

	/*
	 * Iterate over the bps in this ds.
	 */
	dmu_buf_will_dirty(ds->ds_dbuf, tx);
	dsl_scan_visit_rootbp(scn, ds, &ds->ds_phys->ds_bp, tx);

	char *dsname = kmem_alloc(ZFS_MAXNAMELEN, KM_SLEEP);
	dsl_dataset_name(ds, dsname);
	zfs_dbgmsg("scanned dataset %llu (%s) with min=%llu max=%llu; "
	    "pausing=%u",
	    (longlong_t)dsobj, dsname,
	    (longlong_t)scn->scn_phys.scn_cur_min_txg,
	    (longlong_t)scn->scn_phys.scn_cur_max_txg,
	    (int)scn->scn_pausing);
	kmem_free(dsname, ZFS_MAXNAMELEN);

	if (scn->scn_pausing)
		goto out;

	/*
	 * We've finished this pass over this dataset.
	 */

	/*
	 * If we did not completely visit this dataset, do another pass.
	 */
	if (scn->scn_phys.scn_flags & DSF_VISIT_DS_AGAIN) {
		zfs_dbgmsg("incomplete pass; visiting again");
		scn->scn_phys.scn_flags &= ~DSF_VISIT_DS_AGAIN;
		VERIFY(zap_add_int_key(dp->dp_meta_objset,
		    scn->scn_phys.scn_queue_obj, ds->ds_object,
		    scn->scn_phys.scn_cur_max_txg, tx) == 0);
		goto out;
	}

	/*
	 * Add descendent datasets to work queue.
	 */
	if (ds->ds_phys->ds_next_snap_obj != 0) {
		VERIFY(zap_add_int_key(dp->dp_meta_objset,
		    scn->scn_phys.scn_queue_obj, ds->ds_phys->ds_next_snap_obj,
		    ds->ds_phys->ds_creation_txg, tx) == 0);
	}
	if (ds->ds_phys->ds_num_children > 1) {
		boolean_t usenext = B_FALSE;
		if (ds->ds_phys->ds_next_clones_obj != 0) {
			uint64_t count;
			/*
			 * A bug in a previous version of the code could
			 * cause upgrade_clones_cb() to not set
			 * ds_next_snap_obj when it should, leading to a
			 * missing entry.  Therefore we can only use the
			 * next_clones_obj when its count is correct.
			 */
			int err = zap_count(dp->dp_meta_objset,
			    ds->ds_phys->ds_next_clones_obj, &count);
			if (err == 0 &&
			    count == ds->ds_phys->ds_num_children - 1)
				usenext = B_TRUE;
		}

		if (usenext) {
			VERIFY(zap_join_key(dp->dp_meta_objset,
			    ds->ds_phys->ds_next_clones_obj,
			    scn->scn_phys.scn_queue_obj,
			    ds->ds_phys->ds_creation_txg, tx) == 0);
		} else {
			struct enqueue_clones_arg eca;
			eca.tx = tx;
			eca.originobj = ds->ds_object;

			(void) dmu_objset_find_spa(ds->ds_dir->dd_pool->dp_spa,
			    NULL, enqueue_clones_cb, &eca, DS_FIND_CHILDREN);
		}
	}

out:
	dsl_dataset_rele(ds, FTAG);
}

/* ARGSUSED */
static int
enqueue_cb(spa_t *spa, uint64_t dsobj, const char *dsname, void *arg)
{
	dmu_tx_t *tx = arg;
	dsl_dataset_t *ds;
	int err;
	dsl_pool_t *dp = spa->spa_dsl_pool;
	dsl_scan_t *scn = dp->dp_scan;

	err = dsl_dataset_hold_obj(dp, dsobj, FTAG, &ds);
	if (err)
		return (err);

	while (ds->ds_phys->ds_prev_snap_obj != 0) {
		dsl_dataset_t *prev;
		err = dsl_dataset_hold_obj(dp, ds->ds_phys->ds_prev_snap_obj,
		    FTAG, &prev);
		if (err) {
			dsl_dataset_rele(ds, FTAG);
			return (err);
		}

		/*
		 * If this is a clone, we don't need to worry about it for now.
		 */
		if (prev->ds_phys->ds_next_snap_obj != ds->ds_object) {
			dsl_dataset_rele(ds, FTAG);
			dsl_dataset_rele(prev, FTAG);
			return (0);
		}
		dsl_dataset_rele(ds, FTAG);
		ds = prev;
	}

	VERIFY(zap_add_int_key(dp->dp_meta_objset, scn->scn_phys.scn_queue_obj,
	    ds->ds_object, ds->ds_phys->ds_prev_snap_txg, tx) == 0);
	dsl_dataset_rele(ds, FTAG);
	return (0);
}

/*
 * Scrub/dedup interaction.
 *
 * If there are N references to a deduped block, we don't want to scrub it
 * N times -- ideally, we should scrub it exactly once.
 *
 * We leverage the fact that the dde's replication class (enum ddt_class)
 * is ordered from highest replication class (DDT_CLASS_DITTO) to lowest
 * (DDT_CLASS_UNIQUE) so that we may walk the DDT in that order.
 *
 * To prevent excess scrubbing, the scrub begins by walking the DDT
 * to find all blocks with refcnt > 1, and scrubs each of these once.
 * Since there are two replication classes which contain blocks with
 * refcnt > 1, we scrub the highest replication class (DDT_CLASS_DITTO) first.
 * Finally the top-down scrub begins, only visiting blocks with refcnt == 1.
 *
 * There would be nothing more to say if a block's refcnt couldn't change
 * during a scrub, but of course it can so we must account for changes
 * in a block's replication class.
 *
 * Here's an example of what can occur:
 *
 * If a block has refcnt > 1 during the DDT scrub phase, but has refcnt == 1
 * when visited during the top-down scrub phase, it will be scrubbed twice.
 * This negates our scrub optimization, but is otherwise harmless.
 *
 * If a block has refcnt == 1 during the DDT scrub phase, but has refcnt > 1
 * on each visit during the top-down scrub phase, it will never be scrubbed.
 * To catch this, ddt_sync_entry() notifies the scrub code whenever a block's
 * reference class transitions to a higher level (i.e DDT_CLASS_UNIQUE to
 * DDT_CLASS_DUPLICATE); if it transitions from refcnt == 1 to refcnt > 1
 * while a scrub is in progress, it scrubs the block right then.
 */
static void
dsl_scan_ddt(dsl_scan_t *scn, dmu_tx_t *tx)
{
	ddt_bookmark_t *ddb = &scn->scn_phys.scn_ddt_bookmark;
	ddt_entry_t dde = { 0 };
	int error;
	uint64_t n = 0;

	while ((error = ddt_walk(scn->scn_dp->dp_spa, ddb, &dde)) == 0) {
		ddt_t *ddt;

		if (ddb->ddb_class > scn->scn_phys.scn_ddt_class_max)
			break;
		dprintf("visiting ddb=%llu/%llu/%llu/%llx\n",
		    (longlong_t)ddb->ddb_class,
		    (longlong_t)ddb->ddb_type,
		    (longlong_t)ddb->ddb_checksum,
		    (longlong_t)ddb->ddb_cursor);

		/* There should be no pending changes to the dedup table */
		ddt = scn->scn_dp->dp_spa->spa_ddt[ddb->ddb_checksum];
		ASSERT(avl_first(&ddt->ddt_tree) == NULL);

		dsl_scan_ddt_entry(scn, ddb->ddb_checksum, &dde, tx);
		n++;

		if (dsl_scan_check_pause(scn, NULL))
			break;
	}

	zfs_dbgmsg("scanned %llu ddt entries with class_max = %u; pausing=%u",
	    (longlong_t)n, (int)scn->scn_phys.scn_ddt_class_max,
	    (int)scn->scn_pausing);

	ASSERT(error == 0 || error == ENOENT);
	ASSERT(error != ENOENT ||
	    ddb->ddb_class > scn->scn_phys.scn_ddt_class_max);
}

/* ARGSUSED */
void
dsl_scan_ddt_entry(dsl_scan_t *scn, enum zio_checksum checksum,
    ddt_entry_t *dde, dmu_tx_t *tx)
{
	const ddt_key_t *ddk = &dde->dde_key;
	ddt_phys_t *ddp = dde->dde_phys;
	blkptr_t bp;
	zbookmark_t zb = { 0 };
	int p;

	if (scn->scn_phys.scn_state != DSS_SCANNING)
		return;

	for (p = 0; p < DDT_PHYS_TYPES; p++, ddp++) {
		if (ddp->ddp_phys_birth == 0 ||
		    ddp->ddp_phys_birth > scn->scn_phys.scn_cur_max_txg)
			continue;
		ddt_bp_create(checksum, ddk, ddp, &bp);

		scn->scn_visited_this_txg++;
		scan_funcs[scn->scn_phys.scn_func](scn->scn_dp, &bp, &zb);
	}
}

static void
dsl_scan_visit(dsl_scan_t *scn, dmu_tx_t *tx)
{
	dsl_pool_t *dp = scn->scn_dp;
	zap_cursor_t zc;
	zap_attribute_t za;

	if (scn->scn_phys.scn_ddt_bookmark.ddb_class <=
	    scn->scn_phys.scn_ddt_class_max) {
		scn->scn_phys.scn_cur_min_txg = scn->scn_phys.scn_min_txg;
		scn->scn_phys.scn_cur_max_txg = scn->scn_phys.scn_max_txg;
		dsl_scan_ddt(scn, tx);
		if (scn->scn_pausing)
			return;
	}

	if (scn->scn_phys.scn_bookmark.zb_objset == DMU_META_OBJSET) {
		/* First do the MOS & ORIGIN */

		scn->scn_phys.scn_cur_min_txg = scn->scn_phys.scn_min_txg;
		scn->scn_phys.scn_cur_max_txg = scn->scn_phys.scn_max_txg;
		dsl_scan_visit_rootbp(scn, NULL,
		    &dp->dp_meta_rootbp, tx);
		spa_set_rootblkptr(dp->dp_spa, &dp->dp_meta_rootbp);
		if (scn->scn_pausing)
			return;

		if (spa_version(dp->dp_spa) < SPA_VERSION_DSL_SCRUB) {
			VERIFY(0 == dmu_objset_find_spa(dp->dp_spa,
			    NULL, enqueue_cb, tx, DS_FIND_CHILDREN));
		} else {
			dsl_scan_visitds(scn,
			    dp->dp_origin_snap->ds_object, tx);
		}
		ASSERT(!scn->scn_pausing);
	} else if (scn->scn_phys.scn_bookmark.zb_objset !=
	    ZB_DESTROYED_OBJSET) {
		/*
		 * If we were paused, continue from here.  Note if the
		 * ds we were paused on was deleted, the zb_objset may
		 * be -1, so we will skip this and find a new objset
		 * below.
		 */
		dsl_scan_visitds(scn, scn->scn_phys.scn_bookmark.zb_objset, tx);
		if (scn->scn_pausing)
			return;
	}

	/*
	 * In case we were paused right at the end of the ds, zero the
	 * bookmark so we don't think that we're still trying to resume.
	 */
	bzero(&scn->scn_phys.scn_bookmark, sizeof (zbookmark_t));

	/* keep pulling things out of the zap-object-as-queue */
	while (zap_cursor_init(&zc, dp->dp_meta_objset,
	    scn->scn_phys.scn_queue_obj),
	    zap_cursor_retrieve(&zc, &za) == 0) {
		dsl_dataset_t *ds;
		uint64_t dsobj;

		dsobj = strtonum(za.za_name, NULL);
		VERIFY3U(0, ==, zap_remove_int(dp->dp_meta_objset,
		    scn->scn_phys.scn_queue_obj, dsobj, tx));

		/* Set up min/max txg */
		VERIFY3U(0, ==, dsl_dataset_hold_obj(dp, dsobj, FTAG, &ds));
		if (za.za_first_integer != 0) {
			scn->scn_phys.scn_cur_min_txg =
			    MAX(scn->scn_phys.scn_min_txg,
			    za.za_first_integer);
		} else {
			scn->scn_phys.scn_cur_min_txg =
			    MAX(scn->scn_phys.scn_min_txg,
			    ds->ds_phys->ds_prev_snap_txg);
		}
		scn->scn_phys.scn_cur_max_txg = dsl_scan_ds_maxtxg(ds);
		dsl_dataset_rele(ds, FTAG);

		dsl_scan_visitds(scn, dsobj, tx);
		zap_cursor_fini(&zc);
		if (scn->scn_pausing)
			return;
	}
	zap_cursor_fini(&zc);
}

static int
dsl_scan_free_cb(void *arg, const blkptr_t *bp, dmu_tx_t *tx)
{
	dsl_scan_t *scn = arg;
	uint64_t elapsed_nanosecs;

	elapsed_nanosecs = gethrtime() - scn->scn_sync_start_time;

	if (elapsed_nanosecs / NANOSEC > zfs_txg_timeout ||
	    (elapsed_nanosecs / MICROSEC > zfs_free_min_time_ms &&
	    txg_sync_waiting(scn->scn_dp)) ||
	    spa_shutting_down(scn->scn_dp->dp_spa))
		return (ERESTART);

	zio_nowait(zio_free_sync(scn->scn_zio_root, scn->scn_dp->dp_spa,
	    dmu_tx_get_txg(tx), bp, 0));
	dsl_dir_diduse_space(tx->tx_pool->dp_free_dir, DD_USED_HEAD,
	    -bp_get_dsize_sync(scn->scn_dp->dp_spa, bp),
	    -BP_GET_PSIZE(bp), -BP_GET_UCSIZE(bp), tx);
	scn->scn_visited_this_txg++;
	return (0);
}

boolean_t
dsl_scan_active(dsl_scan_t *scn)
{
	spa_t *spa = scn->scn_dp->dp_spa;
	uint64_t used = 0, comp, uncomp;

	if (spa->spa_load_state != SPA_LOAD_NONE)
		return (B_FALSE);
	if (spa_shutting_down(spa))
		return (B_FALSE);

	if (scn->scn_phys.scn_state == DSS_SCANNING)
		return (B_TRUE);

	if (spa_version(scn->scn_dp->dp_spa) >= SPA_VERSION_DEADLISTS) {
		(void) bpobj_space(&scn->scn_dp->dp_free_bpobj,
		    &used, &comp, &uncomp);
	}
	return (used != 0);
}

void
dsl_scan_sync(dsl_pool_t *dp, dmu_tx_t *tx)
{
	dsl_scan_t *scn = dp->dp_scan;
	spa_t *spa = dp->dp_spa;
	int err;

	/*
	 * Check for scn_restart_txg before checking spa_load_state, so
	 * that we can restart an old-style scan while the pool is being
	 * imported (see dsl_scan_init).
	 */
	if (scn->scn_restart_txg != 0 &&
	    scn->scn_restart_txg <= tx->tx_txg) {
		pool_scan_func_t func = POOL_SCAN_SCRUB;
		dsl_scan_done(scn, B_FALSE, tx);
		if (vdev_resilver_needed(spa->spa_root_vdev, NULL, NULL))
			func = POOL_SCAN_RESILVER;
		zfs_dbgmsg("restarting scan func=%u txg=%llu",
		    func, tx->tx_txg);
		dsl_scan_setup_sync(scn, &func, tx);
	}


	if (!dsl_scan_active(scn) ||
	    spa_sync_pass(dp->dp_spa) > 1)
		return;

	scn->scn_visited_this_txg = 0;
	scn->scn_pausing = B_FALSE;
	scn->scn_sync_start_time = gethrtime();
	spa->spa_scrub_active = B_TRUE;

	/*
	 * First process the free list.  If we pause the free, don't do
	 * any scanning.  This ensures that there is no free list when
	 * we are scanning, so the scan code doesn't have to worry about
	 * traversing it.
	 */
	if (spa_version(dp->dp_spa) >= SPA_VERSION_DEADLISTS) {
		scn->scn_zio_root = zio_root(dp->dp_spa, NULL,
		    NULL, ZIO_FLAG_MUSTSUCCEED);
		err = bpobj_iterate(&dp->dp_free_bpobj,
		    dsl_scan_free_cb, scn, tx);
		VERIFY3U(0, ==, zio_wait(scn->scn_zio_root));
		if (scn->scn_visited_this_txg) {
			zfs_dbgmsg("freed %llu blocks in %llums from "
			    "free_bpobj txg %llu",
			    (longlong_t)scn->scn_visited_this_txg,
			    (longlong_t)
			    (gethrtime() - scn->scn_sync_start_time) / MICROSEC,
			    (longlong_t)tx->tx_txg);
			scn->scn_visited_this_txg = 0;
			/*
			 * Re-sync the ddt so that we can further modify
			 * it when doing bprewrite.
			 */
			ddt_sync(spa, tx->tx_txg);
		}
		if (err == ERESTART)
			return;
	}

	if (scn->scn_phys.scn_state != DSS_SCANNING)
		return;


	if (scn->scn_phys.scn_ddt_bookmark.ddb_class <=
	    scn->scn_phys.scn_ddt_class_max) {
		zfs_dbgmsg("doing scan sync txg %llu; "
		    "ddt bm=%llu/%llu/%llu/%llx",
		    (longlong_t)tx->tx_txg,
		    (longlong_t)scn->scn_phys.scn_ddt_bookmark.ddb_class,
		    (longlong_t)scn->scn_phys.scn_ddt_bookmark.ddb_type,
		    (longlong_t)scn->scn_phys.scn_ddt_bookmark.ddb_checksum,
		    (longlong_t)scn->scn_phys.scn_ddt_bookmark.ddb_cursor);
		ASSERT(scn->scn_phys.scn_bookmark.zb_objset == 0);
		ASSERT(scn->scn_phys.scn_bookmark.zb_object == 0);
		ASSERT(scn->scn_phys.scn_bookmark.zb_level == 0);
		ASSERT(scn->scn_phys.scn_bookmark.zb_blkid == 0);
	} else {
		zfs_dbgmsg("doing scan sync txg %llu; bm=%llu/%llu/%llu/%llu",
		    (longlong_t)tx->tx_txg,
		    (longlong_t)scn->scn_phys.scn_bookmark.zb_objset,
		    (longlong_t)scn->scn_phys.scn_bookmark.zb_object,
		    (longlong_t)scn->scn_phys.scn_bookmark.zb_level,
		    (longlong_t)scn->scn_phys.scn_bookmark.zb_blkid);
	}

	scn->scn_zio_root = zio_root(dp->dp_spa, NULL,
	    NULL, ZIO_FLAG_CANFAIL);
	dsl_scan_visit(scn, tx);
	(void) zio_wait(scn->scn_zio_root);
	scn->scn_zio_root = NULL;

	zfs_dbgmsg("visited %llu blocks in %llums",
	    (longlong_t)scn->scn_visited_this_txg,
	    (longlong_t)(gethrtime() - scn->scn_sync_start_time) / MICROSEC);

	if (!scn->scn_pausing) {
		/* finished with scan. */
		zfs_dbgmsg("finished scan txg %llu", (longlong_t)tx->tx_txg);
		dsl_scan_done(scn, B_TRUE, tx);
	}

	if (DSL_SCAN_IS_SCRUB_RESILVER(scn)) {
		mutex_enter(&spa->spa_scrub_lock);
		while (spa->spa_scrub_inflight > 0) {
			cv_wait(&spa->spa_scrub_io_cv,
			    &spa->spa_scrub_lock);
		}
		mutex_exit(&spa->spa_scrub_lock);
	}

	dsl_scan_sync_state(scn, tx);
}

/*
 * This will start a new scan, or restart an existing one.
 */
void
dsl_resilver_restart(dsl_pool_t *dp, uint64_t txg)
{
	if (txg == 0) {
		dmu_tx_t *tx;
		tx = dmu_tx_create_dd(dp->dp_mos_dir);
		VERIFY(0 == dmu_tx_assign(tx, TXG_WAIT));

		txg = dmu_tx_get_txg(tx);
		dp->dp_scan->scn_restart_txg = txg;
		dmu_tx_commit(tx);
	} else {
		dp->dp_scan->scn_restart_txg = txg;
	}
	zfs_dbgmsg("restarting resilver txg=%llu", txg);
}

boolean_t
dsl_scan_resilvering(dsl_pool_t *dp)
{
	return (dp->dp_scan->scn_phys.scn_state == DSS_SCANNING &&
	    dp->dp_scan->scn_phys.scn_func == POOL_SCAN_RESILVER);
}

/*
 * scrub consumers
 */

static void
count_block(zfs_all_blkstats_t *zab, const blkptr_t *bp)
{
	int i;

	/*
	 * If we resume after a reboot, zab will be NULL; don't record
	 * incomplete stats in that case.
	 */
	if (zab == NULL)
		return;

	for (i = 0; i < 4; i++) {
		int l = (i < 2) ? BP_GET_LEVEL(bp) : DN_MAX_LEVELS;
		int t = (i & 1) ? BP_GET_TYPE(bp) : DMU_OT_TOTAL;
		zfs_blkstat_t *zb = &zab->zab_type[l][t];
		int equal;

		zb->zb_count++;
		zb->zb_asize += BP_GET_ASIZE(bp);
		zb->zb_lsize += BP_GET_LSIZE(bp);
		zb->zb_psize += BP_GET_PSIZE(bp);
		zb->zb_gangs += BP_COUNT_GANG(bp);

		switch (BP_GET_NDVAS(bp)) {
		case 2:
			if (DVA_GET_VDEV(&bp->blk_dva[0]) ==
			    DVA_GET_VDEV(&bp->blk_dva[1]))
				zb->zb_ditto_2_of_2_samevdev++;
			break;
		case 3:
			equal = (DVA_GET_VDEV(&bp->blk_dva[0]) ==
			    DVA_GET_VDEV(&bp->blk_dva[1])) +
			    (DVA_GET_VDEV(&bp->blk_dva[0]) ==
			    DVA_GET_VDEV(&bp->blk_dva[2])) +
			    (DVA_GET_VDEV(&bp->blk_dva[1]) ==
			    DVA_GET_VDEV(&bp->blk_dva[2]));
			if (equal == 1)
				zb->zb_ditto_2_of_3_samevdev++;
			else if (equal == 3)
				zb->zb_ditto_3_of_3_samevdev++;
			break;
		}
	}
}

static void
dsl_scan_scrub_done(zio_t *zio)
{
	spa_t *spa = zio->io_spa;

	zio_data_buf_free(zio->io_data, zio->io_size);

	mutex_enter(&spa->spa_scrub_lock);
	spa->spa_scrub_inflight--;
	cv_broadcast(&spa->spa_scrub_io_cv);

	if (zio->io_error && (zio->io_error != ECKSUM ||
	    !(zio->io_flags & ZIO_FLAG_SPECULATIVE))) {
		spa->spa_dsl_pool->dp_scan->scn_phys.scn_errors++;
	}
	mutex_exit(&spa->spa_scrub_lock);
}

static int
dsl_scan_scrub_cb(dsl_pool_t *dp,
    const blkptr_t *bp, const zbookmark_t *zb)
{
	dsl_scan_t *scn = dp->dp_scan;
	size_t size = BP_GET_PSIZE(bp);
	spa_t *spa = dp->dp_spa;
	uint64_t phys_birth = BP_PHYSICAL_BIRTH(bp);
	boolean_t needs_io;
	int zio_flags = ZIO_FLAG_SCRUB_THREAD | ZIO_FLAG_RAW | ZIO_FLAG_CANFAIL;
	int zio_priority;
	int d;

	if (phys_birth <= scn->scn_phys.scn_min_txg ||
	    phys_birth >= scn->scn_phys.scn_max_txg)
		return (0);

	count_block(dp->dp_blkstats, bp);

	ASSERT(DSL_SCAN_IS_SCRUB_RESILVER(scn));
	if (scn->scn_phys.scn_func == POOL_SCAN_SCRUB) {
		zio_flags |= ZIO_FLAG_SCRUB;
		zio_priority = ZIO_PRIORITY_SCRUB;
		needs_io = B_TRUE;
	} else if (scn->scn_phys.scn_func == POOL_SCAN_RESILVER) {
		zio_flags |= ZIO_FLAG_RESILVER;
		zio_priority = ZIO_PRIORITY_RESILVER;
		needs_io = B_FALSE;
	}

	/* If it's an intent log block, failure is expected. */
	if (zb->zb_level == ZB_ZIL_LEVEL)
		zio_flags |= ZIO_FLAG_SPECULATIVE;

	for (d = 0; d < BP_GET_NDVAS(bp); d++) {
		vdev_t *vd = vdev_lookup_top(spa,
		    DVA_GET_VDEV(&bp->blk_dva[d]));

		/*
		 * Keep track of how much data we've examined so that
		 * zpool(1M) status can make useful progress reports.
		 */
		scn->scn_phys.scn_examined += DVA_GET_ASIZE(&bp->blk_dva[d]);
		spa->spa_scan_pass_exam += DVA_GET_ASIZE(&bp->blk_dva[d]);

		/* if it's a resilver, this may not be in the target range */
		if (!needs_io) {
			if (DVA_GET_GANG(&bp->blk_dva[d])) {
				/*
				 * Gang members may be spread across multiple
				 * vdevs, so the best estimate we have is the
				 * scrub range, which has already been checked.
				 * XXX -- it would be better to change our
				 * allocation policy to ensure that all
				 * gang members reside on the same vdev.
				 */
				needs_io = B_TRUE;
			} else {
				needs_io = vdev_dtl_contains(vd, DTL_PARTIAL,
				    phys_birth, 1);
			}
		}
	}

	if (needs_io && !zfs_no_scrub_io) {
		void *data = zio_data_buf_alloc(size);

		mutex_enter(&spa->spa_scrub_lock);
		while (spa->spa_scrub_inflight >= spa->spa_scrub_maxinflight)
			cv_wait(&spa->spa_scrub_io_cv, &spa->spa_scrub_lock);
		spa->spa_scrub_inflight++;
		mutex_exit(&spa->spa_scrub_lock);

		zio_nowait(zio_read(NULL, spa, bp, data, size,
		    dsl_scan_scrub_done, NULL, zio_priority,
		    zio_flags, zb));
	}

	/* do not relocate this block */
	return (0);
}

int
dsl_scan(dsl_pool_t *dp, pool_scan_func_t func)
{
	spa_t *spa = dp->dp_spa;

	/*
	 * Purge all vdev caches and probe all devices.  We do this here
	 * rather than in sync context because this requires a writer lock
	 * on the spa_config lock, which we can't do from sync context.  The
	 * spa_scrub_reopen flag indicates that vdev_open() should not
	 * attempt to start another scrub.
	 */
	spa_vdev_state_enter(spa, SCL_NONE);
	spa->spa_scrub_reopen = B_TRUE;
	vdev_reopen(spa->spa_root_vdev);
	spa->spa_scrub_reopen = B_FALSE;
	(void) spa_vdev_state_exit(spa, NULL, 0);

	return (dsl_sync_task_do(dp, dsl_scan_setup_check,
	    dsl_scan_setup_sync, dp->dp_scan, &func, 0));
}