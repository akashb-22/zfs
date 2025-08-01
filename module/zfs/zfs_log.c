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
 * Copyright (c) 2015, 2018 by Delphix. All rights reserved.
 * Copyright (c) 2022 by Pawel Jakub Dawidek
 */


#include <sys/types.h>
#include <sys/param.h>
#include <sys/sysmacros.h>
#include <sys/cmn_err.h>
#include <sys/kmem.h>
#include <sys/thread.h>
#include <sys/file.h>
#include <sys/vfs.h>
#include <sys/zfs_znode.h>
#include <sys/zfs_dir.h>
#include <sys/zil.h>
#include <sys/zil_impl.h>
#include <sys/byteorder.h>
#include <sys/policy.h>
#include <sys/stat.h>
#include <sys/acl.h>
#include <sys/dmu.h>
#include <sys/dbuf.h>
#include <sys/spa.h>
#include <sys/zfs_fuid.h>
#include <sys/dsl_dataset.h>

/*
 * These zfs_log_* functions must be called within a dmu tx, in one
 * of 2 contexts depending on zilog->z_replay:
 *
 * Non replay mode
 * ---------------
 * We need to record the transaction so that if it is committed to
 * the Intent Log then it can be replayed.  An intent log transaction
 * structure (itx_t) is allocated and all the information necessary to
 * possibly replay the transaction is saved in it. The itx is then assigned
 * a sequence number and inserted in the in-memory list anchored in the zilog.
 *
 * Replay mode
 * -----------
 * We need to mark the intent log record as replayed in the log header.
 * This is done in the same transaction as the replay so that they
 * commit atomically.
 */

int
zfs_log_create_txtype(zil_create_t type, vsecattr_t *vsecp, vattr_t *vap)
{
	int isxvattr = (vap->va_mask & ATTR_XVATTR);
	switch (type) {
	case Z_FILE:
		if (vsecp == NULL && !isxvattr)
			return (TX_CREATE);
		if (vsecp && isxvattr)
			return (TX_CREATE_ACL_ATTR);
		if (vsecp)
			return (TX_CREATE_ACL);
		else
			return (TX_CREATE_ATTR);
	case Z_DIR:
		if (vsecp == NULL && !isxvattr)
			return (TX_MKDIR);
		if (vsecp && isxvattr)
			return (TX_MKDIR_ACL_ATTR);
		if (vsecp)
			return (TX_MKDIR_ACL);
		else
			return (TX_MKDIR_ATTR);
	case Z_XATTRDIR:
		return (TX_MKXATTR);
	}
	ASSERT(0);
	return (TX_MAX_TYPE);
}

/*
 * build up the log data necessary for logging xvattr_t
 * First lr_attr_t is initialized.  following the lr_attr_t
 * is the mapsize and attribute bitmap copied from the xvattr_t.
 * Following the bitmap and bitmapsize two 64 bit words are reserved
 * for the create time which may be set.  Following the create time
 * records a single 64 bit integer which has the bits to set on
 * replay for the xvattr.
 */
