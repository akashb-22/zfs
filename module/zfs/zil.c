// SPDX-License-Identifier: CDDL-1.0
/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or https://opensource.org/licenses/CDDL-1.0.
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
 * Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2011, 2018 by Delphix. All rights reserved.
 * Copyright (c) 2014 Integros [integros.com]
 * Copyright (c) 2018 Datto Inc.
 */

/* Portions Copyright 2010 Robert Milkowski */

#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/dmu.h>
#include <sys/zap.h>
#include <sys/arc.h>
#include <sys/stat.h>
#include <sys/zil.h>
#include <sys/zil_impl.h>
#include <sys/dsl_dataset.h>
#include <sys/vdev_impl.h>
#include <sys/dmu_tx.h>
#include <sys/dsl_pool.h>
#include <sys/metaslab.h>
#include <sys/trace_zfs.h>
#include <sys/abd.h>
#include <sys/brt.h>
#include <sys/wmsum.h>

/*
 * The ZFS Intent Log (ZIL) saves "transaction records" (itxs) of system
 * calls that change the file system. Each itx has enough information to
 * be able to replay them after a system crash, power loss, or
 * equivalent failure mode. These are stored in memory until either:
 *
 *   1. they are committed to the pool by the DMU transaction group
 *      (txg), at which point they can be discarded; or
 *   2. they are committed to the on-disk ZIL for the dataset being
 *      modified (e.g. due to an fsync, O_DSYNC, or other synchronous
 *      requirement).
 *
 * In the event of a crash or power loss, the itxs contained by each
 * dataset's on-disk ZIL will be replayed when that dataset is first
 * instantiated (e.g. if the dataset is a normal filesystem, when it is
 * first mounted).
 *
 * As hinted at above, there is one ZIL per dataset (both the in-memory
 * representation, and the on-disk representation). The on-disk format
 * consists of 3 parts:
 *
 * 	- a single, per-dataset, ZIL header; which points to a chain of
 * 	- zero or more ZIL blocks; each of which contains
 * 	- zero or more ZIL records
 *
 * A ZIL record holds the information necessary to replay a single
 * system call transaction. A ZIL block can hold many ZIL records, and
 * the blocks are chained together, similarly to a singly linked list.
 *
 * Each ZIL block contains a block pointer (blkptr_t) to the next ZIL
 * block in the chain, and the ZIL header points to the first block in
 * the chain.
 *
 * Note, there is not a fixed place in the pool to hold these ZIL
 * blocks; they are dynamically allocated and freed as needed from the
 * blocks available on the pool, though they can be preferentially
 * allocated from a dedicated "log" vdev.
 */

/*
 * This controls the amount of time that a ZIL block (lwb) will remain
 * "open" when it isn't "full", and it has a thread waiting for it to be
 * committed to stable storage. Please refer to the zil_commit_waiter()
 * function (and the comments within it) for more details.
 */
static uint_t zfs_commit_timeout_pct = 10;

/*
 * See zil.h for more information about these fields.
 */
static zil_kstat_values_t zil_stats = {
	{ "zil_commit_count",			KSTAT_DATA_UINT64 },
	{ "zil_commit_writer_count",		KSTAT_DATA_UINT64 },
	{ "zil_commit_error_count",		KSTAT_DATA_UINT64 },
	{ "zil_commit_stall_count",		KSTAT_DATA_UINT64 },
	{ "zil_commit_suspend_count",		KSTAT_DATA_UINT64 },
	{ "zil_itx_count",			KSTAT_DATA_UINT64 },
	{ "zil_itx_indirect_count",		KSTAT_DATA_UINT64 },
	{ "zil_itx_indirect_bytes",		KSTAT_DATA_UINT64 },
	{ "zil_itx_copied_count",		KSTAT_DATA_UINT64 },
	{ "zil_itx_copied_bytes",		KSTAT_DATA_UINT64 },
	{ "zil_itx_needcopy_count",		KSTAT_DATA_UINT64 },
	{ "zil_itx_needcopy_bytes",		KSTAT_DATA_UINT64 },
	{ "zil_itx_metaslab_normal_count",	KSTAT_DATA_UINT64 },
	{ "zil_itx_metaslab_normal_bytes",	KSTAT_DATA_UINT64 },
	{ "zil_itx_metaslab_normal_write",	KSTAT_DATA_UINT64 },
	{ "zil_itx_metaslab_normal_alloc",	KSTAT_DATA_UINT64 },
	{ "zil_itx_metaslab_slog_count",	KSTAT_DATA_UINT64 },
	{ "zil_itx_metaslab_slog_bytes",	KSTAT_DATA_UINT64 },
	{ "zil_itx_metaslab_slog_write",	KSTAT_DATA_UINT64 },
	{ "zil_itx_metaslab_slog_alloc",	KSTAT_DATA_UINT64 },
};

static zil_sums_t zil_sums_global;
static kstat_t *zil_kstats_global;

/*
 * Disable intent logging replay.  This global ZIL switch affects all pools.
 */
int zil_replay_disable = 0;

/*
 * Disable the flush commands that are normally sent to the disk(s) by the ZIL
 * after an LWB write has completed. Setting this will cause ZIL corruption on
 * power loss if a volatile out-of-order write cache is enabled.
 */
static int zil_nocacheflush = 0;

/*
 * Limit SLOG write size per commit executed with synchronous priority.
 * Any writes above that will be executed with lower (asynchronous) priority
 * to limit potential SLOG device abuse by single active ZIL writer.
 */
static uint64_t zil_slog_bulk = 64 * 1024 * 1024;

static kmem_cache_t *zil_lwb_cache;
static kmem_cache_t *zil_zcw_cache;

static void zil_lwb_commit(zilog_t *zilog, lwb_t *lwb, itx_t *itx);
static itx_t *zil_itx_clone(itx_t *oitx);
static uint64_t zil_max_waste_space(zilog_t *zilog);

static int
zil_bp_compare(const void *x1, const void *x2)
{
	const dva_t *dva1 = &((zil_bp_node_t *)x1)->zn_dva;
	const dva_t *dva2 = &((zil_bp_node_t *)x2)->zn_dva;

	int cmp = TREE_CMP(DVA_GET_VDEV(dva1), DVA_GET_VDEV(dva2));
	if (likely(cmp))
		return (cmp);

	return (TREE_CMP(DVA_GET_OFFSET(dva1), DVA_GET_OFFSET(dva2)));
}

static void
zil_bp_tree_init(zilog_t *zilog)
{
	avl_create(&zilog->zl_bp_tree, zil_bp_compare,
	    sizeof (zil_bp_node_t), offsetof(zil_bp_node_t, zn_node));
}

static void
zil_bp_tree_fini(zilog_t *zilog)
{
	avl_tree_t *t = &zilog->zl_bp_tree;
	zil_bp_node_t *zn;
	void *cookie = NULL;

	while ((zn = avl_destroy_nodes(t, &cookie)) != NULL)
		kmem_free(zn, sizeof (zil_bp_node_t));

	avl_destroy(t);
}

int
zil_bp_tree_add(zilog_t *zilog, const blkptr_t *bp)
{
	avl_tree_t *t = &zilog->zl_bp_tree;
	const dva_t *dva;
	zil_bp_node_t *zn;
	avl_index_t where;

	if (BP_IS_EMBEDDED(bp))
		return (0);

	dva = BP_IDENTITY(bp);

	if (avl_find(t, dva, &where) != NULL)
		return (SET_ERROR(EEXIST));

	zn = kmem_alloc(sizeof (zil_bp_node_t), KM_SLEEP);
	zn->zn_dva = *dva;
	avl_insert(t, zn, where);

	return (0);
}

static zil_header_t *
zil_header_in_syncing_context(zilog_t *zilog)
{
	return ((zil_header_t *)zilog->zl_header);
}

static void
zil_init_log_chain(zilog_t *zilog, blkptr_t *bp)
{
	zio_cksum_t *zc = &bp->blk_cksum;

	(void) random_get_pseudo_bytes((void *)&zc->zc_word[ZIL_ZC_GUID_0],
	    sizeof (zc->zc_word[ZIL_ZC_GUID_0]));
	(void) random_get_pseudo_bytes((void *)&zc->zc_word[ZIL_ZC_GUID_1],
	    sizeof (zc->zc_word[ZIL_ZC_GUID_1]));
	zc->zc_word[ZIL_ZC_OBJSET] = dmu_objset_id(zilog->zl_os);
	zc->zc_word[ZIL_ZC_SEQ] = 1ULL;
}

static int
zil_kstats_global_update(kstat_t *ksp, int rw)
{
	zil_kstat_values_t *zs = ksp->ks_data;
	ASSERT3P(&zil_stats, ==, zs);

	if (rw == KSTAT_WRITE) {
		return (SET_ERROR(EACCES));
	}

	zil_kstat_values_update(zs, &zil_sums_global);

	return (0);
}

/*
 * Read a log block and make sure it's valid.
 */
static int
zil_read_log_block(zilog_t *zilog, boolean_t decrypt, const blkptr_t *bp,
    blkptr_t *nbp, char **begin, char **end, arc_buf_t **abuf)
{
	zio_flag_t zio_flags = ZIO_FLAG_CANFAIL;
	arc_flags_t aflags = ARC_FLAG_WAIT;
	zbookmark_phys_t zb;
	int error;

	if (zilog->zl_header->zh_claim_txg == 0)
		zio_flags |= ZIO_FLAG_SPECULATIVE | ZIO_FLAG_SCRUB;

	if (!(zilog->zl_header->zh_flags & ZIL_CLAIM_LR_SEQ_VALID))
		zio_flags |= ZIO_FLAG_SPECULATIVE;

	if (!decrypt)
		zio_flags |= ZIO_FLAG_RAW;

	SET_BOOKMARK(&zb, bp->blk_cksum.zc_word[ZIL_ZC_OBJSET],
	    ZB_ZIL_OBJECT, ZB_ZIL_LEVEL, bp->blk_cksum.zc_word[ZIL_ZC_SEQ]);

	error = arc_read(NULL, zilog->zl_spa, bp, arc_getbuf_func,
	    abuf, ZIO_PRIORITY_SYNC_READ, zio_flags, &aflags, &zb);

	if (error == 0) {
		zio_cksum_t cksum = bp->blk_cksum;

		/*
		 * Validate the checksummed log block.
		 *
		 * Sequence numbers should be... sequential.  The checksum
		 * verifier for the next block should be bp's checksum plus 1.
		 *
		 * Also check the log chain linkage and size used.
		 */
		cksum.zc_word[ZIL_ZC_SEQ]++;

		uint64_t size = BP_GET_LSIZE(bp);
		if (BP_GET_CHECKSUM(bp) == ZIO_CHECKSUM_ZILOG2) {
			zil_chain_t *zilc = (*abuf)->b_data;
			char *lr = (char *)(zilc + 1);

			if (memcmp(&cksum, &zilc->zc_next_blk.blk_cksum,
			    sizeof (cksum)) ||
			    zilc->zc_nused < sizeof (*zilc) ||
			    zilc->zc_nused > size) {
				error = SET_ERROR(ECKSUM);
			} else {
				*begin = lr;
				*end = lr + zilc->zc_nused - sizeof (*zilc);
				*nbp = zilc->zc_next_blk;
			}
		} else {
			char *lr = (*abuf)->b_data;
			zil_chain_t *zilc = (zil_chain_t *)(lr + size) - 1;

			if (memcmp(&cksum, &zilc->zc_next_blk.blk_cksum,
			    sizeof (cksum)) ||
			    (zilc->zc_nused > (size - sizeof (*zilc)))) {
				error = SET_ERROR(ECKSUM);
			} else {
				*begin = lr;
				*end = lr + zilc->zc_nused;
				*nbp = zilc->zc_next_blk;
			}
		}
	}

	return (error);
}

/*
 * Read a TX_WRITE log data block.
 */
static int
zil_read_log_data(zilog_t *zilog, const lr_write_t *lr, void *wbuf)
{
	zio_flag_t zio_flags = ZIO_FLAG_CANFAIL;
	const blkptr_t *bp = &lr->lr_blkptr;
	arc_flags_t aflags = ARC_FLAG_WAIT;
	arc_buf_t *abuf = NULL;
	zbookmark_phys_t zb;
	int error;

	if (BP_IS_HOLE(bp)) {
		if (wbuf != NULL)
			memset(wbuf, 0, MAX(BP_GET_LSIZE(bp), lr->lr_length));
		return (0);
	}

	if (zilog->zl_header->zh_claim_txg == 0)
		zio_flags |= ZIO_FLAG_SPECULATIVE | ZIO_FLAG_SCRUB;

	/*
	 * If we are not using the resulting data, we are just checking that
	 * it hasn't been corrupted so we don't need to waste CPU time
	 * decompressing and decrypting it.
	 */
	if (wbuf == NULL)
		zio_flags |= ZIO_FLAG_RAW;

	ASSERT3U(BP_GET_LSIZE(bp), !=, 0);
	SET_BOOKMARK(&zb, dmu_objset_id(zilog->zl_os), lr->lr_foid,
	    ZB_ZIL_LEVEL, lr->lr_offset / BP_GET_LSIZE(bp));

	error = arc_read(NULL, zilog->zl_spa, bp, arc_getbuf_func, &abuf,
	    ZIO_PRIORITY_SYNC_READ, zio_flags, &aflags, &zb);

	if (error == 0) {
		if (wbuf != NULL)
			memcpy(wbuf, abuf->b_data, arc_buf_size(abuf));
		arc_buf_destroy(abuf, &abuf);
	}

	return (error);
}

void
zil_sums_init(zil_sums_t *zs)
{
	wmsum_init(&zs->zil_commit_count, 0);
	wmsum_init(&zs->zil_commit_writer_count, 0);
	wmsum_init(&zs->zil_commit_error_count, 0);
	wmsum_init(&zs->zil_commit_stall_count, 0);
	wmsum_init(&zs->zil_commit_suspend_count, 0);
	wmsum_init(&zs->zil_itx_count, 0);
	wmsum_init(&zs->zil_itx_indirect_count, 0);
	wmsum_init(&zs->zil_itx_indirect_bytes, 0);
	wmsum_init(&zs->zil_itx_copied_count, 0);
	wmsum_init(&zs->zil_itx_copied_bytes, 0);
	wmsum_init(&zs->zil_itx_needcopy_count, 0);
	wmsum_init(&zs->zil_itx_needcopy_bytes, 0);
	wmsum_init(&zs->zil_itx_metaslab_normal_count, 0);
	wmsum_init(&zs->zil_itx_metaslab_normal_bytes, 0);
	wmsum_init(&zs->zil_itx_metaslab_normal_write, 0);
	wmsum_init(&zs->zil_itx_metaslab_normal_alloc, 0);
	wmsum_init(&zs->zil_itx_metaslab_slog_count, 0);
	wmsum_init(&zs->zil_itx_metaslab_slog_bytes, 0);
	wmsum_init(&zs->zil_itx_metaslab_slog_write, 0);
	wmsum_init(&zs->zil_itx_metaslab_slog_alloc, 0);
}

void
zil_sums_fini(zil_sums_t *zs)
{
	wmsum_fini(&zs->zil_commit_count);
	wmsum_fini(&zs->zil_commit_writer_count);
	wmsum_fini(&zs->zil_commit_error_count);
	wmsum_fini(&zs->zil_commit_stall_count);
	wmsum_fini(&zs->zil_commit_suspend_count);
	wmsum_fini(&zs->zil_itx_count);
	wmsum_fini(&zs->zil_itx_indirect_count);
	wmsum_fini(&zs->zil_itx_indirect_bytes);
	wmsum_fini(&zs->zil_itx_copied_count);
	wmsum_fini(&zs->zil_itx_copied_bytes);
	wmsum_fini(&zs->zil_itx_needcopy_count);
	wmsum_fini(&zs->zil_itx_needcopy_bytes);
	wmsum_fini(&zs->zil_itx_metaslab_normal_count);
	wmsum_fini(&zs->zil_itx_metaslab_normal_bytes);
	wmsum_fini(&zs->zil_itx_metaslab_normal_write);
	wmsum_fini(&zs->zil_itx_metaslab_normal_alloc);
	wmsum_fini(&zs->zil_itx_metaslab_slog_count);
	wmsum_fini(&zs->zil_itx_metaslab_slog_bytes);
	wmsum_fini(&zs->zil_itx_metaslab_slog_write);
	wmsum_fini(&zs->zil_itx_metaslab_slog_alloc);
}

void
zil_kstat_values_update(zil_kstat_values_t *zs, zil_sums_t *zil_sums)
{
	zs->zil_commit_count.value.ui64 =
	    wmsum_value(&zil_sums->zil_commit_count);
	zs->zil_commit_writer_count.value.ui64 =
	    wmsum_value(&zil_sums->zil_commit_writer_count);
	zs->zil_commit_error_count.value.ui64 =
	    wmsum_value(&zil_sums->zil_commit_error_count);
	zs->zil_commit_stall_count.value.ui64 =
	    wmsum_value(&zil_sums->zil_commit_stall_count);
	zs->zil_commit_suspend_count.value.ui64 =
	    wmsum_value(&zil_sums->zil_commit_suspend_count);
	zs->zil_itx_count.value.ui64 =
	    wmsum_value(&zil_sums->zil_itx_count);
	zs->zil_itx_indirect_count.value.ui64 =
	    wmsum_value(&zil_sums->zil_itx_indirect_count);
	zs->zil_itx_indirect_bytes.value.ui64 =
	    wmsum_value(&zil_sums->zil_itx_indirect_bytes);
	zs->zil_itx_copied_count.value.ui64 =
	    wmsum_value(&zil_sums->zil_itx_copied_count);
	zs->zil_itx_copied_bytes.value.ui64 =
	    wmsum_value(&zil_sums->zil_itx_copied_bytes);
	zs->zil_itx_needcopy_count.value.ui64 =
	    wmsum_value(&zil_sums->zil_itx_needcopy_count);
	zs->zil_itx_needcopy_bytes.value.ui64 =
	    wmsum_value(&zil_sums->zil_itx_needcopy_bytes);
	zs->zil_itx_metaslab_normal_count.value.ui64 =
	    wmsum_value(&zil_sums->zil_itx_metaslab_normal_count);
	zs->zil_itx_metaslab_normal_bytes.value.ui64 =
	    wmsum_value(&zil_sums->zil_itx_metaslab_normal_bytes);
	zs->zil_itx_metaslab_normal_write.value.ui64 =
	    wmsum_value(&zil_sums->zil_itx_metaslab_normal_write);
	zs->zil_itx_metaslab_normal_alloc.value.ui64 =
	    wmsum_value(&zil_sums->zil_itx_metaslab_normal_alloc);
	zs->zil_itx_metaslab_slog_count.value.ui64 =
	    wmsum_value(&zil_sums->zil_itx_metaslab_slog_count);
	zs->zil_itx_metaslab_slog_bytes.value.ui64 =
	    wmsum_value(&zil_sums->zil_itx_metaslab_slog_bytes);
	zs->zil_itx_metaslab_slog_write.value.ui64 =
	    wmsum_value(&zil_sums->zil_itx_metaslab_slog_write);
	zs->zil_itx_metaslab_slog_alloc.value.ui64 =
	    wmsum_value(&zil_sums->zil_itx_metaslab_slog_alloc);
}

/*
 * Parse the intent log, and call parse_func for each valid record within.
 */
int
zil_parse(zilog_t *zilog, zil_parse_blk_func_t *parse_blk_func,
    zil_parse_lr_func_t *parse_lr_func, void *arg, uint64_t txg,
    boolean_t decrypt)
{
	const zil_header_t *zh = zilog->zl_header;
	boolean_t claimed = !!zh->zh_claim_txg;
	uint64_t claim_blk_seq = claimed ? zh->zh_claim_blk_seq : UINT64_MAX;
	uint64_t claim_lr_seq = claimed ? zh->zh_claim_lr_seq : UINT64_MAX;
	uint64_t max_blk_seq = 0;
	uint64_t max_lr_seq = 0;
	uint64_t blk_count = 0;
	uint64_t lr_count = 0;
	blkptr_t blk, next_blk = {{{{0}}}};
	int error = 0;

	/*
	 * Old logs didn't record the maximum zh_claim_lr_seq.
	 */
	if (!(zh->zh_flags & ZIL_CLAIM_LR_SEQ_VALID))
		claim_lr_seq = UINT64_MAX;

	/*
	 * Starting at the block pointed to by zh_log we read the log chain.
	 * For each block in the chain we strongly check that block to
	 * ensure its validity.  We stop when an invalid block is found.
	 * For each block pointer in the chain we call parse_blk_func().
	 * For each record in each valid block we call parse_lr_func().
	 * If the log has been claimed, stop if we encounter a sequence
	 * number greater than the highest claimed sequence number.
	 */
	zil_bp_tree_init(zilog);

	for (blk = zh->zh_log; !BP_IS_HOLE(&blk); blk = next_blk) {
		uint64_t blk_seq = blk.blk_cksum.zc_word[ZIL_ZC_SEQ];
		int reclen;
		char *lrp, *end;
		arc_buf_t *abuf = NULL;

		if (blk_seq > claim_blk_seq)
			break;

		error = parse_blk_func(zilog, &blk, arg, txg);
		if (error != 0)
			break;
		ASSERT3U(max_blk_seq, <, blk_seq);
		max_blk_seq = blk_seq;
		blk_count++;

		if (max_lr_seq == claim_lr_seq && max_blk_seq == claim_blk_seq)
			break;

		error = zil_read_log_block(zilog, decrypt, &blk, &next_blk,
		    &lrp, &end, &abuf);
		if (error != 0) {
			if (abuf)
				arc_buf_destroy(abuf, &abuf);
			if (claimed) {
				char name[ZFS_MAX_DATASET_NAME_LEN];

				dmu_objset_name(zilog->zl_os, name);

				cmn_err(CE_WARN, "ZFS read log block error %d, "
				    "dataset %s, seq 0x%llx\n", error, name,
				    (u_longlong_t)blk_seq);
			}
			break;
		}

		for (; lrp < end; lrp += reclen) {
			lr_t *lr = (lr_t *)lrp;

			/*
			 * Are the remaining bytes large enough to hold an
			 * log record?
			 */
			if ((char *)(lr + 1) > end) {
				cmn_err(CE_WARN, "zil_parse: lr_t overrun");
				error = SET_ERROR(ECKSUM);
				arc_buf_destroy(abuf, &abuf);
				goto done;
			}
			reclen = lr->lrc_reclen;
			if (reclen < sizeof (lr_t) || reclen > end - lrp) {
				cmn_err(CE_WARN,
				    "zil_parse: lr_t has an invalid reclen");
				error = SET_ERROR(ECKSUM);
				arc_buf_destroy(abuf, &abuf);
				goto done;
			}

			if (lr->lrc_seq > claim_lr_seq) {
				arc_buf_destroy(abuf, &abuf);
				goto done;
			}

			error = parse_lr_func(zilog, lr, arg, txg);
			if (error != 0) {
				arc_buf_destroy(abuf, &abuf);
				goto done;
			}
			ASSERT3U(max_lr_seq, <, lr->lrc_seq);
			max_lr_seq = lr->lrc_seq;
			lr_count++;
		}
		arc_buf_destroy(abuf, &abuf);
	}
done:
	zilog->zl_parse_error = error;
	zilog->zl_parse_blk_seq = max_blk_seq;
	zilog->zl_parse_lr_seq = max_lr_seq;
	zilog->zl_parse_blk_count = blk_count;
	zilog->zl_parse_lr_count = lr_count;

	zil_bp_tree_fini(zilog);

	return (error);
}

