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
 * Copyright (c) 2017, Intel Corporation.
 */

#ifndef _SYS_METASLAB_H
#define	_SYS_METASLAB_H

#include <sys/spa.h>
#include <sys/space_map.h>
#include <sys/txg.h>
#include <sys/zio.h>
#include <sys/avl.h>

#ifdef	__cplusplus
extern "C" {
#endif


typedef struct metaslab_ops {
	const char *msop_name;
	uint64_t (*msop_alloc)(metaslab_t *, uint64_t, uint64_t, uint64_t *);
} metaslab_ops_t;


extern const metaslab_ops_t zfs_metaslab_ops;

int metaslab_init(metaslab_group_t *, uint64_t, uint64_t, uint64_t,
    metaslab_t **);
void metaslab_fini(metaslab_t *);

void metaslab_set_unflushed_dirty(metaslab_t *, boolean_t);
void metaslab_set_unflushed_txg(metaslab_t *, uint64_t, dmu_tx_t *);
void metaslab_set_estimated_condensed_size(metaslab_t *, uint64_t, dmu_tx_t *);
boolean_t metaslab_unflushed_dirty(metaslab_t *);
uint64_t metaslab_unflushed_txg(metaslab_t *);
uint64_t metaslab_estimated_condensed_size(metaslab_t *);
int metaslab_sort_by_flushed(const void *, const void *);
void metaslab_unflushed_bump(metaslab_t *, dmu_tx_t *, boolean_t);
uint64_t metaslab_unflushed_changes_memused(metaslab_t *);

int metaslab_load(metaslab_t *);
void metaslab_unload(metaslab_t *);
boolean_t metaslab_flush(metaslab_t *, dmu_tx_t *);

uint64_t metaslab_allocated_space(metaslab_t *);

void metaslab_sync(metaslab_t *, uint64_t);
void metaslab_sync_done(metaslab_t *, uint64_t);
void metaslab_sync_reassess(metaslab_group_t *);
uint64_t metaslab_largest_allocatable(metaslab_t *);

/*
 * metaslab alloc flags
 */
#define	METASLAB_ZIL			0x1
#define	METASLAB_GANG_HEADER		0x2
#define	METASLAB_GANG_CHILD		0x4
#define	METASLAB_ASYNC_ALLOC		0x8

int metaslab_alloc(spa_t *, metaslab_class_t *, uint64_t, blkptr_t *, int,
    uint64_t, const blkptr_t *, int, zio_alloc_list_t *, int, const void *);
int metaslab_alloc_range(spa_t *, metaslab_class_t *, uint64_t, uint64_t,
    blkptr_t *, int, uint64_t, const blkptr_t *, int, zio_alloc_list_t *,
    int, const void *, uint64_t *);
int metaslab_alloc_dva(spa_t *, metaslab_class_t *, uint64_t,
    dva_t *, int, const dva_t *, uint64_t, int, zio_alloc_list_t *, int);
void metaslab_free(spa_t *, const blkptr_t *, uint64_t, boolean_t);
void metaslab_free_concrete(vdev_t *, uint64_t, uint64_t, boolean_t);
void metaslab_free_dva(spa_t *, const dva_t *, boolean_t);
void metaslab_free_impl_cb(uint64_t, vdev_t *, uint64_t, uint64_t, void *);
void metaslab_unalloc_dva(spa_t *, const dva_t *, uint64_t);
int metaslab_claim(spa_t *, const blkptr_t *, uint64_t);
int metaslab_claim_impl(vdev_t *, uint64_t, uint64_t, uint64_t);
void metaslab_check_free(spa_t *, const blkptr_t *);

void metaslab_stat_init(void);
void metaslab_stat_fini(void);
void metaslab_trace_move(zio_alloc_list_t *, zio_alloc_list_t *);
void metaslab_trace_init(zio_alloc_list_t *);
void metaslab_trace_fini(zio_alloc_list_t *);

metaslab_class_t *metaslab_class_create(spa_t *, const char *,
    const metaslab_ops_t *, boolean_t);
void metaslab_class_destroy(metaslab_class_t *);
void metaslab_class_validate(metaslab_class_t *);
void metaslab_class_balance(metaslab_class_t *mc, boolean_t onsync);
void metaslab_class_histogram_verify(metaslab_class_t *);
uint64_t metaslab_class_fragmentation(metaslab_class_t *);
uint64_t metaslab_class_expandable_space(metaslab_class_t *);
boolean_t metaslab_class_throttle_reserve(metaslab_class_t *, int, int,
    uint64_t, boolean_t, boolean_t *);
boolean_t metaslab_class_throttle_unreserve(metaslab_class_t *, int, int,
    uint64_t);
void metaslab_class_evict_old(metaslab_class_t *, uint64_t);
const char *metaslab_class_get_name(metaslab_class_t *);
uint64_t metaslab_class_get_alloc(metaslab_class_t *);
uint64_t metaslab_class_get_space(metaslab_class_t *);
uint64_t metaslab_class_get_dspace(metaslab_class_t *);
uint64_t metaslab_class_get_deferred(metaslab_class_t *);

void metaslab_space_update(vdev_t *, metaslab_class_t *,
    int64_t, int64_t, int64_t);

metaslab_group_t *metaslab_group_create(metaslab_class_t *, vdev_t *);
void metaslab_group_destroy(metaslab_group_t *);
void metaslab_group_activate(metaslab_group_t *);
void metaslab_group_passivate(metaslab_group_t *);
boolean_t metaslab_group_initialized(metaslab_group_t *);
uint64_t metaslab_group_get_space(metaslab_group_t *);
void metaslab_group_histogram_verify(metaslab_group_t *);
uint64_t metaslab_group_fragmentation(metaslab_group_t *);
void metaslab_group_histogram_remove(metaslab_group_t *, metaslab_t *);
void metaslab_group_alloc_increment_all(spa_t *, blkptr_t *, int, int,
    uint64_t, const void *);
void metaslab_group_alloc_decrement(spa_t *, uint64_t, int, int, uint64_t,
    const void *);
void metaslab_recalculate_weight_and_sort(metaslab_t *);
void metaslab_disable(metaslab_t *);
void metaslab_enable(metaslab_t *, boolean_t, boolean_t);
void metaslab_set_selected_txg(metaslab_t *, uint64_t);

extern int metaslab_debug_load;

zfs_range_seg_type_t metaslab_calculate_range_tree_type(vdev_t *vdev,
    metaslab_t *msp, uint64_t *start, uint64_t *shift);

#ifdef	__cplusplus
}
#endif

#endif	/* _SYS_METASLAB_H */