static void
zfs_log_xvattr(lr_attr_t *lrattr, xvattr_t *xvap)
{
	xoptattr_t *xoap;

	xoap = xva_getxoptattr(xvap);
	ASSERT(xoap);

	lrattr->lr_attr_masksize = xvap->xva_mapsize;
	uint32_t *bitmap = &lrattr->lr_attr_bitmap;
	for (int i = 0; i != xvap->xva_mapsize; i++, bitmap++)
		*bitmap = xvap->xva_reqattrmap[i];

	lr_attr_end_t *end = (lr_attr_end_t *)bitmap;
	end->lr_attr_attrs = 0;
	end->lr_attr_crtime[0] = 0;
	end->lr_attr_crtime[1] = 0;
	memset(end->lr_attr_scanstamp, 0, AV_SCANSTAMP_SZ);

	if (XVA_ISSET_REQ(xvap, XAT_READONLY))
		end->lr_attr_attrs |= (xoap->xoa_readonly == 0) ? 0 :
		    XAT0_READONLY;
	if (XVA_ISSET_REQ(xvap, XAT_HIDDEN))
		end->lr_attr_attrs |= (xoap->xoa_hidden == 0) ? 0 :
		    XAT0_HIDDEN;
	if (XVA_ISSET_REQ(xvap, XAT_SYSTEM))
		end->lr_attr_attrs |= (xoap->xoa_system == 0) ? 0 :
		    XAT0_SYSTEM;
	if (XVA_ISSET_REQ(xvap, XAT_ARCHIVE))
		end->lr_attr_attrs |= (xoap->xoa_archive == 0) ? 0 :
		    XAT0_ARCHIVE;
	if (XVA_ISSET_REQ(xvap, XAT_IMMUTABLE))
		end->lr_attr_attrs |= (xoap->xoa_immutable == 0) ? 0 :
		    XAT0_IMMUTABLE;
	if (XVA_ISSET_REQ(xvap, XAT_NOUNLINK))
		end->lr_attr_attrs |= (xoap->xoa_nounlink == 0) ? 0 :
		    XAT0_NOUNLINK;
	if (XVA_ISSET_REQ(xvap, XAT_APPENDONLY))
		end->lr_attr_attrs |= (xoap->xoa_appendonly == 0) ? 0 :
		    XAT0_APPENDONLY;
	if (XVA_ISSET_REQ(xvap, XAT_OPAQUE))
		end->lr_attr_attrs |= (xoap->xoa_opaque == 0) ? 0 :
		    XAT0_APPENDONLY;
	if (XVA_ISSET_REQ(xvap, XAT_NODUMP))
		end->lr_attr_attrs |= (xoap->xoa_nodump == 0) ? 0 :
		    XAT0_NODUMP;
	if (XVA_ISSET_REQ(xvap, XAT_AV_QUARANTINED))
		end->lr_attr_attrs |= (xoap->xoa_av_quarantined == 0) ? 0 :
		    XAT0_AV_QUARANTINED;
	if (XVA_ISSET_REQ(xvap, XAT_AV_MODIFIED))
		end->lr_attr_attrs |= (xoap->xoa_av_modified == 0) ? 0 :
		    XAT0_AV_MODIFIED;
	if (XVA_ISSET_REQ(xvap, XAT_CREATETIME))
		ZFS_TIME_ENCODE(&xoap->xoa_createtime, end->lr_attr_crtime);
	if (XVA_ISSET_REQ(xvap, XAT_AV_SCANSTAMP)) {
		ASSERT(!XVA_ISSET_REQ(xvap, XAT_PROJID));

		memcpy(end->lr_attr_scanstamp, xoap->xoa_av_scanstamp,
		    AV_SCANSTAMP_SZ);
	} else if (XVA_ISSET_REQ(xvap, XAT_PROJID)) {
		/*
		 * XAT_PROJID and XAT_AV_SCANSTAMP will never be valid
		 * at the same time, so we can share the same space.
		 */
		memcpy(end->lr_attr_scanstamp, &xoap->xoa_projid,
		    sizeof (uint64_t));
	}
	if (XVA_ISSET_REQ(xvap, XAT_REPARSE))
		end->lr_attr_attrs |= (xoap->xoa_reparse == 0) ? 0 :
		    XAT0_REPARSE;
	if (XVA_ISSET_REQ(xvap, XAT_OFFLINE))
		end->lr_attr_attrs |= (xoap->xoa_offline == 0) ? 0 :
		    XAT0_OFFLINE;
	if (XVA_ISSET_REQ(xvap, XAT_SPARSE))
		end->lr_attr_attrs |= (xoap->xoa_sparse == 0) ? 0 :
		    XAT0_SPARSE;
	if (XVA_ISSET_REQ(xvap, XAT_PROJINHERIT))
		end->lr_attr_attrs |= (xoap->xoa_projinherit == 0) ? 0 :
		    XAT0_PROJINHERIT;
}

static void *
zfs_log_fuid_ids(zfs_fuid_info_t *fuidp, void *start)
{
	zfs_fuid_t *zfuid;
	uint64_t *fuidloc = start;

	/* First copy in the ACE FUIDs */
	for (zfuid = list_head(&fuidp->z_fuids); zfuid;
	    zfuid = list_next(&fuidp->z_fuids, zfuid)) {
		*fuidloc++ = zfuid->z_logfuid;
	}
	return (fuidloc);
}


static void *
zfs_log_fuid_domains(zfs_fuid_info_t *fuidp, void *start)
{
	zfs_fuid_domain_t *zdomain;

	/* now copy in the domain info, if any */
	if (fuidp->z_domain_str_sz != 0) {
		for (zdomain = list_head(&fuidp->z_domains); zdomain;
		    zdomain = list_next(&fuidp->z_domains, zdomain)) {
			memcpy(start, zdomain->z_domain,
			    strlen(zdomain->z_domain) + 1);
			start = (caddr_t)start +
			    strlen(zdomain->z_domain) + 1;
		}
	}
	return (start);
}

/*
 * If zp is an xattr node, check whether the xattr owner is unlinked.
 * We don't want to log anything if the owner is unlinked.
 */