static int
zil_clear_log_block(zilog_t *zilog, const blkptr_t *bp, void *tx,
    uint64_t first_txg)
{
	(void) tx;
	ASSERT(!BP_IS_HOLE(bp));

	/*
	 * As we call this function from the context of a rewind to a
	 * checkpoint, each ZIL block whose txg is later than the txg
	 * that we rewind to is invalid. Thus, we return -1 so
	 * zil_parse() doesn't attempt to read it.
	 */
	if (BP_GET_LOGICAL_BIRTH(bp) >= first_txg)
		return (-1);

	if (zil_bp_tree_add(zilog, bp) != 0)
		return (0);

	zio_free(zilog->zl_spa, first_txg, bp);
	return (0);
}

static int
zil_noop_log_record(zilog_t *zilog, const lr_t *lrc, void *tx,
    uint64_t first_txg)
{
	(void) zilog, (void) lrc, (void) tx, (void) first_txg;
	return (0);
}

static int
zil_claim_log_block(zilog_t *zilog, const blkptr_t *bp, void *tx,
    uint64_t first_txg)
{
	/*
	 * Claim log block if not already committed and not already claimed.
	 * If tx == NULL, just verify that the block is claimable.
	 */
	if (BP_IS_HOLE(bp) || BP_GET_LOGICAL_BIRTH(bp) < first_txg ||
	    zil_bp_tree_add(zilog, bp) != 0)
		return (0);

	return (zio_wait(zio_claim(NULL, zilog->zl_spa,
	    tx == NULL ? 0 : first_txg, bp, spa_claim_notify, NULL,
	    ZIO_FLAG_CANFAIL | ZIO_FLAG_SPECULATIVE | ZIO_FLAG_SCRUB)));
}

static int
zil_claim_write(zilog_t *zilog, const lr_t *lrc, void *tx, uint64_t first_txg)
{
	lr_write_t *lr = (lr_write_t *)lrc;
	int error;

	ASSERT3U(lrc->lrc_reclen, >=, sizeof (*lr));

	/*
	 * If the block is not readable, don't claim it.  This can happen
	 * in normal operation when a log block is written to disk before
	 * some of the dmu_sync() blocks it points to.  In this case, the
	 * transaction cannot have been committed to anyone (we would have
	 * waited for all writes to be stable first), so it is semantically
	 * correct to declare this the end of the log.
	 */
	if (BP_GET_LOGICAL_BIRTH(&lr->lr_blkptr) >= first_txg) {
		error = zil_read_log_data(zilog, lr, NULL);
		if (error != 0)
			return (error);
	}

	return (zil_claim_log_block(zilog, &lr->lr_blkptr, tx, first_txg));
}

static int
zil_claim_clone_range(zilog_t *zilog, const lr_t *lrc, void *tx,
    uint64_t first_txg)
{
	const lr_clone_range_t *lr = (const lr_clone_range_t *)lrc;
	const blkptr_t *bp;
	spa_t *spa = zilog->zl_spa;
	uint_t ii;

	ASSERT3U(lrc->lrc_reclen, >=, sizeof (*lr));
	ASSERT3U(lrc->lrc_reclen, >=, offsetof(lr_clone_range_t,
	    lr_bps[lr->lr_nbps]));

	if (tx == NULL) {
		return (0);
	}

	/*
	 * XXX: Do we need to byteswap lr?
	 */

	for (ii = 0; ii < lr->lr_nbps; ii++) {
		bp = &lr->lr_bps[ii];

		/*
		 * When data is embedded into the BP there is no need to create
		 * BRT entry as there is no data block.  Just copy the BP as it
		 * contains the data.
		 */
		if (BP_IS_HOLE(bp) || BP_IS_EMBEDDED(bp))
			continue;

		/*
		 * We can not handle block pointers from the future, since they
		 * are not yet allocated.  It should not normally happen, but
		 * just in case lets be safe and just stop here now instead of
		 * corrupting the pool.
		 */
		if (BP_GET_BIRTH(bp) >= first_txg)
			return (SET_ERROR(ENOENT));

		/*
		 * Assert the block is really allocated before we reference it.
		 */
		metaslab_check_free(spa, bp);
	}

	for (ii = 0; ii < lr->lr_nbps; ii++) {
		bp = &lr->lr_bps[ii];
		if (!BP_IS_HOLE(bp) && !BP_IS_EMBEDDED(bp))
			brt_pending_add(spa, bp, tx);
	}

	return (0);
}

static int
zil_claim_log_record(zilog_t *zilog, const lr_t *lrc, void *tx,
    uint64_t first_txg)
{

	switch (lrc->lrc_txtype) {
	case TX_WRITE:
		return (zil_claim_write(zilog, lrc, tx, first_txg));
	case TX_CLONE_RANGE:
		return (zil_claim_clone_range(zilog, lrc, tx, first_txg));
	default:
		return (0);
	}
}

static int
zil_free_log_block(zilog_t *zilog, const blkptr_t *bp, void *tx,
    uint64_t claim_txg)
{
	(void) claim_txg;

	zio_free(zilog->zl_spa, dmu_tx_get_txg(tx), bp);

	return (0);
}

static int
zil_free_write(zilog_t *zilog, const lr_t *lrc, void *tx, uint64_t claim_txg)
{
	lr_write_t *lr = (lr_write_t *)lrc;
	blkptr_t *bp = &lr->lr_blkptr;

	ASSERT3U(lrc->lrc_reclen, >=, sizeof (*lr));

	/*
	 * If we previously claimed it, we need to free it.
	 */
	if (BP_GET_LOGICAL_BIRTH(bp) >= claim_txg &&
	    zil_bp_tree_add(zilog, bp) == 0 && !BP_IS_HOLE(bp)) {
		zio_free(zilog->zl_spa, dmu_tx_get_txg(tx), bp);
	}

	return (0);
}

static int
zil_free_clone_range(zilog_t *zilog, const lr_t *lrc, void *tx)
{
	const lr_clone_range_t *lr = (const lr_clone_range_t *)lrc;
	const blkptr_t *bp;
	spa_t *spa;
	uint_t ii;

	ASSERT3U(lrc->lrc_reclen, >=, sizeof (*lr));
	ASSERT3U(lrc->lrc_reclen, >=, offsetof(lr_clone_range_t,
	    lr_bps[lr->lr_nbps]));

	if (tx == NULL) {
		return (0);
	}

	spa = zilog->zl_spa;

	for (ii = 0; ii < lr->lr_nbps; ii++) {
		bp = &lr->lr_bps[ii];

		if (!BP_IS_HOLE(bp)) {
			zio_free(spa, dmu_tx_get_txg(tx), bp);
		}
	}

	return (0);
}

static int
zil_free_log_record(zilog_t *zilog, const lr_t *lrc, void *tx,
    uint64_t claim_txg)
{

	if (claim_txg == 0) {
		return (0);
	}

	switch (lrc->lrc_txtype) {
	case TX_WRITE:
		return (zil_free_write(zilog, lrc, tx, claim_txg));
	case TX_CLONE_RANGE:
		return (zil_free_clone_range(zilog, lrc, tx));
	default:
		return (0);
	}
}

static int
zil_lwb_vdev_compare(const void *x1, const void *x2)
{
	const uint64_t v1 = ((zil_vdev_node_t *)x1)->zv_vdev;
	const uint64_t v2 = ((zil_vdev_node_t *)x2)->zv_vdev;

	return (TREE_CMP(v1, v2));
}

/*
 * Allocate a new lwb.  We may already have a block pointer for it, in which
 * case we get size and version from there.  Or we may not yet, in which case
 * we choose them here and later make the block allocation match.
 */
static lwb_t *
zil_alloc_lwb(zilog_t *zilog, int sz, blkptr_t *bp, boolean_t slog,
    uint64_t txg, lwb_state_t state)
{
	lwb_t *lwb;

	lwb = kmem_cache_alloc(zil_lwb_cache, KM_SLEEP);
	lwb->lwb_zilog = zilog;
	if (bp) {
		lwb->lwb_blk = *bp;
		lwb->lwb_slim = (BP_GET_CHECKSUM(bp) == ZIO_CHECKSUM_ZILOG2);
		sz = BP_GET_LSIZE(bp);
	} else {
		BP_ZERO(&lwb->lwb_blk);
		lwb->lwb_slim = (spa_version(zilog->zl_spa) >=
		    SPA_VERSION_SLIM_ZIL);
	}
	lwb->lwb_slog = slog;
	lwb->lwb_error = 0;
	if (lwb->lwb_slim) {
		lwb->lwb_nmax = sz;
		lwb->lwb_nused = lwb->lwb_nfilled = sizeof (zil_chain_t);
	} else {
		lwb->lwb_nmax = sz - sizeof (zil_chain_t);
		lwb->lwb_nused = lwb->lwb_nfilled = 0;
	}
	lwb->lwb_sz = sz;
	lwb->lwb_state = state;
	lwb->lwb_buf = zio_buf_alloc(sz);
	lwb->lwb_child_zio = NULL;
	lwb->lwb_write_zio = NULL;
	lwb->lwb_root_zio = NULL;
	lwb->lwb_issued_timestamp = 0;
	lwb->lwb_issued_txg = 0;
	lwb->lwb_alloc_txg = txg;
	lwb->lwb_max_txg = 0;

	mutex_enter(&zilog->zl_lock);
	list_insert_tail(&zilog->zl_lwb_list, lwb);
	if (state != LWB_STATE_NEW)
		zilog->zl_last_lwb_opened = lwb;
	mutex_exit(&zilog->zl_lock);

	return (lwb);
}

static void
zil_free_lwb(zilog_t *zilog, lwb_t *lwb)
{
	ASSERT(MUTEX_HELD(&zilog->zl_lock));
	ASSERT(lwb->lwb_state == LWB_STATE_NEW ||
	    lwb->lwb_state == LWB_STATE_FLUSH_DONE);
	ASSERT3P(lwb->lwb_child_zio, ==, NULL);
	ASSERT3P(lwb->lwb_write_zio, ==, NULL);
	ASSERT3P(lwb->lwb_root_zio, ==, NULL);
	ASSERT3U(lwb->lwb_alloc_txg, <=, spa_syncing_txg(zilog->zl_spa));
	ASSERT3U(lwb->lwb_max_txg, <=, spa_syncing_txg(zilog->zl_spa));
	VERIFY(list_is_empty(&lwb->lwb_itxs));
	VERIFY(list_is_empty(&lwb->lwb_waiters));
	ASSERT(avl_is_empty(&lwb->lwb_vdev_tree));
	ASSERT(!MUTEX_HELD(&lwb->lwb_vdev_lock));

	/*
	 * Clear the zilog's field to indicate this lwb is no longer
	 * valid, and prevent use-after-free errors.
	 */
	if (zilog->zl_last_lwb_opened == lwb)
		zilog->zl_last_lwb_opened = NULL;

	kmem_cache_free(zil_lwb_cache, lwb);
}

/*
 * Called when we create in-memory log transactions so that we know
 * to cleanup the itxs at the end of spa_sync().
 */
static void
zilog_dirty(zilog_t *zilog, uint64_t txg)
{
	dsl_pool_t *dp = zilog->zl_dmu_pool;
	dsl_dataset_t *ds = dmu_objset_ds(zilog->zl_os);

	ASSERT(spa_writeable(zilog->zl_spa));

	if (ds->ds_is_snapshot)
		panic("dirtying snapshot!");

	if (txg_list_add(&dp->dp_dirty_zilogs, zilog, txg)) {
		/* up the hold count until we can be written out */
		dmu_buf_add_ref(ds->ds_dbuf, zilog);

		zilog->zl_dirty_max_txg = MAX(txg, zilog->zl_dirty_max_txg);
	}
}

/*
 * Determine if the zil is dirty in the specified txg. Callers wanting to
 * ensure that the dirty state does not change must hold the itxg_lock for
 * the specified txg. Holding the lock will ensure that the zil cannot be
 * dirtied (zil_itx_assign) or cleaned (zil_clean) while we check its current
 * state.
 */
static boolean_t __maybe_unused
zilog_is_dirty_in_txg(zilog_t *zilog, uint64_t txg)
{
	dsl_pool_t *dp = zilog->zl_dmu_pool;

	if (txg_list_member(&dp->dp_dirty_zilogs, zilog, txg & TXG_MASK))
		return (B_TRUE);
	return (B_FALSE);
}

/*
 * Determine if the zil is dirty. The zil is considered dirty if it has
 * any pending itx records that have not been cleaned by zil_clean().
 */
static boolean_t
zilog_is_dirty(zilog_t *zilog)
{
	dsl_pool_t *dp = zilog->zl_dmu_pool;

	for (int t = 0; t < TXG_SIZE; t++) {
		if (txg_list_member(&dp->dp_dirty_zilogs, zilog, t))
			return (B_TRUE);
	}
	return (B_FALSE);
}

/*
 * Its called in zil_commit context (zil_process_commit_list()/zil_create()).
 * It activates SPA_FEATURE_ZILSAXATTR feature, if its enabled.
 * Check dsl_dataset_feature_is_active to avoid txg_wait_synced() on every
 * zil_commit.
 */
static void
zil_commit_activate_saxattr_feature(zilog_t *zilog)
{
	dsl_dataset_t *ds = dmu_objset_ds(zilog->zl_os);
	uint64_t txg = 0;
	dmu_tx_t *tx = NULL;

	if (spa_feature_is_enabled(zilog->zl_spa, SPA_FEATURE_ZILSAXATTR) &&
	    dmu_objset_type(zilog->zl_os) != DMU_OST_ZVOL &&
	    !dsl_dataset_feature_is_active(ds, SPA_FEATURE_ZILSAXATTR)) {
		tx = dmu_tx_create(zilog->zl_os);
		VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT | DMU_TX_SUSPEND));
		dsl_dataset_dirty(ds, tx);
		txg = dmu_tx_get_txg(tx);

		mutex_enter(&ds->ds_lock);
		ds->ds_feature_activation[SPA_FEATURE_ZILSAXATTR] =
		    (void *)B_TRUE;
		mutex_exit(&ds->ds_lock);
		dmu_tx_commit(tx);
		txg_wait_synced(zilog->zl_dmu_pool, txg);
	}
}

/*
 * Create an on-disk intent log.
 */
static lwb_t *
zil_create(zilog_t *zilog)
{
	const zil_header_t *zh = zilog->zl_header;
	lwb_t *lwb = NULL;
	uint64_t txg = 0;
	dmu_tx_t *tx = NULL;
	blkptr_t blk;
	int error = 0;
	boolean_t slog = FALSE;
	dsl_dataset_t *ds = dmu_objset_ds(zilog->zl_os);


	/*
	 * Wait for any previous destroy to complete.
	 */
	txg_wait_synced(zilog->zl_dmu_pool, zilog->zl_destroy_txg);

	ASSERT(zh->zh_claim_txg == 0);
	ASSERT(zh->zh_replay_seq == 0);

	blk = zh->zh_log;

	/*
	 * Allocate an initial log block if:
	 *    - there isn't one already
	 *    - the existing block is the wrong endianness
	 */
	if (BP_IS_HOLE(&blk) || BP_SHOULD_BYTESWAP(&blk)) {
		tx = dmu_tx_create(zilog->zl_os);
		VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT | DMU_TX_SUSPEND));
		dsl_dataset_dirty(dmu_objset_ds(zilog->zl_os), tx);
		txg = dmu_tx_get_txg(tx);

		if (!BP_IS_HOLE(&blk)) {
			zio_free(zilog->zl_spa, txg, &blk);
			BP_ZERO(&blk);
		}

		error = zio_alloc_zil(zilog->zl_spa, zilog->zl_os, txg, &blk,
		    ZIL_MIN_BLKSZ, &slog);
		if (error == 0)
			zil_init_log_chain(zilog, &blk);
	}

	/*
	 * Allocate a log write block (lwb) for the first log block.
	 */
	if (error == 0)
		lwb = zil_alloc_lwb(zilog, 0, &blk, slog, txg, LWB_STATE_NEW);

	/*
	 * If we just allocated the first log block, commit our transaction
	 * and wait for zil_sync() to stuff the block pointer into zh_log.
	 * (zh is part of the MOS, so we cannot modify it in open context.)
	 */
	if (tx != NULL) {
		/*
		 * If "zilsaxattr" feature is enabled on zpool, then activate
		 * it now when we're creating the ZIL chain. We can't wait with
		 * this until we write the first xattr log record because we
		 * need to wait for the feature activation to sync out.
		 */
		if (spa_feature_is_enabled(zilog->zl_spa,
		    SPA_FEATURE_ZILSAXATTR) && dmu_objset_type(zilog->zl_os) !=
		    DMU_OST_ZVOL) {
			mutex_enter(&ds->ds_lock);
			ds->ds_feature_activation[SPA_FEATURE_ZILSAXATTR] =
			    (void *)B_TRUE;
			mutex_exit(&ds->ds_lock);
		}

		dmu_tx_commit(tx);
		txg_wait_synced(zilog->zl_dmu_pool, txg);
	} else {
		/*
		 * This branch covers the case where we enable the feature on a
		 * zpool that has existing ZIL headers.
		 */
		zil_commit_activate_saxattr_feature(zilog);
	}
	IMPLY(spa_feature_is_enabled(zilog->zl_spa, SPA_FEATURE_ZILSAXATTR) &&
	    dmu_objset_type(zilog->zl_os) != DMU_OST_ZVOL,
	    dsl_dataset_feature_is_active(ds, SPA_FEATURE_ZILSAXATTR));

	ASSERT(error != 0 || memcmp(&blk, &zh->zh_log, sizeof (blk)) == 0);
	IMPLY(error == 0, lwb != NULL);

	return (lwb);
}

/*
 * In one tx, free all log blocks and clear the log header. If keep_first
 * is set, then we're replaying a log with no content. We want to keep the
 * first block, however, so that the first synchronous transaction doesn't
 * require a txg_wait_synced() in zil_create(). We don't need to
 * txg_wait_synced() here either when keep_first is set, because both
 * zil_create() and zil_destroy() will wait for any in-progress destroys
 * to complete.
 * Return B_TRUE if there were any entries to replay.
 */
boolean_t
zil_destroy(zilog_t *zilog, boolean_t keep_first)
{
	const zil_header_t *zh = zilog->zl_header;
	lwb_t *lwb;
	dmu_tx_t *tx;
	uint64_t txg;

	/*
	 * Wait for any previous destroy to complete.
	 */
	txg_wait_synced(zilog->zl_dmu_pool, zilog->zl_destroy_txg);

	zilog->zl_old_header = *zh;		/* debugging aid */

	if (BP_IS_HOLE(&zh->zh_log))
		return (B_FALSE);

	tx = dmu_tx_create(zilog->zl_os);
	VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT | DMU_TX_SUSPEND));
	dsl_dataset_dirty(dmu_objset_ds(zilog->zl_os), tx);
	txg = dmu_tx_get_txg(tx);

	mutex_enter(&zilog->zl_lock);

	ASSERT3U(zilog->zl_destroy_txg, <, txg);
	zilog->zl_destroy_txg = txg;
	zilog->zl_keep_first = keep_first;

	if (!list_is_empty(&zilog->zl_lwb_list)) {
		ASSERT(zh->zh_claim_txg == 0);
		VERIFY(!keep_first);
		while ((lwb = list_remove_head(&zilog->zl_lwb_list)) != NULL) {
			if (lwb->lwb_buf != NULL)
				zio_buf_free(lwb->lwb_buf, lwb->lwb_sz);
			if (!BP_IS_HOLE(&lwb->lwb_blk))
				zio_free(zilog->zl_spa, txg, &lwb->lwb_blk);
			zil_free_lwb(zilog, lwb);
		}
	} else if (!keep_first) {
		zil_destroy_sync(zilog, tx);
	}
	mutex_exit(&zilog->zl_lock);

	dmu_tx_commit(tx);

	return (B_TRUE);
}

void
zil_destroy_sync(zilog_t *zilog, dmu_tx_t *tx)
{
	ASSERT(list_is_empty(&zilog->zl_lwb_list));
	(void) zil_parse(zilog, zil_free_log_block,
	    zil_free_log_record, tx, zilog->zl_header->zh_claim_txg, B_FALSE);
}