static int
zfs_xattr_owner_unlinked(znode_t *zp)
{
	int unlinked = 0;
	znode_t *dzp;
#ifdef __FreeBSD__
	znode_t *tzp = zp;

	/*
	 * zrele drops the vnode lock which violates the VOP locking contract
	 * on FreeBSD. See comment at the top of zfs_replay.c for more detail.
	 */
	/*
	 * if zp is XATTR node, keep walking up via z_xattr_parent until we
	 * get the owner
	 */
	while (tzp->z_pflags & ZFS_XATTR) {
		ASSERT3U(zp->z_xattr_parent, !=, 0);
		if (zfs_zget(ZTOZSB(tzp), tzp->z_xattr_parent, &dzp) != 0) {
			unlinked = 1;
			break;
		}

		if (tzp != zp)
			zrele(tzp);
		tzp = dzp;
		unlinked = tzp->z_unlinked;
	}
	if (tzp != zp)
		zrele(tzp);
#else
	zhold(zp);
	/*
	 * if zp is XATTR node, keep walking up via z_xattr_parent until we
	 * get the owner
	 */
	while (zp->z_pflags & ZFS_XATTR) {
		ASSERT3U(zp->z_xattr_parent, !=, 0);
		if (zfs_zget(ZTOZSB(zp), zp->z_xattr_parent, &dzp) != 0) {
			unlinked = 1;
			break;
		}

		zrele(zp);
		zp = dzp;
		unlinked = zp->z_unlinked;
	}
	zrele(zp);
#endif
	return (unlinked);
}

/*
 * Handles TX_CREATE, TX_CREATE_ATTR, TX_MKDIR, TX_MKDIR_ATTR and
 * TK_MKXATTR transactions.
 *
 * TX_CREATE and TX_MKDIR are standard creates, but they may have FUID
 * domain information appended prior to the name.  In this case the
 * uid/gid in the log record will be a log centric FUID.
 *
 * TX_CREATE_ACL_ATTR and TX_MKDIR_ACL_ATTR handle special creates that
 * may contain attributes, ACL and optional fuid information.
 *
 * TX_CREATE_ACL and TX_MKDIR_ACL handle special creates that specify
 * and ACL and normal users/groups in the ACEs.
 *
 * There may be an optional xvattr attribute information similar
 * to zfs_log_setattr.
 *
 * Also, after the file name "domain" strings may be appended.
 */
void
zfs_log_create(zilog_t *zilog, dmu_tx_t *tx, uint64_t txtype,
    znode_t *dzp, znode_t *zp, const char *name, vsecattr_t *vsecp,
    zfs_fuid_info_t *fuidp, vattr_t *vap)
{
	itx_t *itx;
	_lr_create_t *lr;
	lr_acl_create_t *lracl = NULL;
	uint8_t *lrdata;
	size_t aclsize = 0;
	size_t xvatsize = 0;
	size_t txsize;
	xvattr_t *xvap = (xvattr_t *)vap;
	size_t namesize = strlen(name) + 1;
	size_t fuidsz = 0;

	if (zil_replaying(zilog, tx) || zfs_xattr_owner_unlinked(dzp))
		return;

	/*
	 * If we have FUIDs present then add in space for
	 * domains and ACE fuid's if any.
	 */
	if (fuidp) {
		fuidsz += fuidp->z_domain_str_sz;
		fuidsz += fuidp->z_fuid_cnt * sizeof (uint64_t);
	}

	if (vap->va_mask & ATTR_XVATTR)
		xvatsize = ZIL_XVAT_SIZE(xvap->xva_mapsize);

	if ((int)txtype == TX_CREATE_ATTR || (int)txtype == TX_MKDIR_ATTR ||
	    (int)txtype == TX_CREATE || (int)txtype == TX_MKDIR ||
	    (int)txtype == TX_MKXATTR) {
		txsize = sizeof (lr_create_t) + namesize + fuidsz + xvatsize;
		itx = zil_itx_create(txtype, txsize);
		lr_create_t *lrc = (lr_create_t *)&itx->itx_lr;
		lrdata = &lrc->lr_data[0];
	} else {
		txsize =
		    sizeof (lr_acl_create_t) + namesize + fuidsz +
		    ZIL_ACE_LENGTH(aclsize) + xvatsize;
		itx = zil_itx_create(txtype, txsize);
		lracl = (lr_acl_create_t *)&itx->itx_lr;
		lrdata = &lracl->lr_data[0];
	}


	lr = (_lr_create_t *)&itx->itx_lr;
	lr->lr_doid = dzp->z_id;
	lr->lr_foid = zp->z_id;
	/* Store dnode slot count in 8 bits above object id. */
	LR_FOID_SET_SLOTS(lr->lr_foid, zp->z_dnodesize >> DNODE_SHIFT);
	lr->lr_mode = zp->z_mode;
	if (!IS_EPHEMERAL(KUID_TO_SUID(ZTOUID(zp)))) {
		lr->lr_uid = (uint64_t)KUID_TO_SUID(ZTOUID(zp));
	} else {
		lr->lr_uid = fuidp->z_fuid_owner;
	}
	if (!IS_EPHEMERAL(KGID_TO_SGID(ZTOGID(zp)))) {
		lr->lr_gid = (uint64_t)KGID_TO_SGID(ZTOGID(zp));
	} else {
		lr->lr_gid = fuidp->z_fuid_group;
	}
	(void) sa_lookup(zp->z_sa_hdl, SA_ZPL_GEN(ZTOZSB(zp)), &lr->lr_gen,
	    sizeof (uint64_t));
	(void) sa_lookup(zp->z_sa_hdl, SA_ZPL_CRTIME(ZTOZSB(zp)),
	    lr->lr_crtime, sizeof (uint64_t) * 2);

	if (sa_lookup(zp->z_sa_hdl, SA_ZPL_RDEV(ZTOZSB(zp)), &lr->lr_rdev,
	    sizeof (lr->lr_rdev)) != 0)
		lr->lr_rdev = 0;

	/*
	 * Fill in xvattr info if any
	 */
	if (vap->va_mask & ATTR_XVATTR) {
		zfs_log_xvattr((lr_attr_t *)lrdata, xvap);
		lrdata = &lrdata[xvatsize];
	}

	/* Now fill in any ACL info */

	if (vsecp) {
		ASSERT3P(lracl, !=, NULL);
		lracl->lr_aclcnt = vsecp->vsa_aclcnt;
		lracl->lr_acl_bytes = aclsize;
		lracl->lr_domcnt = fuidp ? fuidp->z_domain_cnt : 0;
		lracl->lr_fuidcnt  = fuidp ? fuidp->z_fuid_cnt : 0;
		if (vsecp->vsa_aclflags & VSA_ACE_ACLFLAGS)
			lracl->lr_acl_flags = (uint64_t)vsecp->vsa_aclflags;
		else
			lracl->lr_acl_flags = 0;

		memcpy(lrdata, vsecp->vsa_aclentp, aclsize);
		lrdata = &lrdata[ZIL_ACE_LENGTH(aclsize)];
	}

	/* drop in FUID info */
	if (fuidp) {
		lrdata = zfs_log_fuid_ids(fuidp, lrdata);
		lrdata = zfs_log_fuid_domains(fuidp, lrdata);
	}
	/*
	 * Now place file name in log record
	 */
	memcpy(lrdata, name, namesize);

	zil_itx_assign(zilog, itx, tx);
}

/*
 * Handles both TX_REMOVE and TX_RMDIR transactions.
 */
void
zfs_log_remove(zilog_t *zilog, dmu_tx_t *tx, uint64_t txtype,
    znode_t *dzp, const char *name, uint64_t foid, boolean_t unlinked)
{
	itx_t *itx;
	lr_remove_t *lr;
	size_t namesize = strlen(name) + 1;

	if (zil_replaying(zilog, tx) || zfs_xattr_owner_unlinked(dzp))
		return;

	itx = zil_itx_create(txtype, sizeof (*lr) + namesize);
	lr = (lr_remove_t *)&itx->itx_lr;
	lr->lr_doid = dzp->z_id;
	memcpy(&lr->lr_data[0], name, namesize);

	itx->itx_oid = foid;

	/*
	 * Object ids can be re-instantiated in the next txg so
	 * remove any async transactions to avoid future leaks.
	 * This can happen if a fsync occurs on the re-instantiated
	 * object for a WR_INDIRECT or WR_NEED_COPY write, which gets
	 * the new file data and flushes a write record for the old object.
	 */
	if (unlinked) {
		ASSERT((txtype & ~TX_CI) == TX_REMOVE);
		zil_remove_async(zilog, foid);
	}
	zil_itx_assign(zilog, itx, tx);
}

/*
 * Handles TX_LINK transactions.
 */
void
zfs_log_link(zilog_t *zilog, dmu_tx_t *tx, uint64_t txtype,
    znode_t *dzp, znode_t *zp, const char *name)
{
	itx_t *itx;
	lr_link_t *lr;
	size_t namesize = strlen(name) + 1;

	if (zil_replaying(zilog, tx))
		return;

	itx = zil_itx_create(txtype, sizeof (*lr) + namesize);
	lr = (lr_link_t *)&itx->itx_lr;
	lr->lr_doid = dzp->z_id;
	lr->lr_link_obj = zp->z_id;
	memcpy(&lr->lr_data[0], name, namesize);

	zil_itx_assign(zilog, itx, tx);
}