int
zil_claim(dsl_pool_t *dp, dsl_dataset_t *ds, void *txarg)
{
	dmu_tx_t *tx = txarg;
	zilog_t *zilog;
	uint64_t first_txg;
	zil_header_t *zh;
	objset_t *os;
	int error;

	error = dmu_objset_own_obj(dp, ds->ds_object,
	    DMU_OST_ANY, B_FALSE, B_FALSE, FTAG, &os);
	if (error != 0) {
		/*
		 * EBUSY indicates that the objset is inconsistent, in which
		 * case it can not have a ZIL.
		 */
		if (error != EBUSY) {
			cmn_err(CE_WARN, "can't open objset for %llu, error %u",
			    (unsigned long long)ds->ds_object, error);
		}

		return (0);
	}

	zilog = dmu_objset_zil(os);
	zh = zil_header_in_syncing_context(zilog);
	ASSERT3U(tx->tx_txg, ==, spa_first_txg(zilog->zl_spa));
	first_txg = spa_min_claim_txg(zilog->zl_spa);

	/*
	 * If the spa_log_state is not set to be cleared, check whether
	 * the current uberblock is a checkpoint one and if the current
	 * header has been claimed before moving on.
	 *
	 * If the current uberblock is a checkpointed uberblock then
	 * one of the following scenarios took place:
	 *
	 * 1] We are currently rewinding to the checkpoint of the pool.
	 * 2] We crashed in the middle of a checkpoint rewind but we
	 *    did manage to write the checkpointed uberblock to the
	 *    vdev labels, so when we tried to import the pool again
	 *    the checkpointed uberblock was selected from the import
	 *    procedure.
	 *
	 * In both cases we want to zero out all the ZIL blocks, except
	 * the ones that have been claimed at the time of the checkpoint
	 * (their zh_claim_txg != 0). The reason is that these blocks
	 * may be corrupted since we may have reused their locations on
	 * disk after we took the checkpoint.
	 *
	 * We could try to set spa_log_state to SPA_LOG_CLEAR earlier
	 * when we first figure out whether the current uberblock is
	 * checkpointed or not. Unfortunately, that would discard all
	 * the logs, including the ones that are claimed, and we would
	 * leak space.
	 */
	if (spa_get_log_state(zilog->zl_spa) == SPA_LOG_CLEAR ||
	    (zilog->zl_spa->spa_uberblock.ub_checkpoint_txg != 0 &&
	    zh->zh_claim_txg == 0)) {
		if (!BP_IS_HOLE(&zh->zh_log)) {
			(void) zil_parse(zilog, zil_clear_log_block,
			    zil_noop_log_record, tx, first_txg, B_FALSE);
		}
		BP_ZERO(&zh->zh_log);
		if (os->os_encrypted)
			os->os_next_write_raw[tx->tx_txg & TXG_MASK] = B_TRUE;
		dsl_dataset_dirty(dmu_objset_ds(os), tx);
		dmu_objset_disown(os, B_FALSE, FTAG);
		return (0);
	}

	/*
	 * If we are not rewinding and opening the pool normally, then
	 * the min_claim_txg should be equal to the first txg of the pool.
	 */
	ASSERT3U(first_txg, ==, spa_first_txg(zilog->zl_spa));

	/*
	 * Claim all log blocks if we haven't already done so, and remember
	 * the highest claimed sequence number.  This ensures that if we can
	 * read only part of the log now (e.g. due to a missing device),
	 * but we can read the entire log later, we will not try to replay
	 * or destroy beyond the last block we successfully claimed.
	 */
	ASSERT3U(zh->zh_claim_txg, <=, first_txg);
	if (zh->zh_claim_txg == 0 && !BP_IS_HOLE(&zh->zh_log)) {
		(void) zil_parse(zilog, zil_claim_log_block,
		    zil_claim_log_record, tx, first_txg, B_FALSE);
		zh->zh_claim_txg = first_txg;
		zh->zh_claim_blk_seq = zilog->zl_parse_blk_seq;
		zh->zh_claim_lr_seq = zilog->zl_parse_lr_seq;
		if (zilog->zl_parse_lr_count || zilog->zl_parse_blk_count > 1)
			zh->zh_flags |= ZIL_REPLAY_NEEDED;
		zh->zh_flags |= ZIL_CLAIM_LR_SEQ_VALID;
		if (os->os_encrypted)
			os->os_next_write_raw[tx->tx_txg & TXG_MASK] = B_TRUE;
		dsl_dataset_dirty(dmu_objset_ds(os), tx);
	}

	ASSERT3U(first_txg, ==, (spa_last_synced_txg(zilog->zl_spa) + 1));
	dmu_objset_disown(os, B_FALSE, FTAG);
	return (0);
}

/*
 * Check the log by walking the log chain.
 * Checksum errors are ok as they indicate the end of the chain.
 * Any other error (no device or read failure) returns an error.
 */
int
zil_check_log_chain(dsl_pool_t *dp, dsl_dataset_t *ds, void *tx)
{
	(void) dp;
	zilog_t *zilog;
	objset_t *os;
	blkptr_t *bp;
	int error;

	ASSERT(tx == NULL);

	error = dmu_objset_from_ds(ds, &os);
	if (error != 0) {
		cmn_err(CE_WARN, "can't open objset %llu, error %d",
		    (unsigned long long)ds->ds_object, error);
		return (0);
	}

	zilog = dmu_objset_zil(os);
	bp = (blkptr_t *)&zilog->zl_header->zh_log;

	if (!BP_IS_HOLE(bp)) {
		vdev_t *vd;
		boolean_t valid = B_TRUE;

		/*
		 * Check the first block and determine if it's on a log device
		 * which may have been removed or faulted prior to loading this
		 * pool.  If so, there's no point in checking the rest of the
		 * log as its content should have already been synced to the
		 * pool.
		 */
		spa_config_enter(os->os_spa, SCL_STATE, FTAG, RW_READER);
		vd = vdev_lookup_top(os->os_spa, DVA_GET_VDEV(&bp->blk_dva[0]));
		if (vd->vdev_islog && vdev_is_dead(vd))
			valid = vdev_log_state_valid(vd);
		spa_config_exit(os->os_spa, SCL_STATE, FTAG);

		if (!valid)
			return (0);

		/*
		 * Check whether the current uberblock is checkpointed (e.g.
		 * we are rewinding) and whether the current header has been
		 * claimed or not. If it hasn't then skip verifying it. We
		 * do this because its ZIL blocks may be part of the pool's
		 * state before the rewind, which is no longer valid.
		 */
		zil_header_t *zh = zil_header_in_syncing_context(zilog);
		if (zilog->zl_spa->spa_uberblock.ub_checkpoint_txg != 0 &&
		    zh->zh_claim_txg == 0)
			return (0);
	}

	/*
	 * Because tx == NULL, zil_claim_log_block() will not actually claim
	 * any blocks, but just determine whether it is possible to do so.
	 * In addition to checking the log chain, zil_claim_log_block()
	 * will invoke zio_claim() with a done func of spa_claim_notify(),
	 * which will update spa_max_claim_txg.  See spa_load() for details.
	 */
	error = zil_parse(zilog, zil_claim_log_block, zil_claim_log_record, tx,
	    zilog->zl_header->zh_claim_txg ? -1ULL :
	    spa_min_claim_txg(os->os_spa), B_FALSE);

	return ((error == ECKSUM || error == ENOENT) ? 0 : error);
}

/*
 * When an itx is "skipped", this function is used to properly mark the
 * waiter as "done, and signal any thread(s) waiting on it. An itx can
 * be skipped (and not committed to an lwb) for a variety of reasons,
 * one of them being that the itx was committed via spa_sync(), prior to
 * it being committed to an lwb; this can happen if a thread calling
 * zil_commit() is racing with spa_sync().
 */
static void
zil_commit_waiter_skip(zil_commit_waiter_t *zcw)
{
	mutex_enter(&zcw->zcw_lock);
	ASSERT3B(zcw->zcw_done, ==, B_FALSE);
	zcw->zcw_done = B_TRUE;
	cv_broadcast(&zcw->zcw_cv);
	mutex_exit(&zcw->zcw_lock);
}

/*
 * This function is used when the given waiter is to be linked into an
 * lwb's "lwb_waiter" list; i.e. when the itx is committed to the lwb.
 * At this point, the waiter will no longer be referenced by the itx,
 * and instead, will be referenced by the lwb.
 */
static void
zil_commit_waiter_link_lwb(zil_commit_waiter_t *zcw, lwb_t *lwb)
{
	/*
	 * The lwb_waiters field of the lwb is protected by the zilog's
	 * zl_issuer_lock while the lwb is open and zl_lock otherwise.
	 * zl_issuer_lock also protects leaving the open state.
	 * zcw_lwb setting is protected by zl_issuer_lock and state !=
	 * flush_done, which transition is protected by zl_lock.
	 */
	ASSERT(MUTEX_HELD(&lwb->lwb_zilog->zl_issuer_lock));
	IMPLY(lwb->lwb_state != LWB_STATE_OPENED,
	    MUTEX_HELD(&lwb->lwb_zilog->zl_lock));
	ASSERT3S(lwb->lwb_state, !=, LWB_STATE_NEW);
	ASSERT3S(lwb->lwb_state, !=, LWB_STATE_FLUSH_DONE);

	ASSERT(!list_link_active(&zcw->zcw_node));
	list_insert_tail(&lwb->lwb_waiters, zcw);
	ASSERT3P(zcw->zcw_lwb, ==, NULL);
	zcw->zcw_lwb = lwb;
}

/*
 * This function is used when zio_alloc_zil() fails to allocate a ZIL
 * block, and the given waiter must be linked to the "nolwb waiters"
 * list inside of zil_process_commit_list().
 */
static void
zil_commit_waiter_link_nolwb(zil_commit_waiter_t *zcw, list_t *nolwb)
{
	ASSERT(!list_link_active(&zcw->zcw_node));
	list_insert_tail(nolwb, zcw);
	ASSERT3P(zcw->zcw_lwb, ==, NULL);
}

void
zil_lwb_add_block(lwb_t *lwb, const blkptr_t *bp)
{
	avl_tree_t *t = &lwb->lwb_vdev_tree;
	avl_index_t where;
	zil_vdev_node_t *zv, zvsearch;
	int ndvas = BP_GET_NDVAS(bp);
	int i;

	ASSERT3S(lwb->lwb_state, !=, LWB_STATE_WRITE_DONE);
	ASSERT3S(lwb->lwb_state, !=, LWB_STATE_FLUSH_DONE);

	if (zil_nocacheflush)
		return;

	mutex_enter(&lwb->lwb_vdev_lock);
	for (i = 0; i < ndvas; i++) {
		zvsearch.zv_vdev = DVA_GET_VDEV(&bp->blk_dva[i]);
		if (avl_find(t, &zvsearch, &where) == NULL) {
			zv = kmem_alloc(sizeof (*zv), KM_SLEEP);
			zv->zv_vdev = zvsearch.zv_vdev;
			avl_insert(t, zv, where);
		}
	}
	mutex_exit(&lwb->lwb_vdev_lock);
}

static void
zil_lwb_flush_defer(lwb_t *lwb, lwb_t *nlwb)
{
	avl_tree_t *src = &lwb->lwb_vdev_tree;
	avl_tree_t *dst = &nlwb->lwb_vdev_tree;
	void *cookie = NULL;
	zil_vdev_node_t *zv;

	ASSERT3S(lwb->lwb_state, ==, LWB_STATE_WRITE_DONE);
	ASSERT3S(nlwb->lwb_state, !=, LWB_STATE_WRITE_DONE);
	ASSERT3S(nlwb->lwb_state, !=, LWB_STATE_FLUSH_DONE);

	/*
	 * While 'lwb' is at a point in its lifetime where lwb_vdev_tree does
	 * not need the protection of lwb_vdev_lock (it will only be modified
	 * while holding zilog->zl_lock) as its writes and those of its
	 * children have all completed.  The younger 'nlwb' may be waiting on
	 * future writes to additional vdevs.
	 */
	mutex_enter(&nlwb->lwb_vdev_lock);
	/*
	 * Tear down the 'lwb' vdev tree, ensuring that entries which do not
	 * exist in 'nlwb' are moved to it, freeing any would-be duplicates.
	 */
	while ((zv = avl_destroy_nodes(src, &cookie)) != NULL) {
		avl_index_t where;

		if (avl_find(dst, zv, &where) == NULL) {
			avl_insert(dst, zv, where);
		} else {
			kmem_free(zv, sizeof (*zv));
		}
	}
	mutex_exit(&nlwb->lwb_vdev_lock);
}

void
zil_lwb_add_txg(lwb_t *lwb, uint64_t txg)
{
	lwb->lwb_max_txg = MAX(lwb->lwb_max_txg, txg);
}

/*
 * This function is a called after all vdevs associated with a given lwb write
 * have completed their flush command; or as soon as the lwb write completes,
 * if "zil_nocacheflush" is set. Further, all "previous" lwb's will have
 * completed before this function is called; i.e. this function is called for
 * all previous lwbs before it's called for "this" lwb (enforced via zio the
 * dependencies configured in zil_lwb_set_zio_dependency()).
 *
 * The intention is for this function to be called as soon as the contents of
 * an lwb are considered "stable" on disk, and will survive any sudden loss of
 * power. At this point, any threads waiting for the lwb to reach this state
 * are signalled, and the "waiter" structures are marked "done".
 */
static void
zil_lwb_flush_vdevs_done(zio_t *zio)
{
	lwb_t *lwb = zio->io_private;
	zilog_t *zilog = lwb->lwb_zilog;
	zil_commit_waiter_t *zcw;
	itx_t *itx;

	spa_config_exit(zilog->zl_spa, SCL_STATE, lwb);

	hrtime_t t = gethrtime() - lwb->lwb_issued_timestamp;

	mutex_enter(&zilog->zl_lock);

	zilog->zl_last_lwb_latency = (zilog->zl_last_lwb_latency * 7 + t) / 8;

	lwb->lwb_root_zio = NULL;

	ASSERT3S(lwb->lwb_state, ==, LWB_STATE_WRITE_DONE);
	lwb->lwb_state = LWB_STATE_FLUSH_DONE;

	if (zilog->zl_last_lwb_opened == lwb) {
		/*
		 * Remember the highest committed log sequence number
		 * for ztest. We only update this value when all the log
		 * writes succeeded, because ztest wants to ASSERT that
		 * it got the whole log chain.
		 */
		zilog->zl_commit_lr_seq = zilog->zl_lr_seq;
	}

	while ((itx = list_remove_head(&lwb->lwb_itxs)) != NULL)
		zil_itx_destroy(itx);

	while ((zcw = list_remove_head(&lwb->lwb_waiters)) != NULL) {
		mutex_enter(&zcw->zcw_lock);

		ASSERT3P(zcw->zcw_lwb, ==, lwb);
		zcw->zcw_lwb = NULL;
		/*
		 * We expect any ZIO errors from child ZIOs to have been
		 * propagated "up" to this specific LWB's root ZIO, in
		 * order for this error handling to work correctly. This
		 * includes ZIO errors from either this LWB's write or
		 * flush, as well as any errors from other dependent LWBs
		 * (e.g. a root LWB ZIO that might be a child of this LWB).
		 *
		 * With that said, it's important to note that LWB flush
		 * errors are not propagated up to the LWB root ZIO.
		 * This is incorrect behavior, and results in VDEV flush
		 * errors not being handled correctly here. See the
		 * comment above the call to "zio_flush" for details.
		 */

		zcw->zcw_zio_error = zio->io_error;

		ASSERT3B(zcw->zcw_done, ==, B_FALSE);
		zcw->zcw_done = B_TRUE;
		cv_broadcast(&zcw->zcw_cv);

		mutex_exit(&zcw->zcw_lock);
	}

	uint64_t txg = lwb->lwb_issued_txg;

	/* Once we drop the lock, lwb may be freed by zil_sync(). */
	mutex_exit(&zilog->zl_lock);

	mutex_enter(&zilog->zl_lwb_io_lock);
	ASSERT3U(zilog->zl_lwb_inflight[txg & TXG_MASK], >, 0);
	zilog->zl_lwb_inflight[txg & TXG_MASK]--;
	if (zilog->zl_lwb_inflight[txg & TXG_MASK] == 0)
		cv_broadcast(&zilog->zl_lwb_io_cv);
	mutex_exit(&zilog->zl_lwb_io_lock);
}

/*
 * Wait for the completion of all issued write/flush of that txg provided.
 * It guarantees zil_lwb_flush_vdevs_done() is called and returned.
 */
static void
zil_lwb_flush_wait_all(zilog_t *zilog, uint64_t txg)
{
	ASSERT3U(txg, ==, spa_syncing_txg(zilog->zl_spa));

	mutex_enter(&zilog->zl_lwb_io_lock);
	while (zilog->zl_lwb_inflight[txg & TXG_MASK] > 0)
		cv_wait(&zilog->zl_lwb_io_cv, &zilog->zl_lwb_io_lock);
	mutex_exit(&zilog->zl_lwb_io_lock);

#ifdef ZFS_DEBUG
	mutex_enter(&zilog->zl_lock);
	mutex_enter(&zilog->zl_lwb_io_lock);
	lwb_t *lwb = list_head(&zilog->zl_lwb_list);
	while (lwb != NULL) {
		if (lwb->lwb_issued_txg <= txg) {
			ASSERT(lwb->lwb_state != LWB_STATE_ISSUED);
			ASSERT(lwb->lwb_state != LWB_STATE_WRITE_DONE);
			IMPLY(lwb->lwb_issued_txg > 0,
			    lwb->lwb_state == LWB_STATE_FLUSH_DONE);
		}
		IMPLY(lwb->lwb_state == LWB_STATE_WRITE_DONE ||
		    lwb->lwb_state == LWB_STATE_FLUSH_DONE,
		    lwb->lwb_buf == NULL);
		lwb = list_next(&zilog->zl_lwb_list, lwb);
	}
	mutex_exit(&zilog->zl_lwb_io_lock);
	mutex_exit(&zilog->zl_lock);
#endif
}

/*
 * This is called when an lwb's write zio completes. The callback's purpose is
 * to issue the flush commands for the vdevs in the lwb's lwb_vdev_tree. The
 * tree will contain the vdevs involved in writing out this specific lwb's
 * data, and in the case that cache flushes have been deferred, vdevs involved
 * in writing the data for previous lwbs. The writes corresponding to all the
 * vdevs in the lwb_vdev_tree will have completed by the time this is called,
 * due to the zio dependencies configured in zil_lwb_set_zio_dependency(),
 * which takes deferred flushes into account. The lwb will be "done" once
 * zil_lwb_flush_vdevs_done() is called, which occurs in the zio completion
 * callback for the lwb's root zio.
 */
static void
zil_lwb_write_done(zio_t *zio)
{
	lwb_t *lwb = zio->io_private;
	spa_t *spa = zio->io_spa;
	zilog_t *zilog = lwb->lwb_zilog;
	avl_tree_t *t = &lwb->lwb_vdev_tree;
	void *cookie = NULL;
	zil_vdev_node_t *zv;
	lwb_t *nlwb;

	ASSERT3S(spa_config_held(spa, SCL_STATE, RW_READER), !=, 0);

	abd_free(zio->io_abd);
	zio_buf_free(lwb->lwb_buf, lwb->lwb_sz);
	lwb->lwb_buf = NULL;

	mutex_enter(&zilog->zl_lock);
	ASSERT3S(lwb->lwb_state, ==, LWB_STATE_ISSUED);
	lwb->lwb_state = LWB_STATE_WRITE_DONE;
	lwb->lwb_child_zio = NULL;
	lwb->lwb_write_zio = NULL;

	/*
	 * If nlwb is not yet issued, zil_lwb_set_zio_dependency() is not
	 * called for it yet, and when it will be, it won't be able to make
	 * its write ZIO a parent this ZIO.  In such case we can not defer
	 * our flushes or below may be a race between the done callbacks.
	 */
	nlwb = list_next(&zilog->zl_lwb_list, lwb);
	if (nlwb && nlwb->lwb_state != LWB_STATE_ISSUED)
		nlwb = NULL;
	mutex_exit(&zilog->zl_lock);

	if (avl_numnodes(t) == 0)
		return;

	/*
	 * If there was an IO error, we're not going to call zio_flush()
	 * on these vdevs, so we simply empty the tree and free the
	 * nodes. We avoid calling zio_flush() since there isn't any
	 * good reason for doing so, after the lwb block failed to be
	 * written out.
	 *
	 * Additionally, we don't perform any further error handling at
	 * this point (e.g. setting "zcw_zio_error" appropriately), as
	 * we expect that to occur in "zil_lwb_flush_vdevs_done" (thus,
	 * we expect any error seen here, to have been propagated to
	 * that function).
	 */
	if (zio->io_error != 0) {
		while ((zv = avl_destroy_nodes(t, &cookie)) != NULL)
			kmem_free(zv, sizeof (*zv));
		return;
	}

	/*
	 * If this lwb does not have any threads waiting for it to complete, we
	 * want to defer issuing the flush command to the vdevs written to by
	 * "this" lwb, and instead rely on the "next" lwb to handle the flush
	 * command for those vdevs. Thus, we merge the vdev tree of "this" lwb
	 * with the vdev tree of the "next" lwb in the list, and assume the
	 * "next" lwb will handle flushing the vdevs (or deferring the flush(s)
	 * again).
	 *
	 * This is a useful performance optimization, especially for workloads
	 * with lots of async write activity and few sync write and/or fsync
	 * activity, as it has the potential to coalesce multiple flush
	 * commands to a vdev into one.
	 */
	if (list_is_empty(&lwb->lwb_waiters) && nlwb != NULL) {
		zil_lwb_flush_defer(lwb, nlwb);
		ASSERT(avl_is_empty(&lwb->lwb_vdev_tree));
		return;
	}

	while ((zv = avl_destroy_nodes(t, &cookie)) != NULL) {
		vdev_t *vd = vdev_lookup_top(spa, zv->zv_vdev);
		if (vd != NULL) {
			/*
			 * The "ZIO_FLAG_DONT_PROPAGATE" is currently
			 * always used within "zio_flush". This means,
			 * any errors when flushing the vdev(s), will
			 * (unfortunately) not be handled correctly,
			 * since these "zio_flush" errors will not be
			 * propagated up to "zil_lwb_flush_vdevs_done".
			 */
			zio_flush(lwb->lwb_root_zio, vd);
		}
		kmem_free(zv, sizeof (*zv));
	}
}