/*
 * Handles TX_SYMLINK transactions.
 */
void
zfs_log_symlink(zilog_t *zilog, dmu_tx_t *tx, uint64_t txtype,
    znode_t *dzp, znode_t *zp, const char *name, const char *link)
{
	itx_t *itx;
	_lr_create_t *lr;
	lr_create_t *lrc;
	size_t namesize = strlen(name) + 1;
	size_t linksize = strlen(link) + 1;

	if (zil_replaying(zilog, tx))
		return;

	itx = zil_itx_create(txtype, sizeof (*lrc) + namesize + linksize);
	lrc = (lr_create_t *)&itx->itx_lr;
	lr = &lrc->lr_create;
	lr->lr_doid = dzp->z_id;
	lr->lr_foid = zp->z_id;
	lr->lr_uid = KUID_TO_SUID(ZTOUID(zp));
	lr->lr_gid = KGID_TO_SGID(ZTOGID(zp));
	lr->lr_mode = zp->z_mode;
	(void) sa_lookup(zp->z_sa_hdl, SA_ZPL_GEN(ZTOZSB(zp)), &lr->lr_gen,
	    sizeof (uint64_t));
	(void) sa_lookup(zp->z_sa_hdl, SA_ZPL_CRTIME(ZTOZSB(zp)),
	    lr->lr_crtime, sizeof (uint64_t) * 2);
	memcpy(&lrc->lr_data[0], name, namesize);
	memcpy(&lrc->lr_data[namesize], link, linksize);

	zil_itx_assign(zilog, itx, tx);
}

static void
do_zfs_log_rename(zilog_t *zilog, dmu_tx_t *tx, uint64_t txtype, znode_t *sdzp,
    const char *sname, znode_t *tdzp, const char *dname, znode_t *szp)
{
	itx_t *itx;
	_lr_rename_t *lr;
	lr_rename_t *lrr;
	size_t snamesize = strlen(sname) + 1;
	size_t dnamesize = strlen(dname) + 1;

	if (zil_replaying(zilog, tx))
		return;

	itx = zil_itx_create(txtype, sizeof (*lr) + snamesize + dnamesize);
	lrr = (lr_rename_t *)&itx->itx_lr;
	lr = &lrr->lr_rename;
	lr->lr_sdoid = sdzp->z_id;
	lr->lr_tdoid = tdzp->z_id;
	memcpy(&lrr->lr_data[0], sname, snamesize);
	memcpy(&lrr->lr_data[snamesize], dname, dnamesize);
	itx->itx_oid = szp->z_id;

	zil_itx_assign(zilog, itx, tx);
}

/*
 * Handles TX_RENAME transactions.
 */
void
zfs_log_rename(zilog_t *zilog, dmu_tx_t *tx, uint64_t txtype, znode_t *sdzp,
    const char *sname, znode_t *tdzp, const char *dname, znode_t *szp)
{
	txtype |= TX_RENAME;
	do_zfs_log_rename(zilog, tx, txtype, sdzp, sname, tdzp, dname, szp);
}

/*
 * Handles TX_RENAME_EXCHANGE transactions.
 */
void
zfs_log_rename_exchange(zilog_t *zilog, dmu_tx_t *tx, uint64_t txtype,
    znode_t *sdzp, const char *sname, znode_t *tdzp, const char *dname,
    znode_t *szp)
{
	txtype |= TX_RENAME_EXCHANGE;
	do_zfs_log_rename(zilog, tx, txtype, sdzp, sname, tdzp, dname, szp);
}

/*
 * Handles TX_RENAME_WHITEOUT transactions.
 *
 * Unfortunately we cannot reuse do_zfs_log_rename because we we need to call
 * zfs_mknode() on replay which requires stashing bits as with TX_CREATE.
 */