/*
 * Build the zio dependency chain, which is used to preserve the ordering of
 * lwb completions that is required by the semantics of the ZIL. Each new lwb
 * zio becomes a parent of the previous lwb zio, such that the new lwb's zio
 * cannot complete until the previous lwb's zio completes.
 *
 * This is required by the semantics of zil_commit(): the commit waiters
 * attached to the lwbs will be woken in the lwb zio's completion callback,
 * so this zio dependency graph ensures the waiters are woken in the correct
 * order (the same order the lwbs were created).
 */
static void
zil_lwb_set_zio_dependency(zilog_t *zilog, lwb_t *lwb)
{
	ASSERT(MUTEX_HELD(&zilog->zl_lock));

	lwb_t *prev_lwb = list_prev(&zilog->zl_lwb_list, lwb);
	if (prev_lwb == NULL ||
	    prev_lwb->lwb_state == LWB_STATE_FLUSH_DONE)
		return;

	/*
	 * If the previous lwb's write hasn't already completed, we also want
	 * to order the completion of the lwb write zios (above, we only order
	 * the completion of the lwb root zios). This is required because of
	 * how we can defer the flush commands for any lwb without waiters.
	 *
	 * When the flush commands are deferred, the previous lwb will rely on
	 * this lwb to flush the vdevs written to by that previous lwb. Thus,
	 * we need to ensure this lwb doesn't issue the flush until after the
	 * previous lwb's write completes. We ensure this ordering by setting
	 * the zio parent/child relationship here.
	 *
	 * Without this relationship on the lwb's write zio, it's possible for
	 * this lwb's write to complete prior to the previous lwb's write
	 * completing; and thus, the vdevs for the previous lwb would be
	 * flushed prior to that lwb's data being written to those vdevs (the
	 * vdevs are flushed in the lwb write zio's completion handler,
	 * zil_lwb_write_done()).
	 */
	if (prev_lwb->lwb_state == LWB_STATE_ISSUED) {
		ASSERT3P(prev_lwb->lwb_write_zio, !=, NULL);
		if (list_is_empty(&prev_lwb->lwb_waiters)) {
			zio_add_child(lwb->lwb_write_zio,
			    prev_lwb->lwb_write_zio);
		}
	} else {
		ASSERT3S(prev_lwb->lwb_state, ==, LWB_STATE_WRITE_DONE);
	}

	ASSERT3P(prev_lwb->lwb_root_zio, !=, NULL);
	zio_add_child(lwb->lwb_root_zio, prev_lwb->lwb_root_zio);
}


/*
 * This function's purpose is to "open" an lwb such that it is ready to
 * accept new itxs being committed to it. This function is idempotent; if
 * the passed in lwb has already been opened, it is essentially a no-op.
 */
static void
zil_lwb_write_open(zilog_t *zilog, lwb_t *lwb)
{
	ASSERT(MUTEX_HELD(&zilog->zl_issuer_lock));

	if (lwb->lwb_state != LWB_STATE_NEW) {
		ASSERT3S(lwb->lwb_state, ==, LWB_STATE_OPENED);
		return;
	}

	mutex_enter(&zilog->zl_lock);
	lwb->lwb_state = LWB_STATE_OPENED;
	zilog->zl_last_lwb_opened = lwb;
	mutex_exit(&zilog->zl_lock);
}

/*
 * Maximum block size used by the ZIL.  This is picked up when the ZIL is
 * initialized.  Otherwise this should not be used directly; see
 * zl_max_block_size instead.
 */
static uint_t zil_maxblocksize = SPA_OLD_MAXBLOCKSIZE;

/*
 * Plan splitting of the provided burst size between several blocks.
 */
static uint_t
zil_lwb_plan(zilog_t *zilog, uint64_t size, uint_t *minsize)
{
	uint_t md = zilog->zl_max_block_size - sizeof (zil_chain_t);

	if (size <= md) {
		/*
		 * Small bursts are written as-is in one block.
		 */
		*minsize = size;
		return (size);
	} else if (size > 8 * md) {
		/*
		 * Big bursts use maximum blocks.  The first block size
		 * is hard to predict, but it does not really matter.
		 */
		*minsize = 0;
		return (md);
	}

	/*
	 * Medium bursts try to divide evenly to better utilize several SLOG
	 * VDEVs.  The first block size we predict assuming the worst case of
	 * maxing out others.  Fall back to using maximum blocks if due to
	 * large records or wasted space we can not predict anything better.
	 */
	uint_t s = size;
	uint_t n = DIV_ROUND_UP(s, md - sizeof (lr_write_t));
	uint_t chunk = DIV_ROUND_UP(s, n);
	uint_t waste = zil_max_waste_space(zilog);
	waste = MAX(waste, zilog->zl_cur_max);
	if (chunk <= md - waste) {
		*minsize = MAX(s - (md - waste) * (n - 1), waste);
		return (chunk);
	} else {
		*minsize = 0;
		return (md);
	}
}

/*
 * Try to predict next block size based on previous history.  Make prediction
 * sufficient for 7 of 8 previous bursts.  Don't try to save if the saving is
 * less then 50%, extra writes may cost more, but we don't want single spike
 * to badly affect our predictions.
 */
static uint_t
zil_lwb_predict(zilog_t *zilog)
{
	uint_t m, o;

	/* If we are in the middle of a burst, take it into account also. */
	if (zilog->zl_cur_size > 0) {
		o = zil_lwb_plan(zilog, zilog->zl_cur_size, &m);
	} else {
		o = UINT_MAX;
		m = 0;
	}

	/* Find minimum optimal size.  We don't need to go below that. */
	for (int i = 0; i < ZIL_BURSTS; i++)
		o = MIN(o, zilog->zl_prev_opt[i]);

	/* Find two biggest minimal first block sizes above the optimal. */
	uint_t m1 = MAX(m, o), m2 = o;
	for (int i = 0; i < ZIL_BURSTS; i++) {
		m = zilog->zl_prev_min[i];
		if (m >= m1) {
			m2 = m1;
			m1 = m;
		} else if (m > m2) {
			m2 = m;
		}
	}

	/*
	 * If second minimum size gives 50% saving -- use it.  It may cost us
	 * one additional write later, but the space saving is just too big.
	 */
	return ((m1 < m2 * 2) ? m1 : m2);
}

/*
 * Close the log block for being issued and allocate the next one.
 * Has to be called under zl_issuer_lock to chain more lwbs.
 */
static lwb_t *
zil_lwb_write_close(zilog_t *zilog, lwb_t *lwb, lwb_state_t state)
{
	uint64_t blksz, plan, plan2;

	ASSERT(MUTEX_HELD(&zilog->zl_issuer_lock));
	ASSERT3S(lwb->lwb_state, ==, LWB_STATE_OPENED);
	lwb->lwb_state = LWB_STATE_CLOSED;

	/*
	 * If there was an allocation failure then returned NULL will trigger
	 * zil_commit_writer_stall() at the caller.  This is inherently racy,
	 * since allocation may not have happened yet.
	 */
	if (lwb->lwb_error != 0)
		return (NULL);

	/*
	 * Log blocks are pre-allocated.  Here we select the size of the next
	 * block, based on what's left of this burst and the previous history.
	 * While we try to only write used part of the block, we can't just
	 * always allocate the maximum block size because we can exhaust all
	 * available pool log space, so we try to be reasonable.
	 */
	if (zilog->zl_cur_left > 0) {
		/*
		 * We are in the middle of a burst and know how much is left.
		 * But if workload is multi-threaded there may be more soon.
		 * Try to predict what can it be and plan for the worst case.
		 */
		uint_t m;
		plan = zil_lwb_plan(zilog, zilog->zl_cur_left, &m);
		if (zilog->zl_parallel) {
			plan2 = zil_lwb_plan(zilog, zilog->zl_cur_left +
			    zil_lwb_predict(zilog), &m);
			if (plan < plan2)
				plan = plan2;
		}
	} else {
		/*
		 * The previous burst is done and we can only predict what
		 * will come next.
		 */
		plan = zil_lwb_predict(zilog);
	}
	blksz = plan + sizeof (zil_chain_t);
	blksz = P2ROUNDUP_TYPED(blksz, ZIL_MIN_BLKSZ, uint64_t);
	blksz = MIN(blksz, zilog->zl_max_block_size);
	DTRACE_PROBE3(zil__block__size, zilog_t *, zilog, uint64_t, blksz,
	    uint64_t, plan);

	return (zil_alloc_lwb(zilog, blksz, NULL, 0, 0, state));
}

/*
 * Finalize previously closed block and issue the write zio.
 */
static void
zil_lwb_write_issue(zilog_t *zilog, lwb_t *lwb)
{
	spa_t *spa = zilog->zl_spa;
	zil_chain_t *zilc;
	boolean_t slog;
	zbookmark_phys_t zb;
	zio_priority_t prio;
	int error;

	ASSERT3S(lwb->lwb_state, ==, LWB_STATE_CLOSED);

	/* Actually fill the lwb with the data. */
	for (itx_t *itx = list_head(&lwb->lwb_itxs); itx;
	    itx = list_next(&lwb->lwb_itxs, itx))
		zil_lwb_commit(zilog, lwb, itx);
	lwb->lwb_nused = lwb->lwb_nfilled;
	ASSERT3U(lwb->lwb_nused, <=, lwb->lwb_nmax);

	lwb->lwb_root_zio = zio_root(spa, zil_lwb_flush_vdevs_done, lwb,
	    ZIO_FLAG_CANFAIL);

	/*
	 * The lwb is now ready to be issued, but it can be only if it already
	 * got its block pointer allocated or the allocation has failed.
	 * Otherwise leave it as-is, relying on some other thread to issue it
	 * after allocating its block pointer via calling zil_lwb_write_issue()
	 * for the previous lwb(s) in the chain.
	 */
	mutex_enter(&zilog->zl_lock);
	lwb->lwb_state = LWB_STATE_READY;
	if (BP_IS_HOLE(&lwb->lwb_blk) && lwb->lwb_error == 0) {
		mutex_exit(&zilog->zl_lock);
		return;
	}
	mutex_exit(&zilog->zl_lock);

next_lwb:
	if (lwb->lwb_slim)
		zilc = (zil_chain_t *)lwb->lwb_buf;
	else
		zilc = (zil_chain_t *)(lwb->lwb_buf + lwb->lwb_nmax);
	int wsz = lwb->lwb_sz;
	if (lwb->lwb_error == 0) {
		abd_t *lwb_abd = abd_get_from_buf(lwb->lwb_buf, lwb->lwb_sz);
		if (!lwb->lwb_slog || zilog->zl_cur_size <= zil_slog_bulk)
			prio = ZIO_PRIORITY_SYNC_WRITE;
		else
			prio = ZIO_PRIORITY_ASYNC_WRITE;
		SET_BOOKMARK(&zb, lwb->lwb_blk.blk_cksum.zc_word[ZIL_ZC_OBJSET],
		    ZB_ZIL_OBJECT, ZB_ZIL_LEVEL,
		    lwb->lwb_blk.blk_cksum.zc_word[ZIL_ZC_SEQ]);
		lwb->lwb_write_zio = zio_rewrite(lwb->lwb_root_zio, spa, 0,
		    &lwb->lwb_blk, lwb_abd, lwb->lwb_sz, zil_lwb_write_done,
		    lwb, prio, ZIO_FLAG_CANFAIL, &zb);
		zil_lwb_add_block(lwb, &lwb->lwb_blk);

		if (lwb->lwb_slim) {
			/* For Slim ZIL only write what is used. */
			wsz = P2ROUNDUP_TYPED(lwb->lwb_nused, ZIL_MIN_BLKSZ,
			    int);
			ASSERT3S(wsz, <=, lwb->lwb_sz);
			zio_shrink(lwb->lwb_write_zio, wsz);
			wsz = lwb->lwb_write_zio->io_size;
		}
		memset(lwb->lwb_buf + lwb->lwb_nused, 0, wsz - lwb->lwb_nused);
		zilc->zc_pad = 0;
		zilc->zc_nused = lwb->lwb_nused;
		zilc->zc_eck.zec_cksum = lwb->lwb_blk.blk_cksum;
	} else {
		/*
		 * We can't write the lwb if there was an allocation failure,
		 * so create a null zio instead just to maintain dependencies.
		 */
		lwb->lwb_write_zio = zio_null(lwb->lwb_root_zio, spa, NULL,
		    zil_lwb_write_done, lwb, ZIO_FLAG_CANFAIL);
		lwb->lwb_write_zio->io_error = lwb->lwb_error;
	}
	if (lwb->lwb_child_zio)
		zio_add_child(lwb->lwb_write_zio, lwb->lwb_child_zio);

	/*
	 * Open transaction to allocate the next block pointer.
	 */
	dmu_tx_t *tx = dmu_tx_create(zilog->zl_os);
	VERIFY0(dmu_tx_assign(tx,
	    DMU_TX_WAIT | DMU_TX_NOTHROTTLE | DMU_TX_SUSPEND));
	dsl_dataset_dirty(dmu_objset_ds(zilog->zl_os), tx);
	uint64_t txg = dmu_tx_get_txg(tx);

	/*
	 * Allocate next the block pointer unless we are already in error.
	 */
	lwb_t *nlwb = list_next(&zilog->zl_lwb_list, lwb);
	blkptr_t *bp = &zilc->zc_next_blk;
	BP_ZERO(bp);
	error = lwb->lwb_error;
	if (error == 0) {
		error = zio_alloc_zil(spa, zilog->zl_os, txg, bp, nlwb->lwb_sz,
		    &slog);
	}
	if (error == 0) {
		ASSERT3U(BP_GET_LOGICAL_BIRTH(bp), ==, txg);
		BP_SET_CHECKSUM(bp, nlwb->lwb_slim ? ZIO_CHECKSUM_ZILOG2 :
		    ZIO_CHECKSUM_ZILOG);
		bp->blk_cksum = lwb->lwb_blk.blk_cksum;
		bp->blk_cksum.zc_word[ZIL_ZC_SEQ]++;
	}

	/*
	 * Reduce TXG open time by incrementing inflight counter and committing
	 * the transaciton.  zil_sync() will wait for it to return to zero.
	 */
	mutex_enter(&zilog->zl_lwb_io_lock);
	lwb->lwb_issued_txg = txg;
	zilog->zl_lwb_inflight[txg & TXG_MASK]++;
	zilog->zl_lwb_max_issued_txg = MAX(txg, zilog->zl_lwb_max_issued_txg);
	mutex_exit(&zilog->zl_lwb_io_lock);
	dmu_tx_commit(tx);

	spa_config_enter(spa, SCL_STATE, lwb, RW_READER);

	/*
	 * We've completed all potentially blocking operations.  Update the
	 * nlwb and allow it proceed without possible lock order reversals.
	 */
	mutex_enter(&zilog->zl_lock);
	zil_lwb_set_zio_dependency(zilog, lwb);
	lwb->lwb_state = LWB_STATE_ISSUED;

	if (nlwb) {
		nlwb->lwb_blk = *bp;
		nlwb->lwb_error = error;
		nlwb->lwb_slog = slog;
		nlwb->lwb_alloc_txg = txg;
		if (nlwb->lwb_state != LWB_STATE_READY)
			nlwb = NULL;
	}
	mutex_exit(&zilog->zl_lock);

	if (lwb->lwb_slog) {
		ZIL_STAT_BUMP(zilog, zil_itx_metaslab_slog_count);
		ZIL_STAT_INCR(zilog, zil_itx_metaslab_slog_bytes,
		    lwb->lwb_nused);
		ZIL_STAT_INCR(zilog, zil_itx_metaslab_slog_write,
		    wsz);
		ZIL_STAT_INCR(zilog, zil_itx_metaslab_slog_alloc,
		    BP_GET_LSIZE(&lwb->lwb_blk));
	} else {
		ZIL_STAT_BUMP(zilog, zil_itx_metaslab_normal_count);
		ZIL_STAT_INCR(zilog, zil_itx_metaslab_normal_bytes,
		    lwb->lwb_nused);
		ZIL_STAT_INCR(zilog, zil_itx_metaslab_normal_write,
		    wsz);
		ZIL_STAT_INCR(zilog, zil_itx_metaslab_normal_alloc,
		    BP_GET_LSIZE(&lwb->lwb_blk));
	}
	lwb->lwb_issued_timestamp = gethrtime();
	if (lwb->lwb_child_zio)
		zio_nowait(lwb->lwb_child_zio);
	zio_nowait(lwb->lwb_write_zio);
	zio_nowait(lwb->lwb_root_zio);

	/*
	 * If nlwb was ready when we gave it the block pointer,
	 * it is on us to issue it and possibly following ones.
	 */
	lwb = nlwb;
	if (lwb)
		goto next_lwb;
}

/*
 * Maximum amount of data that can be put into single log block.
 */
uint64_t
zil_max_log_data(zilog_t *zilog, size_t hdrsize)
{
	return (zilog->zl_max_block_size - sizeof (zil_chain_t) - hdrsize);
}

/*
 * Maximum amount of log space we agree to waste to reduce number of
 * WR_NEED_COPY chunks to reduce zl_get_data() overhead (~6%).
 */
static inline uint64_t
zil_max_waste_space(zilog_t *zilog)
{
	return (zil_max_log_data(zilog, sizeof (lr_write_t)) / 16);
}

/*
 * Maximum amount of write data for WR_COPIED.  For correctness, consumers
 * must fall back to WR_NEED_COPY if we can't fit the entire record into one
 * maximum sized log block, because each WR_COPIED record must fit in a
 * single log block.  Below that it is a tradeoff of additional memory copy
 * and possibly worse log space efficiency vs additional range lock/unlock.
 */
static uint_t zil_maxcopied = 7680;

/*
 * Largest write size to store the data directly into ZIL.
 */
uint_t zfs_immediate_write_sz = 32768;

/*
 * When enabled and blocks go to normal vdev, treat special vdevs as SLOG,
 * writing data to ZIL (WR_COPIED/WR_NEED_COPY).  Disabling this forces the
 * indirect writes (WR_INDIRECT) to preserve special vdev throughput and
 * endurance, likely at the cost of normal vdev latency.
 */
int zil_special_is_slog = 1;

uint64_t
zil_max_copied_data(zilog_t *zilog)
{
	uint64_t max_data = zil_max_log_data(zilog, sizeof (lr_write_t));
	return (MIN(max_data, zil_maxcopied));
}

/*
 * Determine the appropriate write state for ZIL transactions based on
 * pool configuration, data placement, write size, and logbias settings.
 */
itx_wr_state_t
zil_write_state(zilog_t *zilog, uint64_t size, uint32_t blocksize,
    boolean_t o_direct, boolean_t commit)
{
	if (zilog->zl_logbias == ZFS_LOGBIAS_THROUGHPUT || o_direct)
		return (WR_INDIRECT);

	/*
	 * Don't use indirect for too small writes to reduce overhead.
	 * Don't use indirect if written less than a half of a block if
	 * we are going to commit it immediately, since next write might
	 * rewrite the same block again, causing inflation.  If commit
	 * is not planned, then next writes might coalesce, and so the
	 * indirect may be perfect.
	 */
	boolean_t indirect = (size >= zfs_immediate_write_sz &&
	    (size >= blocksize / 2 || !commit));

	if (spa_has_slogs(zilog->zl_spa)) {
		/* Dedicated slogs: never use indirect */
		indirect = B_FALSE;
	} else if (spa_has_special(zilog->zl_spa)) {
		/* Special vdevs: only when beneficial */
		boolean_t on_special = (blocksize <=
		    zilog->zl_os->os_zpl_special_smallblock);
		indirect &= (on_special || !zil_special_is_slog);
	}

	if (indirect)
		return (WR_INDIRECT);
	else if (commit)
		return (WR_COPIED);
	else
		return (WR_NEED_COPY);
}

static uint64_t
zil_itx_record_size(itx_t *itx)
{
	lr_t *lr = &itx->itx_lr;

	if (lr->lrc_txtype == TX_COMMIT)
		return (0);
	ASSERT3U(lr->lrc_reclen, >=, sizeof (lr_t));
	return (lr->lrc_reclen);
}

static uint64_t
zil_itx_data_size(itx_t *itx)
{
	lr_t *lr = &itx->itx_lr;
	lr_write_t *lrw = (lr_write_t *)lr;

	if (lr->lrc_txtype == TX_WRITE && itx->itx_wr_state == WR_NEED_COPY) {
		ASSERT3U(lr->lrc_reclen, ==, sizeof (lr_write_t));
		return (P2ROUNDUP_TYPED(lrw->lr_length, sizeof (uint64_t),
		    uint64_t));
	}
	return (0);
}

static uint64_t
zil_itx_full_size(itx_t *itx)
{
	lr_t *lr = &itx->itx_lr;

	if (lr->lrc_txtype == TX_COMMIT)
		return (0);
	ASSERT3U(lr->lrc_reclen, >=, sizeof (lr_t));
	return (lr->lrc_reclen + zil_itx_data_size(itx));
}

/*
 * Estimate space needed in the lwb for the itx.  Allocate more lwbs or
 * split the itx as needed, but don't touch the actual transaction data.
 * Has to be called under zl_issuer_lock to call zil_lwb_write_close()
 * to chain more lwbs.
 */
static lwb_t *
zil_lwb_assign(zilog_t *zilog, lwb_t *lwb, itx_t *itx, list_t *ilwbs)
{
	itx_t *citx;
	lr_t *lr, *clr;
	lr_write_t *lrw;
	uint64_t dlen, dnow, lwb_sp, reclen, max_log_data;

	ASSERT(MUTEX_HELD(&zilog->zl_issuer_lock));
	ASSERT3P(lwb, !=, NULL);
	ASSERT3P(lwb->lwb_buf, !=, NULL);

	zil_lwb_write_open(zilog, lwb);

	lr = &itx->itx_lr;
	lrw = (lr_write_t *)lr;

	/*
	 * A commit itx doesn't represent any on-disk state; instead
	 * it's simply used as a place holder on the commit list, and
	 * provides a mechanism for attaching a "commit waiter" onto the
	 * correct lwb (such that the waiter can be signalled upon
	 * completion of that lwb). Thus, we don't process this itx's
	 * log record if it's a commit itx (these itx's don't have log
	 * records), and instead link the itx's waiter onto the lwb's
	 * list of waiters.
	 *
	 * For more details, see the comment above zil_commit().
	 */
	if (lr->lrc_txtype == TX_COMMIT) {
		zil_commit_waiter_link_lwb(itx->itx_private, lwb);
		list_insert_tail(&lwb->lwb_itxs, itx);
		return (lwb);
	}

	reclen = lr->lrc_reclen;
	ASSERT3U(reclen, >=, sizeof (lr_t));
	ASSERT3U(reclen, <=, zil_max_log_data(zilog, 0));
	dlen = zil_itx_data_size(itx);

cont:
	/*
	 * If this record won't fit in the current log block, start a new one.
	 * For WR_NEED_COPY optimize layout for minimal number of chunks.
	 */
	lwb_sp = lwb->lwb_nmax - lwb->lwb_nused;
	max_log_data = zil_max_log_data(zilog, sizeof (lr_write_t));
	if (reclen > lwb_sp || (reclen + dlen > lwb_sp &&
	    lwb_sp < zil_max_waste_space(zilog) &&
	    (dlen % max_log_data == 0 ||
	    lwb_sp < reclen + dlen % max_log_data))) {
		list_insert_tail(ilwbs, lwb);
		lwb = zil_lwb_write_close(zilog, lwb, LWB_STATE_OPENED);
		if (lwb == NULL)
			return (NULL);
		lwb_sp = lwb->lwb_nmax - lwb->lwb_nused;
	}

	/*
	 * There must be enough space in the log block to hold reclen.
	 * For WR_COPIED, we need to fit the whole record in one block,
	 * and reclen is the write record header size + the data size.
	 * For WR_NEED_COPY, we can create multiple records, splitting
	 * the data into multiple blocks, so we only need to fit one
	 * word of data per block; in this case reclen is just the header
	 * size (no data).
	 */
	ASSERT3U(reclen + MIN(dlen, sizeof (uint64_t)), <=, lwb_sp);

	dnow = MIN(dlen, lwb_sp - reclen);
	if (dlen > dnow) {
		ASSERT3U(lr->lrc_txtype, ==, TX_WRITE);
		ASSERT3U(itx->itx_wr_state, ==, WR_NEED_COPY);
		citx = zil_itx_clone(itx);
		clr = &citx->itx_lr;
		lr_write_t *clrw = (lr_write_t *)clr;
		clrw->lr_length = dnow;
		lrw->lr_offset += dnow;
		lrw->lr_length -= dnow;
		zilog->zl_cur_left -= dnow;
	} else {
		citx = itx;
		clr = lr;
	}

	/*
	 * We're actually making an entry, so update lrc_seq to be the
	 * log record sequence number.  Note that this is generally not
	 * equal to the itx sequence number because not all transactions
	 * are synchronous, and sometimes spa_sync() gets there first.
	 */
	clr->lrc_seq = ++zilog->zl_lr_seq;

	lwb->lwb_nused += reclen + dnow;
	ASSERT3U(lwb->lwb_nused, <=, lwb->lwb_nmax);
	ASSERT0(P2PHASE(lwb->lwb_nused, sizeof (uint64_t)));

	zil_lwb_add_txg(lwb, lr->lrc_txg);
	list_insert_tail(&lwb->lwb_itxs, citx);

	dlen -= dnow;
	if (dlen > 0)
		goto cont;

	if (lr->lrc_txtype == TX_WRITE &&
	    lr->lrc_txg > spa_freeze_txg(zilog->zl_spa))
		txg_wait_synced(zilog->zl_dmu_pool, lr->lrc_txg);

	return (lwb);
}

/*
 * Fill the actual transaction data into the lwb, following zil_lwb_assign().
 * Does not require locking.
 */
static void
zil_lwb_commit(zilog_t *zilog, lwb_t *lwb, itx_t *itx)
{
	lr_t *lr, *lrb;
	lr_write_t *lrw, *lrwb;
	char *lr_buf;
	uint64_t dlen, reclen;

	lr = &itx->itx_lr;
	lrw = (lr_write_t *)lr;

	if (lr->lrc_txtype == TX_COMMIT)
		return;

	reclen = lr->lrc_reclen;
	dlen = zil_itx_data_size(itx);
	ASSERT3U(reclen + dlen, <=, lwb->lwb_nused - lwb->lwb_nfilled);

	lr_buf = lwb->lwb_buf + lwb->lwb_nfilled;
	memcpy(lr_buf, lr, reclen);
	lrb = (lr_t *)lr_buf;		/* Like lr, but inside lwb. */
	lrwb = (lr_write_t *)lrb;	/* Like lrw, but inside lwb. */

	ZIL_STAT_BUMP(zilog, zil_itx_count);

	/*
	 * If it's a write, fetch the data or get its blkptr as appropriate.
	 */
	if (lr->lrc_txtype == TX_WRITE) {
		if (itx->itx_wr_state == WR_COPIED) {
			ZIL_STAT_BUMP(zilog, zil_itx_copied_count);
			ZIL_STAT_INCR(zilog, zil_itx_copied_bytes,
			    lrw->lr_length);
		} else {
			char *dbuf;
			int error;

			if (itx->itx_wr_state == WR_NEED_COPY) {
				dbuf = lr_buf + reclen;
				lrb->lrc_reclen += dlen;
				ZIL_STAT_BUMP(zilog, zil_itx_needcopy_count);
				ZIL_STAT_INCR(zilog, zil_itx_needcopy_bytes,
				    dlen);
			} else {
				ASSERT3S(itx->itx_wr_state, ==, WR_INDIRECT);
				dbuf = NULL;
				ZIL_STAT_BUMP(zilog, zil_itx_indirect_count);
				ZIL_STAT_INCR(zilog, zil_itx_indirect_bytes,
				    lrw->lr_length);
				if (lwb->lwb_child_zio == NULL) {
					lwb->lwb_child_zio = zio_null(NULL,
					    zilog->zl_spa, NULL, NULL, NULL,
					    ZIO_FLAG_CANFAIL);
				}
			}

			/*
			 * The "lwb_child_zio" we pass in will become a child of
			 * "lwb_write_zio", when one is created, so one will be
			 * a parent of any zio's created by the "zl_get_data".
			 * This way "lwb_write_zio" will first wait for children
			 * block pointers before own writing, and then for their
			 * writing completion before the vdev cache flushing.
			 */
			error = zilog->zl_get_data(itx->itx_private,
			    itx->itx_gen, lrwb, dbuf, lwb,
			    lwb->lwb_child_zio);
			if (dbuf != NULL && error == 0) {
				/* Zero any padding bytes in the last block. */
				memset((char *)dbuf + lrwb->lr_length, 0,
				    dlen - lrwb->lr_length);
			}

			/*
			 * Typically, the only return values we should see from
			 * ->zl_get_data() are 0, EIO, ENOENT, EEXIST or
			 *  EALREADY. However, it is also possible to see other
			 *  error values such as ENOSPC or EINVAL from
			 *  dmu_read() -> dnode_hold() -> dnode_hold_impl() or
			 *  ENXIO as well as a multitude of others from the
			 *  block layer through dmu_buf_hold() -> dbuf_read()
			 *  -> zio_wait(), as well as through dmu_read() ->
			 *  dnode_hold() -> dnode_hold_impl() -> dbuf_read() ->
			 *  zio_wait(). When these errors happen, we can assume
			 *  that neither an immediate write nor an indirect
			 *  write occurred, so we need to fall back to
			 *  txg_wait_synced(). This is unusual, so we print to
			 *  dmesg whenever one of these errors occurs.
			 */
			switch (error) {
			case 0:
				break;
			default:
				cmn_err(CE_WARN, "zil_lwb_commit() received "
				    "unexpected error %d from ->zl_get_data()"
				    ". Falling back to txg_wait_synced().",
				    error);
				zfs_fallthrough;
			case EIO:
				txg_wait_synced(zilog->zl_dmu_pool,
				    lr->lrc_txg);
				zfs_fallthrough;
			case ENOENT:
				zfs_fallthrough;
			case EEXIST:
				zfs_fallthrough;
			case EALREADY:
				return;
			}
		}
	}

	lwb->lwb_nfilled += reclen + dlen;
	ASSERT3S(lwb->lwb_nfilled, <=, lwb->lwb_nused);
	ASSERT0(P2PHASE(lwb->lwb_nfilled, sizeof (uint64_t)));
}

itx_t *
zil_itx_create(uint64_t txtype, size_t olrsize)
{
	size_t itxsize, lrsize;
	itx_t *itx;

	ASSERT3U(olrsize, >=, sizeof (lr_t));
	lrsize = P2ROUNDUP_TYPED(olrsize, sizeof (uint64_t), size_t);
	ASSERT3U(lrsize, >=, olrsize);
	itxsize = offsetof(itx_t, itx_lr) + lrsize;

	itx = zio_data_buf_alloc(itxsize);
	itx->itx_lr.lrc_txtype = txtype;
	itx->itx_lr.lrc_reclen = lrsize;
	itx->itx_lr.lrc_seq = 0;	/* defensive */
	memset((char *)&itx->itx_lr + olrsize, 0, lrsize - olrsize);
	itx->itx_sync = B_TRUE;		/* default is synchronous */
	itx->itx_callback = NULL;
	itx->itx_callback_data = NULL;
	itx->itx_size = itxsize;

	return (itx);
}

static itx_t *
zil_itx_clone(itx_t *oitx)
{
	ASSERT3U(oitx->itx_size, >=, sizeof (itx_t));
	ASSERT3U(oitx->itx_size, ==,
	    offsetof(itx_t, itx_lr) + oitx->itx_lr.lrc_reclen);

	itx_t *itx = zio_data_buf_alloc(oitx->itx_size);
	memcpy(itx, oitx, oitx->itx_size);
	itx->itx_callback = NULL;
	itx->itx_callback_data = NULL;
	return (itx);
}

void
zil_itx_destroy(itx_t *itx)
{
	ASSERT3U(itx->itx_size, >=, sizeof (itx_t));
	ASSERT3U(itx->itx_lr.lrc_reclen, ==,
	    itx->itx_size - offsetof(itx_t, itx_lr));
	IMPLY(itx->itx_lr.lrc_txtype == TX_COMMIT, itx->itx_callback == NULL);
	IMPLY(itx->itx_callback != NULL, itx->itx_lr.lrc_txtype != TX_COMMIT);

	if (itx->itx_callback != NULL)
		itx->itx_callback(itx->itx_callback_data);

	zio_data_buf_free(itx, itx->itx_size);
}

/*
 * Free up the sync and async itxs. The itxs_t has already been detached
 * so no locks are needed.
 */
static void
zil_itxg_clean(void *arg)
{
	itx_t *itx;
	list_t *list;
	avl_tree_t *t;
	void *cookie;
	itxs_t *itxs = arg;
	itx_async_node_t *ian;

	list = &itxs->i_sync_list;
	while ((itx = list_remove_head(list)) != NULL) {
		/*
		 * In the general case, commit itxs will not be found
		 * here, as they'll be committed to an lwb via
		 * zil_lwb_assign(), and free'd in that function. Having
		 * said that, it is still possible for commit itxs to be
		 * found here, due to the following race:
		 *
		 *	- a thread calls zil_commit() which assigns the
		 *	  commit itx to a per-txg i_sync_list
		 *	- zil_itxg_clean() is called (e.g. via spa_sync())
		 *	  while the waiter is still on the i_sync_list
		 *
		 * There's nothing to prevent syncing the txg while the
		 * waiter is on the i_sync_list. This normally doesn't
		 * happen because spa_sync() is slower than zil_commit(),
		 * but if zil_commit() calls txg_wait_synced() (e.g.
		 * because zil_create() or zil_commit_writer_stall() is
		 * called) we will hit this case.
		 */
		if (itx->itx_lr.lrc_txtype == TX_COMMIT)
			zil_commit_waiter_skip(itx->itx_private);

		zil_itx_destroy(itx);
	}

	cookie = NULL;
	t = &itxs->i_async_tree;
	while ((ian = avl_destroy_nodes(t, &cookie)) != NULL) {
		list = &ian->ia_list;
		while ((itx = list_remove_head(list)) != NULL) {
			/* commit itxs should never be on the async lists. */
			ASSERT3U(itx->itx_lr.lrc_txtype, !=, TX_COMMIT);
			zil_itx_destroy(itx);
		}
		list_destroy(list);
		kmem_free(ian, sizeof (itx_async_node_t));
	}
	avl_destroy(t);

	kmem_free(itxs, sizeof (itxs_t));
}

static int
zil_aitx_compare(const void *x1, const void *x2)
{
	const uint64_t o1 = ((itx_async_node_t *)x1)->ia_foid;
	const uint64_t o2 = ((itx_async_node_t *)x2)->ia_foid;

	return (TREE_CMP(o1, o2));
}

/*
 * Remove all async itx with the given oid.
 */
void
zil_remove_async(zilog_t *zilog, uint64_t oid)
{
	uint64_t otxg, txg;
	itx_async_node_t *ian, ian_search;
	avl_tree_t *t;
	avl_index_t where;
	list_t clean_list;
	itx_t *itx;

	ASSERT(oid != 0);
	list_create(&clean_list, sizeof (itx_t), offsetof(itx_t, itx_node));

	if (spa_freeze_txg(zilog->zl_spa) != UINT64_MAX) /* ziltest support */
		otxg = ZILTEST_TXG;
	else
		otxg = spa_last_synced_txg(zilog->zl_spa) + 1;

	for (txg = otxg; txg < (otxg + TXG_CONCURRENT_STATES); txg++) {
		itxg_t *itxg = &zilog->zl_itxg[txg & TXG_MASK];

		mutex_enter(&itxg->itxg_lock);
		if (itxg->itxg_txg != txg) {
			mutex_exit(&itxg->itxg_lock);
			continue;
		}

		/*
		 * Locate the object node and append its list.
		 */
		t = &itxg->itxg_itxs->i_async_tree;
		ian_search.ia_foid = oid;
		ian = avl_find(t, &ian_search, &where);
		if (ian != NULL)
			list_move_tail(&clean_list, &ian->ia_list);
		mutex_exit(&itxg->itxg_lock);
	}
	while ((itx = list_remove_head(&clean_list)) != NULL) {
		/* commit itxs should never be on the async lists. */
		ASSERT3U(itx->itx_lr.lrc_txtype, !=, TX_COMMIT);
		zil_itx_destroy(itx);
	}
	list_destroy(&clean_list);
}

void
zil_itx_assign(zilog_t *zilog, itx_t *itx, dmu_tx_t *tx)
{
	uint64_t txg;
	itxg_t *itxg;
	itxs_t *itxs, *clean = NULL;

	/*
	 * Ensure the data of a renamed file is committed before the rename.
	 */
	if ((itx->itx_lr.lrc_txtype & ~TX_CI) == TX_RENAME)
		zil_async_to_sync(zilog, itx->itx_oid);

	if (spa_freeze_txg(zilog->zl_spa) != UINT64_MAX)
		txg = ZILTEST_TXG;
	else
		txg = dmu_tx_get_txg(tx);

	itxg = &zilog->zl_itxg[txg & TXG_MASK];
	mutex_enter(&itxg->itxg_lock);
	itxs = itxg->itxg_itxs;
	if (itxg->itxg_txg != txg) {
		if (itxs != NULL) {
			/*
			 * The zil_clean callback hasn't got around to cleaning
			 * this itxg. Save the itxs for release below.
			 * This should be rare.
			 */
			zfs_dbgmsg("zil_itx_assign: missed itx cleanup for "
			    "txg %llu", (u_longlong_t)itxg->itxg_txg);
			clean = itxg->itxg_itxs;
		}
		itxg->itxg_txg = txg;
		itxs = itxg->itxg_itxs = kmem_zalloc(sizeof (itxs_t),
		    KM_SLEEP);

		list_create(&itxs->i_sync_list, sizeof (itx_t),
		    offsetof(itx_t, itx_node));
		avl_create(&itxs->i_async_tree, zil_aitx_compare,
		    sizeof (itx_async_node_t),
		    offsetof(itx_async_node_t, ia_node));
	}
	if (itx->itx_sync) {
		list_insert_tail(&itxs->i_sync_list, itx);
	} else {
		avl_tree_t *t = &itxs->i_async_tree;
		uint64_t foid =
		    LR_FOID_GET_OBJ(((lr_ooo_t *)&itx->itx_lr)->lr_foid);
		itx_async_node_t *ian;
		avl_index_t where;

		ian = avl_find(t, &foid, &where);
		if (ian == NULL) {
			ian = kmem_alloc(sizeof (itx_async_node_t),
			    KM_SLEEP);
			list_create(&ian->ia_list, sizeof (itx_t),
			    offsetof(itx_t, itx_node));
			ian->ia_foid = foid;
			avl_insert(t, ian, where);
		}
		list_insert_tail(&ian->ia_list, itx);
	}

	itx->itx_lr.lrc_txg = dmu_tx_get_txg(tx);

	/*
	 * We don't want to dirty the ZIL using ZILTEST_TXG, because
	 * zil_clean() will never be called using ZILTEST_TXG. Thus, we
	 * need to be careful to always dirty the ZIL using the "real"
	 * TXG (not itxg_txg) even when the SPA is frozen.
	 */
	zilog_dirty(zilog, dmu_tx_get_txg(tx));
	mutex_exit(&itxg->itxg_lock);

	/* Release the old itxs now we've dropped the lock */
	if (clean != NULL)
		zil_itxg_clean(clean);
}

/*
 * If there are any in-memory intent log transactions which have now been
 * synced then start up a taskq to free them. We should only do this after we
 * have written out the uberblocks (i.e. txg has been committed) so that
 * don't inadvertently clean out in-memory log records that would be required
 * by zil_commit().
 */
void
zil_clean(zilog_t *zilog, uint64_t synced_txg)
{
	itxg_t *itxg = &zilog->zl_itxg[synced_txg & TXG_MASK];
	itxs_t *clean_me;

	ASSERT3U(synced_txg, <, ZILTEST_TXG);

	mutex_enter(&itxg->itxg_lock);
	if (itxg->itxg_itxs == NULL || itxg->itxg_txg == ZILTEST_TXG) {
		mutex_exit(&itxg->itxg_lock);
		return;
	}
	ASSERT3U(itxg->itxg_txg, <=, synced_txg);
	ASSERT3U(itxg->itxg_txg, !=, 0);
	clean_me = itxg->itxg_itxs;
	itxg->itxg_itxs = NULL;
	itxg->itxg_txg = 0;
	mutex_exit(&itxg->itxg_lock);
	/*
	 * Preferably start a task queue to free up the old itxs but
	 * if taskq_dispatch can't allocate resources to do that then
	 * free it in-line. This should be rare. Note, using TQ_SLEEP
	 * created a bad performance problem.
	 */
	ASSERT3P(zilog->zl_dmu_pool, !=, NULL);
	ASSERT3P(zilog->zl_dmu_pool->dp_zil_clean_taskq, !=, NULL);
	taskqid_t id = taskq_dispatch(zilog->zl_dmu_pool->dp_zil_clean_taskq,
	    zil_itxg_clean, clean_me, TQ_NOSLEEP);
	if (id == TASKQID_INVALID)
		zil_itxg_clean(clean_me);
}

/*
 * This function will traverse the queue of itxs that need to be
 * committed, and move them onto the ZIL's zl_itx_commit_list.
 */
static uint64_t
zil_get_commit_list(zilog_t *zilog)
{
	uint64_t otxg, txg, wtxg = 0;
	list_t *commit_list = &zilog->zl_itx_commit_list;

	ASSERT(MUTEX_HELD(&zilog->zl_issuer_lock));

	if (spa_freeze_txg(zilog->zl_spa) != UINT64_MAX) /* ziltest support */
		otxg = ZILTEST_TXG;
	else
		otxg = spa_last_synced_txg(zilog->zl_spa) + 1;

	/*
	 * This is inherently racy, since there is nothing to prevent
	 * the last synced txg from changing. That's okay since we'll
	 * only commit things in the future.
	 */
	for (txg = otxg; txg < (otxg + TXG_CONCURRENT_STATES); txg++) {
		itxg_t *itxg = &zilog->zl_itxg[txg & TXG_MASK];

		mutex_enter(&itxg->itxg_lock);
		if (itxg->itxg_txg != txg) {
			mutex_exit(&itxg->itxg_lock);
			continue;
		}

		/*
		 * If we're adding itx records to the zl_itx_commit_list,
		 * then the zil better be dirty in this "txg". We can assert
		 * that here since we're holding the itxg_lock which will
		 * prevent spa_sync from cleaning it. Once we add the itxs
		 * to the zl_itx_commit_list we must commit it to disk even
		 * if it's unnecessary (i.e. the txg was synced).
		 */
		ASSERT(zilog_is_dirty_in_txg(zilog, txg) ||
		    spa_freeze_txg(zilog->zl_spa) != UINT64_MAX);
		list_t *sync_list = &itxg->itxg_itxs->i_sync_list;
		itx_t *itx = NULL;
		if (unlikely(zilog->zl_suspend > 0)) {
			/*
			 * ZIL was just suspended, but we lost the race.
			 * Allow all earlier itxs to be committed, but ask
			 * caller to do txg_wait_synced(txg) for any new.
			 */
			if (!list_is_empty(sync_list))
				wtxg = MAX(wtxg, txg);
		} else {
			itx = list_head(sync_list);
			list_move_tail(commit_list, sync_list);
		}

		mutex_exit(&itxg->itxg_lock);

		while (itx != NULL) {
			uint64_t s = zil_itx_full_size(itx);
			zilog->zl_cur_size += s;
			zilog->zl_cur_left += s;
			s = zil_itx_record_size(itx);
			zilog->zl_cur_max = MAX(zilog->zl_cur_max, s);
			itx = list_next(commit_list, itx);
		}
	}
	return (wtxg);
}

/*
 * Move the async itxs for a specified object to commit into sync lists.
 */