void
zfs_log_rename_whiteout(zilog_t *zilog, dmu_tx_t *tx, uint64_t txtype,
    znode_t *sdzp, const char *sname, znode_t *tdzp, const char *dname,
    znode_t *szp, znode_t *wzp)
{
	itx_t *itx;
	lr_rename_whiteout_t *lr;
	size_t snamesize = strlen(sname) + 1;
	size_t dnamesize = strlen(dname) + 1;

	if (zil_replaying(zilog, tx))
		return;

	txtype |= TX_RENAME_WHITEOUT;
	itx = zil_itx_create(txtype, sizeof (*lr) + snamesize + dnamesize);
	lr = (lr_rename_whiteout_t *)&itx->itx_lr;
	lr->lr_rename.lr_sdoid = sdzp->z_id;
	lr->lr_rename.lr_tdoid = tdzp->z_id;

	/*
	 * RENAME_WHITEOUT will create an entry at the source znode, so we need
	 * to store the same data that the equivalent call to zfs_log_create()
	 * would.
	 */
	lr->lr_wfoid = wzp->z_id;
	LR_FOID_SET_SLOTS(lr->lr_wfoid, wzp->z_dnodesize >> DNODE_SHIFT);
	(void) sa_lookup(wzp->z_sa_hdl, SA_ZPL_GEN(ZTOZSB(wzp)), &lr->lr_wgen,
	    sizeof (uint64_t));
	(void) sa_lookup(wzp->z_sa_hdl, SA_ZPL_CRTIME(ZTOZSB(wzp)),
	    lr->lr_wcrtime, sizeof (uint64_t) * 2);
	lr->lr_wmode = wzp->z_mode;
	lr->lr_wuid = (uint64_t)KUID_TO_SUID(ZTOUID(wzp));
	lr->lr_wgid = (uint64_t)KGID_TO_SGID(ZTOGID(wzp));

	/*
	 * This rdev will always be makdevice(0, 0) but because the ZIL log and
	 * replay code needs to be platform independent (and there is no
	 * platform independent makdev()) we need to copy the one created
	 * during the rename operation.
	 */
	(void) sa_lookup(wzp->z_sa_hdl, SA_ZPL_RDEV(ZTOZSB(wzp)), &lr->lr_wrdev,
	    sizeof (lr->lr_wrdev));

	memcpy(&lr->lr_data[0], sname, snamesize);
	memcpy(&lr->lr_data[snamesize], dname, dnamesize);
	itx->itx_oid = szp->z_id;

	zil_itx_assign(zilog, itx, tx);
}

/*
 * zfs_log_write() handles TX_WRITE transactions. The specified callback is
 * called as soon as the write is on stable storage (be it via a DMU sync or a
 * ZIL commit).
 */
void
zfs_log_write(zilog_t *zilog, dmu_tx_t *tx, int txtype,
    znode_t *zp, offset_t off, ssize_t resid, boolean_t commit,
    boolean_t o_direct, zil_callback_t callback, void *callback_data)
{
	dmu_buf_impl_t *db = (dmu_buf_impl_t *)sa_get_db(zp->z_sa_hdl);
	uint32_t blocksize = zp->z_blksz;
	itx_wr_state_t write_state;
	uint64_t gen = 0, log_size = 0;

	if (zil_replaying(zilog, tx) || zp->z_unlinked ||
	    zfs_xattr_owner_unlinked(zp)) {
		if (callback != NULL)
			callback(callback_data);
		return;
	}

	write_state = zil_write_state(zilog, resid, blocksize, o_direct,
	    commit);

	(void) sa_lookup(zp->z_sa_hdl, SA_ZPL_GEN(ZTOZSB(zp)), &gen,
	    sizeof (gen));

	while (resid) {
		itx_t *itx;
		lr_write_t *lr;
		itx_wr_state_t wr_state = write_state;
		ssize_t len = resid;

		/*
		 * A WR_COPIED record must fit entirely in one log block.
		 * Large writes can use WR_NEED_COPY, which the ZIL will
		 * split into multiple records across several log blocks
		 * if necessary.
		 */
		if (wr_state == WR_COPIED &&
		    resid > zil_max_copied_data(zilog))
			wr_state = WR_NEED_COPY;
		else if (wr_state == WR_INDIRECT)
			len = MIN(blocksize - P2PHASE(off, blocksize), resid);

		itx = zil_itx_create(txtype, sizeof (*lr) +
		    (wr_state == WR_COPIED ? len : 0));
		lr = (lr_write_t *)&itx->itx_lr;

		/*
		 * For WR_COPIED records, copy the data into the lr_write_t.
		 */
		if (wr_state == WR_COPIED) {
			int err;
			DB_DNODE_ENTER(db);
			err = dmu_read_by_dnode(DB_DNODE(db), off, len,
			    &lr->lr_data[0], DMU_READ_NO_PREFETCH |
			    DMU_KEEP_CACHING);
			DB_DNODE_EXIT(db);
			if (err != 0) {
				zil_itx_destroy(itx);
				itx = zil_itx_create(txtype, sizeof (*lr));
				lr = (lr_write_t *)&itx->itx_lr;
				wr_state = WR_NEED_COPY;
			}
		}

		log_size += itx->itx_size;
		if (wr_state == WR_NEED_COPY)
			log_size += len;

		itx->itx_wr_state = wr_state;
		lr->lr_foid = zp->z_id;
		lr->lr_offset = off;
		lr->lr_length = len;
		lr->lr_blkoff = 0;
		BP_ZERO(&lr->lr_blkptr);

		itx->itx_private = ZTOZSB(zp);
		itx->itx_sync = (zp->z_sync_cnt != 0);
		itx->itx_gen = gen;

		if (resid == len) {
			itx->itx_callback = callback;
			itx->itx_callback_data = callback_data;
		}

		zil_itx_assign(zilog, itx, tx);

		off += len;
		resid -= len;
	}

	dsl_pool_wrlog_count(zilog->zl_dmu_pool, log_size, tx->tx_txg);
}