void
zil_async_to_sync(zilog_t *zilog, uint64_t foid)
{
	uint64_t otxg, txg;
	itx_async_node_t *ian, ian_search;
	avl_tree_t *t;
	avl_index_t where;

	if (spa_freeze_txg(zilog->zl_spa) != UINT64_MAX) /* ziltest support */
		otxg = ZILTEST_TXG;
	else
		otxg = spa_last_synced_txg(zilog->zl_spa) + 1;

	/*
	 * This is inherently racy, since there is nothing to prevent
	 * the last synced txg from changing.
	 */
	for (txg = otxg; txg < (otxg + TXG_CONCURRENT_STATES); txg++) {
		itxg_t *itxg = &zilog->zl_itxg[txg & TXG_MASK];

		mutex_enter(&itxg->itxg_lock);
		if (itxg->itxg_txg != txg) {
			mutex_exit(&itxg->itxg_lock);
			continue;
		}

		/*
		 * If a foid is specified then find that node and append its
		 * list. Otherwise walk the tree appending all the lists
		 * to the sync list. We add to the end rather than the
		 * beginning to ensure the create has happened.
		 */
		t = &itxg->itxg_itxs->i_async_tree;
		if (foid != 0) {
			ian_search.ia_foid = foid;
			ian = avl_find(t, &ian_search, &where);
			if (ian != NULL) {
				list_move_tail(&itxg->itxg_itxs->i_sync_list,
				    &ian->ia_list);
			}
		} else {
			void *cookie = NULL;

			while ((ian = avl_destroy_nodes(t, &cookie)) != NULL) {
				list_move_tail(&itxg->itxg_itxs->i_sync_list,
				    &ian->ia_list);
				list_destroy(&ian->ia_list);
				kmem_free(ian, sizeof (itx_async_node_t));
			}
		}
		mutex_exit(&itxg->itxg_lock);
	}
}

/*
 * This function will prune commit itxs that are at the head of the
 * commit list (it won't prune past the first non-commit itx), and
 * either: a) attach them to the last lwb that's still pending
 * completion, or b) skip them altogether.
 *
 * This is used as a performance optimization to prevent commit itxs
 * from generating new lwbs when it's unnecessary to do so.
 */
static void
zil_prune_commit_list(zilog_t *zilog)
{
	itx_t *itx;

	ASSERT(MUTEX_HELD(&zilog->zl_issuer_lock));

	while ((itx = list_head(&zilog->zl_itx_commit_list)) != NULL) {
		lr_t *lrc = &itx->itx_lr;
		if (lrc->lrc_txtype != TX_COMMIT)
			break;

		mutex_enter(&zilog->zl_lock);

		lwb_t *last_lwb = zilog->zl_last_lwb_opened;
		if (last_lwb == NULL ||
		    last_lwb->lwb_state == LWB_STATE_FLUSH_DONE) {
			/*
			 * All of the itxs this waiter was waiting on
			 * must have already completed (or there were
			 * never any itx's for it to wait on), so it's
			 * safe to skip this waiter and mark it done.
			 */
			zil_commit_waiter_skip(itx->itx_private);
		} else {
			zil_commit_waiter_link_lwb(itx->itx_private, last_lwb);
		}

		mutex_exit(&zilog->zl_lock);

		list_remove(&zilog->zl_itx_commit_list, itx);
		zil_itx_destroy(itx);
	}

	IMPLY(itx != NULL, itx->itx_lr.lrc_txtype != TX_COMMIT);
}

static void
zil_commit_writer_stall(zilog_t *zilog)
{
	/*
	 * When zio_alloc_zil() fails to allocate the next lwb block on
	 * disk, we must call txg_wait_synced() to ensure all of the
	 * lwbs in the zilog's zl_lwb_list are synced and then freed (in
	 * zil_sync()), such that any subsequent ZIL writer (i.e. a call
	 * to zil_process_commit_list()) will have to call zil_create(),
	 * and start a new ZIL chain.
	 *
	 * Since zil_alloc_zil() failed, the lwb that was previously
	 * issued does not have a pointer to the "next" lwb on disk.
	 * Thus, if another ZIL writer thread was to allocate the "next"
	 * on-disk lwb, that block could be leaked in the event of a
	 * crash (because the previous lwb on-disk would not point to
	 * it).
	 *
	 * We must hold the zilog's zl_issuer_lock while we do this, to
	 * ensure no new threads enter zil_process_commit_list() until
	 * all lwb's in the zl_lwb_list have been synced and freed
	 * (which is achieved via the txg_wait_synced() call).
	 */
	ASSERT(MUTEX_HELD(&zilog->zl_issuer_lock));
	ZIL_STAT_BUMP(zilog, zil_commit_stall_count);
	txg_wait_synced(zilog->zl_dmu_pool, 0);
	ASSERT(list_is_empty(&zilog->zl_lwb_list));
}

static void
zil_burst_done(zilog_t *zilog)
{
	if (!list_is_empty(&zilog->zl_itx_commit_list) ||
	    zilog->zl_cur_size == 0)
		return;

	if (zilog->zl_parallel)
		zilog->zl_parallel--;

	uint_t r = (zilog->zl_prev_rotor + 1) & (ZIL_BURSTS - 1);
	zilog->zl_prev_rotor = r;
	zilog->zl_prev_opt[r] = zil_lwb_plan(zilog, zilog->zl_cur_size,
	    &zilog->zl_prev_min[r]);

	zilog->zl_cur_size = 0;
	zilog->zl_cur_max = 0;
	zilog->zl_cur_left = 0;
}

/*
 * This function will traverse the commit list, creating new lwbs as
 * needed, and committing the itxs from the commit list to these newly
 * created lwbs. Additionally, as a new lwb is created, the previous
 * lwb will be issued to the zio layer to be written to disk.
 */
static void
zil_process_commit_list(zilog_t *zilog, zil_commit_waiter_t *zcw, list_t *ilwbs)
{
	spa_t *spa = zilog->zl_spa;
	list_t nolwb_itxs;
	list_t nolwb_waiters;
	lwb_t *lwb, *plwb;
	itx_t *itx;

	ASSERT(MUTEX_HELD(&zilog->zl_issuer_lock));

	lwb = list_tail(&zilog->zl_lwb_list);
	if (lwb == NULL) {
		/*
		 * Return if there's nothing to commit before we dirty the fs.
		 */
		if (list_is_empty(&zilog->zl_itx_commit_list))
			return;

		lwb = zil_create(zilog);
	} else {
		/*
		 * Activate SPA_FEATURE_ZILSAXATTR for the cases where ZIL will
		 * have already been created (zl_lwb_list not empty).
		 */
		zil_commit_activate_saxattr_feature(zilog);
		ASSERT(lwb->lwb_state == LWB_STATE_NEW ||
		    lwb->lwb_state == LWB_STATE_OPENED);

		/*
		 * If the lwb is still opened, it means the workload is really
		 * multi-threaded and we won the chance of write aggregation.
		 * If it is not opened yet, but previous lwb is still not
		 * flushed, it still means the workload is multi-threaded, but
		 * there was too much time between the commits to aggregate, so
		 * we try aggregation next times, but without too much hopes.
		 */
		if (lwb->lwb_state == LWB_STATE_OPENED) {
			zilog->zl_parallel = ZIL_BURSTS;
		} else if ((plwb = list_prev(&zilog->zl_lwb_list, lwb))
		    != NULL && plwb->lwb_state != LWB_STATE_FLUSH_DONE) {
			zilog->zl_parallel = MAX(zilog->zl_parallel,
			    ZIL_BURSTS / 2);
		}
	}

	list_create(&nolwb_itxs, sizeof (itx_t), offsetof(itx_t, itx_node));
	list_create(&nolwb_waiters, sizeof (zil_commit_waiter_t),
	    offsetof(zil_commit_waiter_t, zcw_node));

	while ((itx = list_remove_head(&zilog->zl_itx_commit_list)) != NULL) {
		lr_t *lrc = &itx->itx_lr;
		uint64_t txg = lrc->lrc_txg;

		ASSERT3U(txg, !=, 0);

		if (lrc->lrc_txtype == TX_COMMIT) {
			DTRACE_PROBE2(zil__process__commit__itx,
			    zilog_t *, zilog, itx_t *, itx);
		} else {
			DTRACE_PROBE2(zil__process__normal__itx,
			    zilog_t *, zilog, itx_t *, itx);
		}

		boolean_t synced = txg <= spa_last_synced_txg(spa);
		boolean_t frozen = txg > spa_freeze_txg(spa);

		/*
		 * If the txg of this itx has already been synced out, then
		 * we don't need to commit this itx to an lwb. This is
		 * because the data of this itx will have already been
		 * written to the main pool. This is inherently racy, and
		 * it's still ok to commit an itx whose txg has already
		 * been synced; this will result in a write that's
		 * unnecessary, but will do no harm.
		 *
		 * With that said, we always want to commit TX_COMMIT itxs
		 * to an lwb, regardless of whether or not that itx's txg
		 * has been synced out. We do this to ensure any OPENED lwb
		 * will always have at least one zil_commit_waiter_t linked
		 * to the lwb.
		 *
		 * As a counter-example, if we skipped TX_COMMIT itx's
		 * whose txg had already been synced, the following
		 * situation could occur if we happened to be racing with
		 * spa_sync:
		 *
		 * 1. We commit a non-TX_COMMIT itx to an lwb, where the
		 *    itx's txg is 10 and the last synced txg is 9.
		 * 2. spa_sync finishes syncing out txg 10.
		 * 3. We move to the next itx in the list, it's a TX_COMMIT
		 *    whose txg is 10, so we skip it rather than committing
		 *    it to the lwb used in (1).
		 *
		 * If the itx that is skipped in (3) is the last TX_COMMIT
		 * itx in the commit list, than it's possible for the lwb
		 * used in (1) to remain in the OPENED state indefinitely.
		 *
		 * To prevent the above scenario from occurring, ensuring
		 * that once an lwb is OPENED it will transition to ISSUED
		 * and eventually DONE, we always commit TX_COMMIT itx's to
		 * an lwb here, even if that itx's txg has already been
		 * synced.
		 *
		 * Finally, if the pool is frozen, we _always_ commit the
		 * itx.  The point of freezing the pool is to prevent data
		 * from being written to the main pool via spa_sync, and
		 * instead rely solely on the ZIL to persistently store the
		 * data; i.e.  when the pool is frozen, the last synced txg
		 * value can't be trusted.
		 */
		if (frozen || !synced || lrc->lrc_txtype == TX_COMMIT) {
			if (lwb != NULL) {
				lwb = zil_lwb_assign(zilog, lwb, itx, ilwbs);
				if (lwb == NULL) {
					list_insert_tail(&nolwb_itxs, itx);
				} else if ((zcw->zcw_lwb != NULL &&
				    zcw->zcw_lwb != lwb) || zcw->zcw_done) {
					/*
					 * Our lwb is done, leave the rest of
					 * itx list to somebody else who care.
					 */
					zilog->zl_parallel = ZIL_BURSTS;
					zilog->zl_cur_left -=
					    zil_itx_full_size(itx);
					break;
				}
			} else {
				if (lrc->lrc_txtype == TX_COMMIT) {
					zil_commit_waiter_link_nolwb(
					    itx->itx_private, &nolwb_waiters);
				}
				list_insert_tail(&nolwb_itxs, itx);
			}
			zilog->zl_cur_left -= zil_itx_full_size(itx);
		} else {
			ASSERT3S(lrc->lrc_txtype, !=, TX_COMMIT);
			zilog->zl_cur_left -= zil_itx_full_size(itx);
			zil_itx_destroy(itx);
		}
	}

	if (lwb == NULL) {
		/*
		 * This indicates zio_alloc_zil() failed to allocate the
		 * "next" lwb on-disk. When this happens, we must stall
		 * the ZIL write pipeline; see the comment within
		 * zil_commit_writer_stall() for more details.
		 */
		while ((lwb = list_remove_head(ilwbs)) != NULL)
			zil_lwb_write_issue(zilog, lwb);
		zil_commit_writer_stall(zilog);

		/*
		 * Additionally, we have to signal and mark the "nolwb"
		 * waiters as "done" here, since without an lwb, we
		 * can't do this via zil_lwb_flush_vdevs_done() like
		 * normal.
		 */
		zil_commit_waiter_t *zcw;
		while ((zcw = list_remove_head(&nolwb_waiters)) != NULL)
			zil_commit_waiter_skip(zcw);

		/*
		 * And finally, we have to destroy the itx's that
		 * couldn't be committed to an lwb; this will also call
		 * the itx's callback if one exists for the itx.
		 */
		while ((itx = list_remove_head(&nolwb_itxs)) != NULL)
			zil_itx_destroy(itx);
	} else {
		ASSERT(list_is_empty(&nolwb_waiters));
		ASSERT3P(lwb, !=, NULL);
		ASSERT(lwb->lwb_state == LWB_STATE_NEW ||
		    lwb->lwb_state == LWB_STATE_OPENED);

		/*
		 * At this point, the ZIL block pointed at by the "lwb"
		 * variable is in "new" or "opened" state.
		 *
		 * If it's "new", then no itxs have been committed to it, so
		 * there's no point in issuing its zio (i.e. it's "empty").
		 *
		 * If it's "opened", then it contains one or more itxs that
		 * eventually need to be committed to stable storage. In
		 * this case we intentionally do not issue the lwb's zio
		 * to disk yet, and instead rely on one of the following
		 * two mechanisms for issuing the zio:
		 *
		 * 1. Ideally, there will be more ZIL activity occurring on
		 * the system, such that this function will be immediately
		 * called again by different thread and this lwb will be
		 * closed by zil_lwb_assign().  This way, the lwb will be
		 * "full" when it is issued to disk, and we'll make use of
		 * the lwb's size the best we can.
		 *
		 * 2. If there isn't sufficient ZIL activity occurring on
		 * the system, zil_commit_waiter() will close it and issue
		 * the zio.  If this occurs, the lwb is not guaranteed
		 * to be "full" by the time its zio is issued, and means
		 * the size of the lwb was "too large" given the amount
		 * of ZIL activity occurring on the system at that time.
		 *
		 * We do this for a couple of reasons:
		 *
		 * 1. To try and reduce the number of IOPs needed to
		 * write the same number of itxs. If an lwb has space
		 * available in its buffer for more itxs, and more itxs
		 * will be committed relatively soon (relative to the
		 * latency of performing a write), then it's beneficial
		 * to wait for these "next" itxs. This way, more itxs
		 * can be committed to stable storage with fewer writes.
		 *
		 * 2. To try and use the largest lwb block size that the
		 * incoming rate of itxs can support. Again, this is to
		 * try and pack as many itxs into as few lwbs as
		 * possible, without significantly impacting the latency
		 * of each individual itx.
		 */
		if (lwb->lwb_state == LWB_STATE_OPENED &&
		    (!zilog->zl_parallel || zilog->zl_suspend > 0)) {
			zil_burst_done(zilog);
			list_insert_tail(ilwbs, lwb);
			lwb = zil_lwb_write_close(zilog, lwb, LWB_STATE_NEW);
			if (lwb == NULL) {
				while ((lwb = list_remove_head(ilwbs)) != NULL)
					zil_lwb_write_issue(zilog, lwb);
				zil_commit_writer_stall(zilog);
			}
		}
	}
}

/*
 * This function is responsible for ensuring the passed in commit waiter
 * (and associated commit itx) is committed to an lwb. If the waiter is
 * not already committed to an lwb, all itxs in the zilog's queue of
 * itxs will be processed. The assumption is the passed in waiter's
 * commit itx will found in the queue just like the other non-commit
 * itxs, such that when the entire queue is processed, the waiter will
 * have been committed to an lwb.
 *
 * The lwb associated with the passed in waiter is not guaranteed to
 * have been issued by the time this function completes. If the lwb is
 * not issued, we rely on future calls to zil_commit_writer() to issue
 * the lwb, or the timeout mechanism found in zil_commit_waiter().
 */
static uint64_t
zil_commit_writer(zilog_t *zilog, zil_commit_waiter_t *zcw)
{
	list_t ilwbs;
	lwb_t *lwb;
	uint64_t wtxg = 0;

	ASSERT(!MUTEX_HELD(&zilog->zl_lock));
	ASSERT(spa_writeable(zilog->zl_spa));

	list_create(&ilwbs, sizeof (lwb_t), offsetof(lwb_t, lwb_issue_node));
	mutex_enter(&zilog->zl_issuer_lock);

	if (zcw->zcw_lwb != NULL || zcw->zcw_done) {
		/*
		 * It's possible that, while we were waiting to acquire
		 * the "zl_issuer_lock", another thread committed this
		 * waiter to an lwb. If that occurs, we bail out early,
		 * without processing any of the zilog's queue of itxs.
		 *
		 * On certain workloads and system configurations, the
		 * "zl_issuer_lock" can become highly contended. In an
		 * attempt to reduce this contention, we immediately drop
		 * the lock if the waiter has already been processed.
		 *
		 * We've measured this optimization to reduce CPU spent
		 * contending on this lock by up to 5%, using a system
		 * with 32 CPUs, low latency storage (~50 usec writes),
		 * and 1024 threads performing sync writes.
		 */
		goto out;
	}

	ZIL_STAT_BUMP(zilog, zil_commit_writer_count);

	wtxg = zil_get_commit_list(zilog);
	zil_prune_commit_list(zilog);
	zil_process_commit_list(zilog, zcw, &ilwbs);

out:
	mutex_exit(&zilog->zl_issuer_lock);
	while ((lwb = list_remove_head(&ilwbs)) != NULL)
		zil_lwb_write_issue(zilog, lwb);
	list_destroy(&ilwbs);
	return (wtxg);
}

static void
zil_commit_waiter_timeout(zilog_t *zilog, zil_commit_waiter_t *zcw)
{
	ASSERT(!MUTEX_HELD(&zilog->zl_issuer_lock));
	ASSERT(MUTEX_HELD(&zcw->zcw_lock));
	ASSERT3B(zcw->zcw_done, ==, B_FALSE);

	lwb_t *lwb = zcw->zcw_lwb;
	ASSERT3P(lwb, !=, NULL);
	ASSERT3S(lwb->lwb_state, !=, LWB_STATE_NEW);

	/*
	 * If the lwb has already been issued by another thread, we can
	 * immediately return since there's no work to be done (the
	 * point of this function is to issue the lwb). Additionally, we
	 * do this prior to acquiring the zl_issuer_lock, to avoid
	 * acquiring it when it's not necessary to do so.
	 */
	if (lwb->lwb_state != LWB_STATE_OPENED)
		return;

	/*
	 * In order to call zil_lwb_write_close() we must hold the
	 * zilog's "zl_issuer_lock". We can't simply acquire that lock,
	 * since we're already holding the commit waiter's "zcw_lock",
	 * and those two locks are acquired in the opposite order
	 * elsewhere.
	 */
	mutex_exit(&zcw->zcw_lock);
	mutex_enter(&zilog->zl_issuer_lock);
	mutex_enter(&zcw->zcw_lock);

	/*
	 * Since we just dropped and re-acquired the commit waiter's
	 * lock, we have to re-check to see if the waiter was marked
	 * "done" during that process. If the waiter was marked "done",
	 * the "lwb" pointer is no longer valid (it can be free'd after
	 * the waiter is marked "done"), so without this check we could
	 * wind up with a use-after-free error below.
	 */
	if (zcw->zcw_done) {
		mutex_exit(&zilog->zl_issuer_lock);
		return;
	}

	ASSERT3P(lwb, ==, zcw->zcw_lwb);

	/*
	 * We've already checked this above, but since we hadn't acquired
	 * the zilog's zl_issuer_lock, we have to perform this check a
	 * second time while holding the lock.
	 *
	 * We don't need to hold the zl_lock since the lwb cannot transition
	 * from OPENED to CLOSED while we hold the zl_issuer_lock. The lwb
	 * _can_ transition from CLOSED to DONE, but it's OK to race with
	 * that transition since we treat the lwb the same, whether it's in
	 * the CLOSED, ISSUED or DONE states.
	 *
	 * The important thing, is we treat the lwb differently depending on
	 * if it's OPENED or CLOSED, and block any other threads that might
	 * attempt to close/issue this lwb. For that reason we hold the
	 * zl_issuer_lock when checking the lwb_state; we must not call
	 * zil_lwb_write_close() if the lwb had already been closed/issued.
	 *
	 * See the comment above the lwb_state_t structure definition for
	 * more details on the lwb states, and locking requirements.
	 */
	if (lwb->lwb_state != LWB_STATE_OPENED) {
		mutex_exit(&zilog->zl_issuer_lock);
		return;
	}

	/*
	 * We do not need zcw_lock once we hold zl_issuer_lock and know lwb
	 * is still open.  But we have to drop it to avoid a deadlock in case
	 * callback of zio issued by zil_lwb_write_issue() try to get it,
	 * while zil_lwb_write_issue() is blocked on attempt to issue next
	 * lwb it found in LWB_STATE_READY state.
	 */
	mutex_exit(&zcw->zcw_lock);

	/*
	 * As described in the comments above zil_commit_waiter() and
	 * zil_process_commit_list(), we need to issue this lwb's zio
	 * since we've reached the commit waiter's timeout and it still
	 * hasn't been issued.
	 */
	zil_burst_done(zilog);
	lwb_t *nlwb = zil_lwb_write_close(zilog, lwb, LWB_STATE_NEW);

	ASSERT3S(lwb->lwb_state, ==, LWB_STATE_CLOSED);

	if (nlwb == NULL) {
		/*
		 * When zil_lwb_write_close() returns NULL, this
		 * indicates zio_alloc_zil() failed to allocate the
		 * "next" lwb on-disk. When this occurs, the ZIL write
		 * pipeline must be stalled; see the comment within the
		 * zil_commit_writer_stall() function for more details.
		 */
		zil_lwb_write_issue(zilog, lwb);
		zil_commit_writer_stall(zilog);
		mutex_exit(&zilog->zl_issuer_lock);
	} else {
		mutex_exit(&zilog->zl_issuer_lock);
		zil_lwb_write_issue(zilog, lwb);
	}
	mutex_enter(&zcw->zcw_lock);
}

/*
 * This function is responsible for performing the following two tasks:
 *
 * 1. its primary responsibility is to block until the given "commit
 *    waiter" is considered "done".
 *
 * 2. its secondary responsibility is to issue the zio for the lwb that
 *    the given "commit waiter" is waiting on, if this function has
 *    waited "long enough" and the lwb is still in the "open" state.
 *
 * Given a sufficient amount of itxs being generated and written using
 * the ZIL, the lwb's zio will be issued via the zil_lwb_assign()
 * function. If this does not occur, this secondary responsibility will
 * ensure the lwb is issued even if there is not other synchronous
 * activity on the system.
 *
 * For more details, see zil_process_commit_list(); more specifically,
 * the comment at the bottom of that function.
 */
static void
zil_commit_waiter(zilog_t *zilog, zil_commit_waiter_t *zcw)
{
	ASSERT(!MUTEX_HELD(&zilog->zl_lock));
	ASSERT(!MUTEX_HELD(&zilog->zl_issuer_lock));
	ASSERT(spa_writeable(zilog->zl_spa));

	mutex_enter(&zcw->zcw_lock);

	/*
	 * The timeout is scaled based on the lwb latency to avoid
	 * significantly impacting the latency of each individual itx.
	 * For more details, see the comment at the bottom of the
	 * zil_process_commit_list() function.
	 */
	int pct = MAX(zfs_commit_timeout_pct, 1);
	hrtime_t sleep = (zilog->zl_last_lwb_latency * pct) / 100;
	hrtime_t wakeup = gethrtime() + sleep;
	boolean_t timedout = B_FALSE;

	while (!zcw->zcw_done) {
		ASSERT(MUTEX_HELD(&zcw->zcw_lock));

		lwb_t *lwb = zcw->zcw_lwb;

		/*
		 * Usually, the waiter will have a non-NULL lwb field here,
		 * but it's possible for it to be NULL as a result of
		 * zil_commit() racing with spa_sync().
		 *
		 * When zil_clean() is called, it's possible for the itxg
		 * list (which may be cleaned via a taskq) to contain
		 * commit itxs. When this occurs, the commit waiters linked
		 * off of these commit itxs will not be committed to an
		 * lwb.  Additionally, these commit waiters will not be
		 * marked done until zil_commit_waiter_skip() is called via
		 * zil_itxg_clean().
		 *
		 * Thus, it's possible for this commit waiter (i.e. the
		 * "zcw" variable) to be found in this "in between" state;
		 * where it's "zcw_lwb" field is NULL, and it hasn't yet
		 * been skipped, so it's "zcw_done" field is still B_FALSE.
		 */
		IMPLY(lwb != NULL, lwb->lwb_state != LWB_STATE_NEW);

		if (lwb != NULL && lwb->lwb_state == LWB_STATE_OPENED) {
			ASSERT3B(timedout, ==, B_FALSE);

			/*
			 * If the lwb hasn't been issued yet, then we
			 * need to wait with a timeout, in case this
			 * function needs to issue the lwb after the
			 * timeout is reached; responsibility (2) from
			 * the comment above this function.
			 */
			int rc = cv_timedwait_hires(&zcw->zcw_cv,
			    &zcw->zcw_lock, wakeup, USEC2NSEC(1),
			    CALLOUT_FLAG_ABSOLUTE);

			if (rc != -1 || zcw->zcw_done)
				continue;

			timedout = B_TRUE;
			zil_commit_waiter_timeout(zilog, zcw);

			if (!zcw->zcw_done) {
				/*
				 * If the commit waiter has already been
				 * marked "done", it's possible for the
				 * waiter's lwb structure to have already
				 * been freed.  Thus, we can only reliably
				 * make these assertions if the waiter
				 * isn't done.
				 */
				ASSERT3P(lwb, ==, zcw->zcw_lwb);
				ASSERT3S(lwb->lwb_state, !=, LWB_STATE_OPENED);
			}
		} else {
			/*
			 * If the lwb isn't open, then it must have already
			 * been issued. In that case, there's no need to
			 * use a timeout when waiting for the lwb to
			 * complete.
			 *
			 * Additionally, if the lwb is NULL, the waiter
			 * will soon be signaled and marked done via
			 * zil_clean() and zil_itxg_clean(), so no timeout
			 * is required.
			 */

			IMPLY(lwb != NULL,
			    lwb->lwb_state == LWB_STATE_CLOSED ||
			    lwb->lwb_state == LWB_STATE_READY ||
			    lwb->lwb_state == LWB_STATE_ISSUED ||
			    lwb->lwb_state == LWB_STATE_WRITE_DONE ||
			    lwb->lwb_state == LWB_STATE_FLUSH_DONE);
			cv_wait(&zcw->zcw_cv, &zcw->zcw_lock);
		}
	}

	mutex_exit(&zcw->zcw_lock);
}

static zil_commit_waiter_t *
zil_alloc_commit_waiter(void)
{
	zil_commit_waiter_t *zcw = kmem_cache_alloc(zil_zcw_cache, KM_SLEEP);

	cv_init(&zcw->zcw_cv, NULL, CV_DEFAULT, NULL);
	mutex_init(&zcw->zcw_lock, NULL, MUTEX_DEFAULT, NULL);
	list_link_init(&zcw->zcw_node);
	zcw->zcw_lwb = NULL;
	zcw->zcw_done = B_FALSE;
	zcw->zcw_zio_error = 0;

	return (zcw);
}

static void
zil_free_commit_waiter(zil_commit_waiter_t *zcw)
{
	ASSERT(!list_link_active(&zcw->zcw_node));
	ASSERT3P(zcw->zcw_lwb, ==, NULL);
	ASSERT3B(zcw->zcw_done, ==, B_TRUE);
	mutex_destroy(&zcw->zcw_lock);
	cv_destroy(&zcw->zcw_cv);
	kmem_cache_free(zil_zcw_cache, zcw);
}

/*
 * This function is used to create a TX_COMMIT itx and assign it. This
 * way, it will be linked into the ZIL's list of synchronous itxs, and
 * then later committed to an lwb (or skipped) when
 * zil_process_commit_list() is called.
 */
static void
zil_commit_itx_assign(zilog_t *zilog, zil_commit_waiter_t *zcw)
{
	dmu_tx_t *tx = dmu_tx_create(zilog->zl_os);

	/*
	 * Since we are not going to create any new dirty data, and we
	 * can even help with clearing the existing dirty data, we
	 * should not be subject to the dirty data based delays. We
	 * use DMU_TX_NOTHROTTLE to bypass the delay mechanism.
	 */
	VERIFY0(dmu_tx_assign(tx,
	    DMU_TX_WAIT | DMU_TX_NOTHROTTLE | DMU_TX_SUSPEND));

	itx_t *itx = zil_itx_create(TX_COMMIT, sizeof (lr_t));
	itx->itx_sync = B_TRUE;
	itx->itx_private = zcw;

	zil_itx_assign(zilog, itx, tx);

	dmu_tx_commit(tx);
}

/*
 * Commit ZFS Intent Log transactions (itxs) to stable storage.
 *
 * When writing ZIL transactions to the on-disk representation of the
 * ZIL, the itxs are committed to a Log Write Block (lwb). Multiple
 * itxs can be committed to a single lwb. Once a lwb is written and
 * committed to stable storage (i.e. the lwb is written, and vdevs have
 * been flushed), each itx that was committed to that lwb is also
 * considered to be committed to stable storage.
 *
 * When an itx is committed to an lwb, the log record (lr_t) contained
 * by the itx is copied into the lwb's zio buffer, and once this buffer
 * is written to disk, it becomes an on-disk ZIL block.
 *
 * As itxs are generated, they're inserted into the ZIL's queue of
 * uncommitted itxs. The semantics of zil_commit() are such that it will
 * block until all itxs that were in the queue when it was called, are
 * committed to stable storage.
 *
 * If "foid" is zero, this means all "synchronous" and "asynchronous"
 * itxs, for all objects in the dataset, will be committed to stable
 * storage prior to zil_commit() returning. If "foid" is non-zero, all
 * "synchronous" itxs for all objects, but only "asynchronous" itxs
 * that correspond to the foid passed in, will be committed to stable
 * storage prior to zil_commit() returning.
 *
 * Generally speaking, when zil_commit() is called, the consumer doesn't
 * actually care about _all_ of the uncommitted itxs. Instead, they're
 * simply trying to waiting for a specific itx to be committed to disk,
 * but the interface(s) for interacting with the ZIL don't allow such
 * fine-grained communication. A better interface would allow a consumer
 * to create and assign an itx, and then pass a reference to this itx to
 * zil_commit(); such that zil_commit() would return as soon as that
 * specific itx was committed to disk (instead of waiting for _all_
 * itxs to be committed).
 *
 * When a thread calls zil_commit() a special "commit itx" will be
 * generated, along with a corresponding "waiter" for this commit itx.
 * zil_commit() will wait on this waiter's CV, such that when the waiter
 * is marked done, and signaled, zil_commit() will return.
 *
 * This commit itx is inserted into the queue of uncommitted itxs. This
 * provides an easy mechanism for determining which itxs were in the
 * queue prior to zil_commit() having been called, and which itxs were
 * added after zil_commit() was called.
 *
 * The commit itx is special; it doesn't have any on-disk representation.
 * When a commit itx is "committed" to an lwb, the waiter associated
 * with it is linked onto the lwb's list of waiters. Then, when that lwb
 * completes, each waiter on the lwb's list is marked done and signaled
 * -- allowing the thread waiting on the waiter to return from zil_commit().
 *
 * It's important to point out a few critical factors that allow us
 * to make use of the commit itxs, commit waiters, per-lwb lists of
 * commit waiters, and zio completion callbacks like we're doing:
 *
 *   1. The list of waiters for each lwb is traversed, and each commit
 *      waiter is marked "done" and signaled, in the zio completion
 *      callback of the lwb's zio[*].
 *
 *      * Actually, the waiters are signaled in the zio completion
 *        callback of the root zio for the flush commands that are sent to
 *        the vdevs upon completion of the lwb zio.
 *
 *   2. When the itxs are inserted into the ZIL's queue of uncommitted
 *      itxs, the order in which they are inserted is preserved[*]; as
 *      itxs are added to the queue, they are added to the tail of
 *      in-memory linked lists.
 *
 *      When committing the itxs to lwbs (to be written to disk), they
 *      are committed in the same order in which the itxs were added to
 *      the uncommitted queue's linked list(s); i.e. the linked list of
 *      itxs to commit is traversed from head to tail, and each itx is
 *      committed to an lwb in that order.
 *
 *      * To clarify:
 *
 *        - the order of "sync" itxs is preserved w.r.t. other
 *          "sync" itxs, regardless of the corresponding objects.
 *        - the order of "async" itxs is preserved w.r.t. other
 *          "async" itxs corresponding to the same object.
 *        - the order of "async" itxs is *not* preserved w.r.t. other
 *          "async" itxs corresponding to different objects.
 *        - the order of "sync" itxs w.r.t. "async" itxs (or vice
 *          versa) is *not* preserved, even for itxs that correspond
 *          to the same object.
 *
 *      For more details, see: zil_itx_assign(), zil_async_to_sync(),
 *      zil_get_commit_list(), and zil_process_commit_list().
 *
 *   3. The lwbs represent a linked list of blocks on disk. Thus, any
 *      lwb cannot be considered committed to stable storage, until its
 *      "previous" lwb is also committed to stable storage. This fact,
 *      coupled with the fact described above, means that itxs are
 *      committed in (roughly) the order in which they were generated.
 *      This is essential because itxs are dependent on prior itxs.
 *      Thus, we *must not* deem an itx as being committed to stable
 *      storage, until *all* prior itxs have also been committed to
 *      stable storage.
 *
 *      To enforce this ordering of lwb zio's, while still leveraging as
 *      much of the underlying storage performance as possible, we rely
 *      on two fundamental concepts:
 *
 *          1. The creation and issuance of lwb zio's is protected by
 *             the zilog's "zl_issuer_lock", which ensures only a single
 *             thread is creating and/or issuing lwb's at a time
 *          2. The "previous" lwb is a child of the "current" lwb
 *             (leveraging the zio parent-child dependency graph)
 *
 *      By relying on this parent-child zio relationship, we can have
 *      many lwb zio's concurrently issued to the underlying storage,
 *      but the order in which they complete will be the same order in
 *      which they were created.
 */
void
zil_commit(zilog_t *zilog, uint64_t foid)
{
	/*
	 * We should never attempt to call zil_commit on a snapshot for
	 * a couple of reasons:
	 *
	 * 1. A snapshot may never be modified, thus it cannot have any
	 *    in-flight itxs that would have modified the dataset.
	 *
	 * 2. By design, when zil_commit() is called, a commit itx will
	 *    be assigned to this zilog; as a result, the zilog will be
	 *    dirtied. We must not dirty the zilog of a snapshot; there's
	 *    checks in the code that enforce this invariant, and will
	 *    cause a panic if it's not upheld.
	 */
	ASSERT3B(dmu_objset_is_snapshot(zilog->zl_os), ==, B_FALSE);

	if (zilog->zl_sync == ZFS_SYNC_DISABLED)
		return;

	if (!spa_writeable(zilog->zl_spa)) {
		/*
		 * If the SPA is not writable, there should never be any
		 * pending itxs waiting to be committed to disk. If that
		 * weren't true, we'd skip writing those itxs out, and
		 * would break the semantics of zil_commit(); thus, we're
		 * verifying that truth before we return to the caller.
		 */
		ASSERT(list_is_empty(&zilog->zl_lwb_list));
		ASSERT3P(zilog->zl_last_lwb_opened, ==, NULL);
		for (int i = 0; i < TXG_SIZE; i++)
			ASSERT3P(zilog->zl_itxg[i].itxg_itxs, ==, NULL);
		return;
	}

	/*
	 * If the ZIL is suspended, we don't want to dirty it by calling
	 * zil_commit_itx_assign() below, nor can we write out
	 * lwbs like would be done in zil_commit_write(). Thus, we
	 * simply rely on txg_wait_synced() to maintain the necessary
	 * semantics, and avoid calling those functions altogether.
	 */
	if (zilog->zl_suspend > 0) {
		ZIL_STAT_BUMP(zilog, zil_commit_suspend_count);
		txg_wait_synced(zilog->zl_dmu_pool, 0);
		return;
	}

	zil_commit_impl(zilog, foid);
}

void
zil_commit_impl(zilog_t *zilog, uint64_t foid)
{
	ZIL_STAT_BUMP(zilog, zil_commit_count);

	/*
	 * Move the "async" itxs for the specified foid to the "sync"
	 * queues, such that they will be later committed (or skipped)
	 * to an lwb when zil_process_commit_list() is called.
	 *
	 * Since these "async" itxs must be committed prior to this
	 * call to zil_commit returning, we must perform this operation
	 * before we call zil_commit_itx_assign().
	 */
	zil_async_to_sync(zilog, foid);

	/*
	 * We allocate a new "waiter" structure which will initially be
	 * linked to the commit itx using the itx's "itx_private" field.
	 * Since the commit itx doesn't represent any on-disk state,
	 * when it's committed to an lwb, rather than copying the its
	 * lr_t into the lwb's buffer, the commit itx's "waiter" will be
	 * added to the lwb's list of waiters. Then, when the lwb is
	 * committed to stable storage, each waiter in the lwb's list of
	 * waiters will be marked "done", and signalled.
	 *
	 * We must create the waiter and assign the commit itx prior to
	 * calling zil_commit_writer(), or else our specific commit itx
	 * is not guaranteed to be committed to an lwb prior to calling
	 * zil_commit_waiter().
	 */
	zil_commit_waiter_t *zcw = zil_alloc_commit_waiter();
	zil_commit_itx_assign(zilog, zcw);

	uint64_t wtxg = zil_commit_writer(zilog, zcw);
	zil_commit_waiter(zilog, zcw);

	if (zcw->zcw_zio_error != 0) {
		/*
		 * If there was an error writing out the ZIL blocks that
		 * this thread is waiting on, then we fallback to
		 * relying on spa_sync() to write out the data this
		 * thread is waiting on. Obviously this has performance
		 * implications, but the expectation is for this to be
		 * an exceptional case, and shouldn't occur often.
		 */
		ZIL_STAT_BUMP(zilog, zil_commit_error_count);
		DTRACE_PROBE2(zil__commit__io__error,
		    zilog_t *, zilog, zil_commit_waiter_t *, zcw);
		txg_wait_synced(zilog->zl_dmu_pool, 0);
	} else if (wtxg != 0) {
		ZIL_STAT_BUMP(zilog, zil_commit_suspend_count);
		txg_wait_synced(zilog->zl_dmu_pool, wtxg);
	}

	zil_free_commit_waiter(zcw);
}

/*
 * Called in syncing context to free committed log blocks and update log header.
 */
void
zil_sync(zilog_t *zilog, dmu_tx_t *tx)
{
	zil_header_t *zh = zil_header_in_syncing_context(zilog);
	uint64_t txg = dmu_tx_get_txg(tx);
	spa_t *spa = zilog->zl_spa;
	uint64_t *replayed_seq = &zilog->zl_replayed_seq[txg & TXG_MASK];
	lwb_t *lwb;

	/*
	 * We don't zero out zl_destroy_txg, so make sure we don't try
	 * to destroy it twice.
	 */
	if (spa_sync_pass(spa) != 1)
		return;

	zil_lwb_flush_wait_all(zilog, txg);

	mutex_enter(&zilog->zl_lock);

	ASSERT(zilog->zl_stop_sync == 0);

	if (*replayed_seq != 0) {
		ASSERT(zh->zh_replay_seq < *replayed_seq);
		zh->zh_replay_seq = *replayed_seq;
		*replayed_seq = 0;
	}

	if (zilog->zl_destroy_txg == txg) {
		blkptr_t blk = zh->zh_log;
		dsl_dataset_t *ds = dmu_objset_ds(zilog->zl_os);

		ASSERT(list_is_empty(&zilog->zl_lwb_list));

		memset(zh, 0, sizeof (zil_header_t));
		memset(zilog->zl_replayed_seq, 0,
		    sizeof (zilog->zl_replayed_seq));

		if (zilog->zl_keep_first) {
			/*
			 * If this block was part of log chain that couldn't
			 * be claimed because a device was missing during
			 * zil_claim(), but that device later returns,
			 * then this block could erroneously appear valid.
			 * To guard against this, assign a new GUID to the new
			 * log chain so it doesn't matter what blk points to.
			 */
			zil_init_log_chain(zilog, &blk);
			zh->zh_log = blk;
		} else {
			/*
			 * A destroyed ZIL chain can't contain any TX_SETSAXATTR
			 * records. So, deactivate the feature for this dataset.
			 * We activate it again when we start a new ZIL chain.
			 */
			if (dsl_dataset_feature_is_active(ds,
			    SPA_FEATURE_ZILSAXATTR))
				dsl_dataset_deactivate_feature(ds,
				    SPA_FEATURE_ZILSAXATTR, tx);
		}
	}

	while ((lwb = list_head(&zilog->zl_lwb_list)) != NULL) {
		zh->zh_log = lwb->lwb_blk;
		if (lwb->lwb_state != LWB_STATE_FLUSH_DONE ||
		    lwb->lwb_alloc_txg > txg || lwb->lwb_max_txg > txg)
			break;
		list_remove(&zilog->zl_lwb_list, lwb);
		if (!BP_IS_HOLE(&lwb->lwb_blk))
			zio_free(spa, txg, &lwb->lwb_blk);
		zil_free_lwb(zilog, lwb);

		/*
		 * If we don't have anything left in the lwb list then
		 * we've had an allocation failure and we need to zero
		 * out the zil_header blkptr so that we don't end
		 * up freeing the same block twice.
		 */
		if (list_is_empty(&zilog->zl_lwb_list))
			BP_ZERO(&zh->zh_log);
	}

	mutex_exit(&zilog->zl_lock);
}

static int
zil_lwb_cons(void *vbuf, void *unused, int kmflag)
{
	(void) unused, (void) kmflag;
	lwb_t *lwb = vbuf;
	list_create(&lwb->lwb_itxs, sizeof (itx_t), offsetof(itx_t, itx_node));
	list_create(&lwb->lwb_waiters, sizeof (zil_commit_waiter_t),
	    offsetof(zil_commit_waiter_t, zcw_node));
	avl_create(&lwb->lwb_vdev_tree, zil_lwb_vdev_compare,
	    sizeof (zil_vdev_node_t), offsetof(zil_vdev_node_t, zv_node));
	mutex_init(&lwb->lwb_vdev_lock, NULL, MUTEX_DEFAULT, NULL);
	return (0);
}

static void
zil_lwb_dest(void *vbuf, void *unused)
{
	(void) unused;
	lwb_t *lwb = vbuf;
	mutex_destroy(&lwb->lwb_vdev_lock);
	avl_destroy(&lwb->lwb_vdev_tree);
	list_destroy(&lwb->lwb_waiters);
	list_destroy(&lwb->lwb_itxs);
}

void
zil_init(void)
{
	zil_lwb_cache = kmem_cache_create("zil_lwb_cache",
	    sizeof (lwb_t), 0, zil_lwb_cons, zil_lwb_dest, NULL, NULL, NULL, 0);

	zil_zcw_cache = kmem_cache_create("zil_zcw_cache",
	    sizeof (zil_commit_waiter_t), 0, NULL, NULL, NULL, NULL, NULL, 0);

	zil_sums_init(&zil_sums_global);
	zil_kstats_global = kstat_create("zfs", 0, "zil", "misc",
	    KSTAT_TYPE_NAMED, sizeof (zil_stats) / sizeof (kstat_named_t),
	    KSTAT_FLAG_VIRTUAL);

	if (zil_kstats_global != NULL) {
		zil_kstats_global->ks_data = &zil_stats;
		zil_kstats_global->ks_update = zil_kstats_global_update;
		zil_kstats_global->ks_private = NULL;
		kstat_install(zil_kstats_global);
	}
}