/*
 * Handles TX_TRUNCATE transactions.
 */
void
zfs_log_truncate(zilog_t *zilog, dmu_tx_t *tx, int txtype,
    znode_t *zp, uint64_t off, uint64_t len)
{
	itx_t *itx;
	lr_truncate_t *lr;

	if (zil_replaying(zilog, tx) || zp->z_unlinked ||
	    zfs_xattr_owner_unlinked(zp))
		return;

	itx = zil_itx_create(txtype, sizeof (*lr));
	lr = (lr_truncate_t *)&itx->itx_lr;
	lr->lr_foid = zp->z_id;
	lr->lr_offset = off;
	lr->lr_length = len;

	itx->itx_sync = (zp->z_sync_cnt != 0);
	zil_itx_assign(zilog, itx, tx);
}

/*
 * Handles TX_SETATTR transactions.
 */
void
zfs_log_setattr(zilog_t *zilog, dmu_tx_t *tx, int txtype,
    znode_t *zp, vattr_t *vap, uint_t mask_applied, zfs_fuid_info_t *fuidp)
{
	itx_t		*itx;
	lr_setattr_t	*lr;
	xvattr_t	*xvap = (xvattr_t *)vap;
	size_t		recsize = sizeof (lr_setattr_t);
	uint8_t		*start;

	if (zil_replaying(zilog, tx) || zp->z_unlinked)
		return;

	/*
	 * If XVATTR set, then log record size needs to allow
	 * for lr_attr_t + xvattr mask, mapsize and create time
	 * plus actual attribute values
	 */
	if (vap->va_mask & ATTR_XVATTR)
		recsize = sizeof (*lr) + ZIL_XVAT_SIZE(xvap->xva_mapsize);

	if (fuidp)
		recsize += fuidp->z_domain_str_sz;

	itx = zil_itx_create(txtype, recsize);
	lr = (lr_setattr_t *)&itx->itx_lr;
	lr->lr_foid = zp->z_id;
	lr->lr_mask = (uint64_t)mask_applied;
	lr->lr_mode = (uint64_t)vap->va_mode;
	if ((mask_applied & ATTR_UID) && IS_EPHEMERAL(vap->va_uid))
		lr->lr_uid = fuidp->z_fuid_owner;
	else
		lr->lr_uid = (uint64_t)vap->va_uid;

	if ((mask_applied & ATTR_GID) && IS_EPHEMERAL(vap->va_gid))
		lr->lr_gid = fuidp->z_fuid_group;
	else
		lr->lr_gid = (uint64_t)vap->va_gid;

	lr->lr_size = (uint64_t)vap->va_size;
	ZFS_TIME_ENCODE(&vap->va_atime, lr->lr_atime);
	ZFS_TIME_ENCODE(&vap->va_mtime, lr->lr_mtime);
	start = &lr->lr_data[0];
	if (vap->va_mask & ATTR_XVATTR) {
		zfs_log_xvattr((lr_attr_t *)start, xvap);
		start = &lr->lr_data[ZIL_XVAT_SIZE(xvap->xva_mapsize)];
	}

	/*
	 * Now stick on domain information if any on end
	 */

	if (fuidp)
		(void) zfs_log_fuid_domains(fuidp, start);

	itx->itx_sync = (zp->z_sync_cnt != 0);
	zil_itx_assign(zilog, itx, tx);
}

/*
 * Handles TX_SETSAXATTR transactions.
 */
void
zfs_log_setsaxattr(zilog_t *zilog, dmu_tx_t *tx, int txtype,
    znode_t *zp, const char *name, const void *value, size_t size)
{
	itx_t		*itx;
	lr_setsaxattr_t	*lr;
	size_t		recsize = sizeof (lr_setsaxattr_t);
	int		namelen;

	if (zil_replaying(zilog, tx) || zp->z_unlinked)
		return;

	namelen = strlen(name) + 1;
	recsize += (namelen + size);
	itx = zil_itx_create(txtype, recsize);
	lr = (lr_setsaxattr_t *)&itx->itx_lr;
	lr->lr_foid = zp->z_id;
	memcpy(&lr->lr_data[0], name, namelen);
	if (value != NULL) {
		memcpy(&lr->lr_data[namelen], value, size);
		lr->lr_size = size;
	} else {
		lr->lr_size = 0;
	}

	itx->itx_sync = (zp->z_sync_cnt != 0);
	zil_itx_assign(zilog, itx, tx);
}