void
zil_fini(void)
{
	kmem_cache_destroy(zil_zcw_cache);
	kmem_cache_destroy(zil_lwb_cache);

	if (zil_kstats_global != NULL) {
		kstat_delete(zil_kstats_global);
		zil_kstats_global = NULL;
	}

	zil_sums_fini(&zil_sums_global);
}

void
zil_set_sync(zilog_t *zilog, uint64_t sync)
{
	zilog->zl_sync = sync;
}

void
zil_set_logbias(zilog_t *zilog, uint64_t logbias)
{
	zilog->zl_logbias = logbias;
}

zilog_t *
zil_alloc(objset_t *os, zil_header_t *zh_phys)
{
	zilog_t *zilog;

	zilog = kmem_zalloc(sizeof (zilog_t), KM_SLEEP);

	zilog->zl_header = zh_phys;
	zilog->zl_os = os;
	zilog->zl_spa = dmu_objset_spa(os);
	zilog->zl_dmu_pool = dmu_objset_pool(os);
	zilog->zl_destroy_txg = TXG_INITIAL - 1;
	zilog->zl_logbias = dmu_objset_logbias(os);
	zilog->zl_sync = dmu_objset_syncprop(os);
	zilog->zl_dirty_max_txg = 0;
	zilog->zl_last_lwb_opened = NULL;
	zilog->zl_last_lwb_latency = 0;
	zilog->zl_max_block_size = MIN(MAX(P2ALIGN_TYPED(zil_maxblocksize,
	    ZIL_MIN_BLKSZ, uint64_t), ZIL_MIN_BLKSZ),
	    spa_maxblocksize(dmu_objset_spa(os)));

	mutex_init(&zilog->zl_lock, NULL, MUTEX_DEFAULT, NULL);
	mutex_init(&zilog->zl_issuer_lock, NULL, MUTEX_DEFAULT, NULL);
	mutex_init(&zilog->zl_lwb_io_lock, NULL, MUTEX_DEFAULT, NULL);

	for (int i = 0; i < TXG_SIZE; i++) {
		mutex_init(&zilog->zl_itxg[i].itxg_lock, NULL,
		    MUTEX_DEFAULT, NULL);
	}

	list_create(&zilog->zl_lwb_list, sizeof (lwb_t),
	    offsetof(lwb_t, lwb_node));

	list_create(&zilog->zl_itx_commit_list, sizeof (itx_t),
	    offsetof(itx_t, itx_node));

	cv_init(&zilog->zl_cv_suspend, NULL, CV_DEFAULT, NULL);
	cv_init(&zilog->zl_lwb_io_cv, NULL, CV_DEFAULT, NULL);

	for (int i = 0; i < ZIL_BURSTS; i++) {
		zilog->zl_prev_opt[i] = zilog->zl_max_block_size -
		    sizeof (zil_chain_t);
	}

	return (zilog);
}

void
zil_free(zilog_t *zilog)
{
	int i;

	zilog->zl_stop_sync = 1;

	ASSERT0(zilog->zl_suspend);
	ASSERT0(zilog->zl_suspending);

	ASSERT(list_is_empty(&zilog->zl_lwb_list));
	list_destroy(&zilog->zl_lwb_list);

	ASSERT(list_is_empty(&zilog->zl_itx_commit_list));
	list_destroy(&zilog->zl_itx_commit_list);

	for (i = 0; i < TXG_SIZE; i++) {
		/*
		 * It's possible for an itx to be generated that doesn't dirty
		 * a txg (e.g. ztest TX_TRUNCATE). So there's no zil_clean()
		 * callback to remove the entry. We remove those here.
		 *
		 * Also free up the ziltest itxs.
		 */
		if (zilog->zl_itxg[i].itxg_itxs)
			zil_itxg_clean(zilog->zl_itxg[i].itxg_itxs);
		mutex_destroy(&zilog->zl_itxg[i].itxg_lock);
	}

	mutex_destroy(&zilog->zl_issuer_lock);
	mutex_destroy(&zilog->zl_lock);
	mutex_destroy(&zilog->zl_lwb_io_lock);

	cv_destroy(&zilog->zl_cv_suspend);
	cv_destroy(&zilog->zl_lwb_io_cv);

	kmem_free(zilog, sizeof (zilog_t));
}

/*
 * Open an intent log.
 */
zilog_t *
zil_open(objset_t *os, zil_get_data_t *get_data, zil_sums_t *zil_sums)
{
	zilog_t *zilog = dmu_objset_zil(os);

	ASSERT3P(zilog->zl_get_data, ==, NULL);
	ASSERT3P(zilog->zl_last_lwb_opened, ==, NULL);
	ASSERT(list_is_empty(&zilog->zl_lwb_list));

	zilog->zl_get_data = get_data;
	zilog->zl_sums = zil_sums;

	return (zilog);
}

/*
 * Close an intent log.
 */
void
zil_close(zilog_t *zilog)
{
	lwb_t *lwb;
	uint64_t txg;

	if (!dmu_objset_is_snapshot(zilog->zl_os)) {
		zil_commit(zilog, 0);
	} else {
		ASSERT(list_is_empty(&zilog->zl_lwb_list));
		ASSERT0(zilog->zl_dirty_max_txg);
		ASSERT3B(zilog_is_dirty(zilog), ==, B_FALSE);
	}

	mutex_enter(&zilog->zl_lock);
	txg = zilog->zl_dirty_max_txg;
	lwb = list_tail(&zilog->zl_lwb_list);
	if (lwb != NULL) {
		txg = MAX(txg, lwb->lwb_alloc_txg);
		txg = MAX(txg, lwb->lwb_max_txg);
	}
	mutex_exit(&zilog->zl_lock);

	/*
	 * zl_lwb_max_issued_txg may be larger than lwb_max_txg. It depends
	 * on the time when the dmu_tx transaction is assigned in
	 * zil_lwb_write_issue().
	 */
	mutex_enter(&zilog->zl_lwb_io_lock);
	txg = MAX(zilog->zl_lwb_max_issued_txg, txg);
	mutex_exit(&zilog->zl_lwb_io_lock);

	/*
	 * We need to use txg_wait_synced() to wait until that txg is synced.
	 * zil_sync() will guarantee all lwbs up to that txg have been
	 * written out, flushed, and cleaned.
	 */
	if (txg != 0)
		txg_wait_synced(zilog->zl_dmu_pool, txg);

	if (zilog_is_dirty(zilog))
		zfs_dbgmsg("zil (%px) is dirty, txg %llu", zilog,
		    (u_longlong_t)txg);
	if (txg < spa_freeze_txg(zilog->zl_spa))
		VERIFY(!zilog_is_dirty(zilog));

	zilog->zl_get_data = NULL;

	/*
	 * We should have only one lwb left on the list; remove it now.
	 */
	mutex_enter(&zilog->zl_lock);
	lwb = list_remove_head(&zilog->zl_lwb_list);
	if (lwb != NULL) {
		ASSERT(list_is_empty(&zilog->zl_lwb_list));
		ASSERT3S(lwb->lwb_state, ==, LWB_STATE_NEW);
		zio_buf_free(lwb->lwb_buf, lwb->lwb_sz);
		zil_free_lwb(zilog, lwb);
	}
	mutex_exit(&zilog->zl_lock);
}

static const char *suspend_tag = "zil suspending";

/*
 * Suspend an intent log.  While in suspended mode, we still honor
 * synchronous semantics, but we rely on txg_wait_synced() to do it.
 * On old version pools, we suspend the log briefly when taking a
 * snapshot so that it will have an empty intent log.
 *
 * Long holds are not really intended to be used the way we do here --
 * held for such a short time.  A concurrent caller of dsl_dataset_long_held()
 * could fail.  Therefore we take pains to only put a long hold if it is
 * actually necessary.  Fortunately, it will only be necessary if the
 * objset is currently mounted (or the ZVOL equivalent).  In that case it
 * will already have a long hold, so we are not really making things any worse.
 *
 * Ideally, we would locate the existing long-holder (i.e. the zfsvfs_t or
 * zvol_state_t), and use their mechanism to prevent their hold from being
 * dropped (e.g. VFS_HOLD()).  However, that would be even more pain for
 * very little gain.
 *
 * if cookiep == NULL, this does both the suspend & resume.
 * Otherwise, it returns with the dataset "long held", and the cookie
 * should be passed into zil_resume().
 */
int
zil_suspend(const char *osname, void **cookiep)
{
	objset_t *os;
	zilog_t *zilog;
	const zil_header_t *zh;
	int error;

	error = dmu_objset_hold(osname, suspend_tag, &os);
	if (error != 0)
		return (error);
	zilog = dmu_objset_zil(os);

	mutex_enter(&zilog->zl_lock);
	zh = zilog->zl_header;

	if (zh->zh_flags & ZIL_REPLAY_NEEDED) {		/* unplayed log */
		mutex_exit(&zilog->zl_lock);
		dmu_objset_rele(os, suspend_tag);
		return (SET_ERROR(EBUSY));
	}

	/*
	 * Don't put a long hold in the cases where we can avoid it.  This
	 * is when there is no cookie so we are doing a suspend & resume
	 * (i.e. called from zil_vdev_offline()), and there's nothing to do
	 * for the suspend because it's already suspended, or there's no ZIL.
	 */
	if (cookiep == NULL && !zilog->zl_suspending &&
	    (zilog->zl_suspend > 0 || BP_IS_HOLE(&zh->zh_log))) {
		mutex_exit(&zilog->zl_lock);
		dmu_objset_rele(os, suspend_tag);
		return (0);
	}

	dsl_dataset_long_hold(dmu_objset_ds(os), suspend_tag);
	dsl_pool_rele(dmu_objset_pool(os), suspend_tag);

	zilog->zl_suspend++;

	if (zilog->zl_suspend > 1) {
		/*
		 * Someone else is already suspending it.
		 * Just wait for them to finish.
		 */

		while (zilog->zl_suspending)
			cv_wait(&zilog->zl_cv_suspend, &zilog->zl_lock);
		mutex_exit(&zilog->zl_lock);

		if (cookiep == NULL)
			zil_resume(os);
		else
			*cookiep = os;
		return (0);
	}

	/*
	 * If there is no pointer to an on-disk block, this ZIL must not
	 * be active (e.g. filesystem not mounted), so there's nothing
	 * to clean up.
	 */
	if (BP_IS_HOLE(&zh->zh_log)) {
		ASSERT(cookiep != NULL); /* fast path already handled */

		*cookiep = os;
		mutex_exit(&zilog->zl_lock);
		return (0);
	}

	/*
	 * The ZIL has work to do. Ensure that the associated encryption
	 * key will remain mapped while we are committing the log by
	 * grabbing a reference to it. If the key isn't loaded we have no
	 * choice but to return an error until the wrapping key is loaded.
	 */
	if (os->os_encrypted &&
	    dsl_dataset_create_key_mapping(dmu_objset_ds(os)) != 0) {
		zilog->zl_suspend--;
		mutex_exit(&zilog->zl_lock);
		dsl_dataset_long_rele(dmu_objset_ds(os), suspend_tag);
		dsl_dataset_rele(dmu_objset_ds(os), suspend_tag);
		return (SET_ERROR(EACCES));
	}

	zilog->zl_suspending = B_TRUE;
	mutex_exit(&zilog->zl_lock);

	/*
	 * We need to use zil_commit_impl to ensure we wait for all
	 * LWB_STATE_OPENED, _CLOSED and _READY lwbs to be committed
	 * to disk before proceeding. If we used zil_commit instead, it
	 * would just call txg_wait_synced(), because zl_suspend is set.
	 * txg_wait_synced() doesn't wait for these lwb's to be
	 * LWB_STATE_FLUSH_DONE before returning.
	 */
	zil_commit_impl(zilog, 0);

	/*
	 * Now that we've ensured all lwb's are LWB_STATE_FLUSH_DONE, we
	 * use txg_wait_synced() to ensure the data from the zilog has
	 * migrated to the main pool before calling zil_destroy().
	 */
	txg_wait_synced(zilog->zl_dmu_pool, 0);

	zil_destroy(zilog, B_FALSE);

	mutex_enter(&zilog->zl_lock);
	zilog->zl_suspending = B_FALSE;
	cv_broadcast(&zilog->zl_cv_suspend);
	mutex_exit(&zilog->zl_lock);

	if (os->os_encrypted)
		dsl_dataset_remove_key_mapping(dmu_objset_ds(os));

	if (cookiep == NULL)
		zil_resume(os);
	else
		*cookiep = os;
	return (0);
}

void
zil_resume(void *cookie)
{
	objset_t *os = cookie;
	zilog_t *zilog = dmu_objset_zil(os);

	mutex_enter(&zilog->zl_lock);
	ASSERT(zilog->zl_suspend != 0);
	zilog->zl_suspend--;
	mutex_exit(&zilog->zl_lock);
	dsl_dataset_long_rele(dmu_objset_ds(os), suspend_tag);
	dsl_dataset_rele(dmu_objset_ds(os), suspend_tag);
}

typedef struct zil_replay_arg {
	zil_replay_func_t *const *zr_replay;
	void		*zr_arg;
	boolean_t	zr_byteswap;
	char		*zr_lr;
} zil_replay_arg_t;

static int
zil_replay_error(zilog_t *zilog, const lr_t *lr, int error)
{
	char name[ZFS_MAX_DATASET_NAME_LEN];

	zilog->zl_replaying_seq--;	/* didn't actually replay this one */

	dmu_objset_name(zilog->zl_os, name);

	cmn_err(CE_WARN, "ZFS replay transaction error %d, "
	    "dataset %s, seq 0x%llx, txtype %llu %s\n", error, name,
	    (u_longlong_t)lr->lrc_seq,
	    (u_longlong_t)(lr->lrc_txtype & ~TX_CI),
	    (lr->lrc_txtype & TX_CI) ? "CI" : "");

	return (error);
}

static int
zil_replay_log_record(zilog_t *zilog, const lr_t *lr, void *zra,
    uint64_t claim_txg)
{
	zil_replay_arg_t *zr = zra;
	const zil_header_t *zh = zilog->zl_header;
	uint64_t reclen = lr->lrc_reclen;
	uint64_t txtype = lr->lrc_txtype;
	int error = 0;

	zilog->zl_replaying_seq = lr->lrc_seq;

	if (lr->lrc_seq <= zh->zh_replay_seq)	/* already replayed */
		return (0);

	if (lr->lrc_txg < claim_txg)		/* already committed */
		return (0);

	/* Strip case-insensitive bit, still present in log record */
	txtype &= ~TX_CI;

	if (txtype == 0 || txtype >= TX_MAX_TYPE)
		return (zil_replay_error(zilog, lr, EINVAL));

	/*
	 * If this record type can be logged out of order, the object
	 * (lr_foid) may no longer exist.  That's legitimate, not an error.
	 */
	if (TX_OOO(txtype)) {
		error = dmu_object_info(zilog->zl_os,
		    LR_FOID_GET_OBJ(((lr_ooo_t *)lr)->lr_foid), NULL);
		if (error == ENOENT || error == EEXIST)
			return (0);
	}

	/*
	 * Make a copy of the data so we can revise and extend it.
	 */
	memcpy(zr->zr_lr, lr, reclen);

	/*
	 * If this is a TX_WRITE with a blkptr, suck in the data.
	 */
	if (txtype == TX_WRITE && reclen == sizeof (lr_write_t)) {
		error = zil_read_log_data(zilog, (lr_write_t *)lr,
		    zr->zr_lr + reclen);
		if (error != 0)
			return (zil_replay_error(zilog, lr, error));
	}

	/*
	 * The log block containing this lr may have been byteswapped
	 * so that we can easily examine common fields like lrc_txtype.
	 * However, the log is a mix of different record types, and only the
	 * replay vectors know how to byteswap their records.  Therefore, if
	 * the lr was byteswapped, undo it before invoking the replay vector.
	 */
	if (zr->zr_byteswap)
		byteswap_uint64_array(zr->zr_lr, reclen);

	/*
	 * We must now do two things atomically: replay this log record,
	 * and update the log header sequence number to reflect the fact that
	 * we did so. At the end of each replay function the sequence number
	 * is updated if we are in replay mode.
	 */
	error = zr->zr_replay[txtype](zr->zr_arg, zr->zr_lr, zr->zr_byteswap);
	if (error != 0) {
		/*
		 * The DMU's dnode layer doesn't see removes until the txg
		 * commits, so a subsequent claim can spuriously fail with
		 * EEXIST. So if we receive any error we try syncing out
		 * any removes then retry the transaction.  Note that we
		 * specify B_FALSE for byteswap now, so we don't do it twice.
		 */
		txg_wait_synced(spa_get_dsl(zilog->zl_spa), 0);
		error = zr->zr_replay[txtype](zr->zr_arg, zr->zr_lr, B_FALSE);
		if (error != 0)
			return (zil_replay_error(zilog, lr, error));
	}
	return (0);
}

static int
zil_incr_blks(zilog_t *zilog, const blkptr_t *bp, void *arg, uint64_t claim_txg)
{
	(void) bp, (void) arg, (void) claim_txg;

	zilog->zl_replay_blks++;

	return (0);
}

/*
 * If this dataset has a non-empty intent log, replay it and destroy it.
 * Return B_TRUE if there were any entries to replay.
 */
boolean_t
zil_replay(objset_t *os, void *arg,
    zil_replay_func_t *const replay_func[TX_MAX_TYPE])
{
	zilog_t *zilog = dmu_objset_zil(os);
	const zil_header_t *zh = zilog->zl_header;
	zil_replay_arg_t zr;

	if ((zh->zh_flags & ZIL_REPLAY_NEEDED) == 0) {
		return (zil_destroy(zilog, B_TRUE));
	}

	zr.zr_replay = replay_func;
	zr.zr_arg = arg;
	zr.zr_byteswap = BP_SHOULD_BYTESWAP(&zh->zh_log);
	zr.zr_lr = vmem_alloc(2 * SPA_MAXBLOCKSIZE, KM_SLEEP);

	/*
	 * Wait for in-progress removes to sync before starting replay.
	 */
	txg_wait_synced(zilog->zl_dmu_pool, 0);

	zilog->zl_replay = B_TRUE;
	zilog->zl_replay_time = ddi_get_lbolt();
	ASSERT(zilog->zl_replay_blks == 0);
	(void) zil_parse(zilog, zil_incr_blks, zil_replay_log_record, &zr,
	    zh->zh_claim_txg, B_TRUE);
	vmem_free(zr.zr_lr, 2 * SPA_MAXBLOCKSIZE);

	zil_destroy(zilog, B_FALSE);
	txg_wait_synced(zilog->zl_dmu_pool, zilog->zl_destroy_txg);
	zilog->zl_replay = B_FALSE;

	return (B_TRUE);
}

boolean_t
zil_replaying(zilog_t *zilog, dmu_tx_t *tx)
{
	if (zilog->zl_sync == ZFS_SYNC_DISABLED)
		return (B_TRUE);

	if (zilog->zl_replay) {
		dsl_dataset_dirty(dmu_objset_ds(zilog->zl_os), tx);
		zilog->zl_replayed_seq[dmu_tx_get_txg(tx) & TXG_MASK] =
		    zilog->zl_replaying_seq;
		return (B_TRUE);
	}

	return (B_FALSE);
}

int
zil_reset(const char *osname, void *arg)
{
	(void) arg;

	int error = zil_suspend(osname, NULL);
	/* EACCES means crypto key not loaded */
	if ((error == EACCES) || (error == EBUSY))
		return (SET_ERROR(error));
	if (error != 0)
		return (SET_ERROR(EEXIST));
	return (0);
}

EXPORT_SYMBOL(zil_alloc);
EXPORT_SYMBOL(zil_free);
EXPORT_SYMBOL(zil_open);
EXPORT_SYMBOL(zil_close);
EXPORT_SYMBOL(zil_replay);
EXPORT_SYMBOL(zil_replaying);
EXPORT_SYMBOL(zil_destroy);
EXPORT_SYMBOL(zil_destroy_sync);
EXPORT_SYMBOL(zil_itx_create);
EXPORT_SYMBOL(zil_itx_destroy);
EXPORT_SYMBOL(zil_itx_assign);
EXPORT_SYMBOL(zil_commit);
EXPORT_SYMBOL(zil_claim);
EXPORT_SYMBOL(zil_check_log_chain);
EXPORT_SYMBOL(zil_sync);
EXPORT_SYMBOL(zil_clean);
EXPORT_SYMBOL(zil_suspend);
EXPORT_SYMBOL(zil_resume);
EXPORT_SYMBOL(zil_lwb_add_block);
EXPORT_SYMBOL(zil_bp_tree_add);
EXPORT_SYMBOL(zil_set_sync);
EXPORT_SYMBOL(zil_set_logbias);
EXPORT_SYMBOL(zil_sums_init);
EXPORT_SYMBOL(zil_sums_fini);
EXPORT_SYMBOL(zil_kstat_values_update);

ZFS_MODULE_PARAM(zfs, zfs_, commit_timeout_pct, UINT, ZMOD_RW,
	"ZIL block open timeout percentage");

ZFS_MODULE_PARAM(zfs_zil, zil_, replay_disable, INT, ZMOD_RW,
	"Disable intent logging replay");

ZFS_MODULE_PARAM(zfs_zil, zil_, nocacheflush, INT, ZMOD_RW,
	"Disable ZIL cache flushes");

ZFS_MODULE_PARAM(zfs_zil, zil_, slog_bulk, U64, ZMOD_RW,
	"Limit in bytes slog sync writes per commit");

ZFS_MODULE_PARAM(zfs_zil, zil_, maxblocksize, UINT, ZMOD_RW,
	"Limit in bytes of ZIL log block size");

ZFS_MODULE_PARAM(zfs_zil, zil_, maxcopied, UINT, ZMOD_RW,
	"Limit in bytes WR_COPIED size");

ZFS_MODULE_PARAM(zfs, zfs_, immediate_write_sz, UINT, ZMOD_RW,
	"Largest write size to store data into ZIL");

ZFS_MODULE_PARAM(zfs_zil, zil_, special_is_slog, INT, ZMOD_RW,
	"Treat special vdevs as SLOG");