/*
 * Handles TX_ACL transactions.
 */
void
zfs_log_acl(zilog_t *zilog, dmu_tx_t *tx, znode_t *zp,
    vsecattr_t *vsecp, zfs_fuid_info_t *fuidp)
{
	itx_t *itx;
	lr_acl_v0_t *lrv0;
	lr_acl_t *lr;
	int txtype;
	int lrsize;
	size_t txsize;
	size_t aclbytes = vsecp->vsa_aclentsz;

	if (zil_replaying(zilog, tx) || zp->z_unlinked)
		return;

	txtype = (ZTOZSB(zp)->z_version < ZPL_VERSION_FUID) ?
	    TX_ACL_V0 : TX_ACL;

	if (txtype == TX_ACL)
		lrsize = sizeof (*lr);
	else
		lrsize = sizeof (*lrv0);

	txsize = lrsize +
	    ((txtype == TX_ACL) ? ZIL_ACE_LENGTH(aclbytes) : aclbytes) +
	    (fuidp ? fuidp->z_domain_str_sz : 0) +
	    sizeof (uint64_t) * (fuidp ? fuidp->z_fuid_cnt : 0);

	itx = zil_itx_create(txtype, txsize);

	lr = (lr_acl_t *)&itx->itx_lr;
	lr->lr_foid = zp->z_id;
	if (txtype == TX_ACL) {
		lr->lr_acl_bytes = aclbytes;
		lr->lr_domcnt = fuidp ? fuidp->z_domain_cnt : 0;
		lr->lr_fuidcnt = fuidp ? fuidp->z_fuid_cnt : 0;
		if (vsecp->vsa_mask & VSA_ACE_ACLFLAGS)
			lr->lr_acl_flags = (uint64_t)vsecp->vsa_aclflags;
		else
			lr->lr_acl_flags = 0;
	}
	lr->lr_aclcnt = (uint64_t)vsecp->vsa_aclcnt;

	if (txtype == TX_ACL_V0) {
		lrv0 = (lr_acl_v0_t *)lr;
		memcpy(&lrv0->lr_data[0], vsecp->vsa_aclentp, aclbytes);
	} else {
		uint8_t *start = &lr->lr_data[0];

		memcpy(start, vsecp->vsa_aclentp, aclbytes);

		start = &lr->lr_data[ZIL_ACE_LENGTH(aclbytes)];

		if (fuidp) {
			start = zfs_log_fuid_ids(fuidp, start);
			(void) zfs_log_fuid_domains(fuidp, start);
		}
	}

	itx->itx_sync = (zp->z_sync_cnt != 0);
	zil_itx_assign(zilog, itx, tx);
}

/*
 * Handles TX_CLONE_RANGE transactions.
 */
void
zfs_log_clone_range(zilog_t *zilog, dmu_tx_t *tx, int txtype, znode_t *zp,
    uint64_t off, uint64_t len, uint64_t blksz, const blkptr_t *bps,
    size_t nbps)
{
	itx_t *itx;
	lr_clone_range_t *lr;
	uint64_t partlen, max_log_data;
	size_t partnbps;

	if (zil_replaying(zilog, tx) || zp->z_unlinked)
		return;

	max_log_data = zil_max_log_data(zilog, sizeof (lr_clone_range_t));

	while (nbps > 0) {
		partnbps = MIN(nbps, max_log_data / sizeof (bps[0]));
		partlen = partnbps * blksz;
		ASSERT3U(partlen, <, len + blksz);
		partlen = MIN(partlen, len);

		itx = zil_itx_create(txtype,
		    sizeof (*lr) + sizeof (bps[0]) * partnbps);
		lr = (lr_clone_range_t *)&itx->itx_lr;
		lr->lr_foid = zp->z_id;
		lr->lr_offset = off;
		lr->lr_length = partlen;
		lr->lr_blksz = blksz;
		lr->lr_nbps = partnbps;
		memcpy(lr->lr_bps, bps, sizeof (bps[0]) * partnbps);

		itx->itx_sync = (zp->z_sync_cnt != 0);

		zil_itx_assign(zilog, itx, tx);

		bps += partnbps;
		ASSERT3U(nbps, >=, partnbps);
		nbps -= partnbps;
		off += partlen;
		ASSERT3U(len, >=, partlen);
		len -= partlen;
	}
}
