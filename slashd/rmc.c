/* $Id$ */
/*
 * %PSC_START_COPYRIGHT%
 * -----------------------------------------------------------------------------
 * Copyright (c) 2006-2010, Pittsburgh Supercomputing Center (PSC).
 *
 * Permission to use, copy, and modify this software and its documentation
 * without fee for personal use or non-commercial use within your organization
 * is hereby granted, provided that the above copyright notice is preserved in
 * all copies and that the copyright and this permission notice appear in
 * supporting documentation.  Permission to redistribute this software to other
 * organizations or individuals is not permitted without the written permission
 * of the Pittsburgh Supercomputing Center.  PSC makes no representations about
 * the suitability of this software for any purpose.  It is provided "as is"
 * without express or implied warranty.
 * -----------------------------------------------------------------------------
 * %PSC_END_COPYRIGHT%
 */

/*
 * Routines for handling RPC requests for MDS from CLIENT.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/fsuid.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <unistd.h>

#include "psc_rpc/rpc.h"
#include "psc_rpc/rsx.h"
#include "psc_rpc/rpclog.h"
#include "psc_rpc/service.h"
#include "psc_util/lock.h"
#include "psc_util/strlcpy.h"

#include "cfd.h"
#include "fdbuf.h"
#include "fidc_mds.h"
#include "fidcache.h"
#include "mdsexpc.h"
#include "mdsio.h"
#include "pathnames.h"
#include "repl_mds.h"
#include "rpc_mds.h"
#include "slashd.h"
#include "slashrpc.h"
#include "slerr.h"
#include "slutil.h"

/*
 * The following SLASHIDs are automatically assigned:
 *	0	not used
 *	1	-> /
 */
#define SLASHID_MIN	2

/*
 * TODO: SLASH ID should be logged on disk, so that it can be advanced
 *	continuously across reboots and crashes.
 */
uint64_t next_slash_id = SLASHID_MIN;

uint64_t
slm_get_next_slashid(void)
{
	static psc_spinlock_t lock = LOCK_INITIALIZER;
	uint64_t slid;

	spinlock(&lock);
	if (next_slash_id >= (UINT64_C(1) << SLASH_ID_FID_BITS))
		next_slash_id = SLASHID_MIN;
	slid = next_slash_id++;
	freelock(&lock);
	return (slid | ((uint64_t)nodeResm->resm_site->site_id <<
	    SLASH_ID_FID_BITS));
}

int
slm_rmc_handle_connect(struct pscrpc_request *rq)
{
	struct pscrpc_export *e=rq->rq_export;
	struct srm_connect_req *mq;
	struct srm_generic_rep *mp;
	struct mexp_cli *mexp_cli;

	RSX_ALLOCREP(rq, mq, mp);
	if (mq->magic != SRMC_MAGIC || mq->version != SRMC_VERSION)
		mp->rc = -EINVAL;

	psc_assert(e->exp_private == NULL);
	mexp_cli = mexpcli_get(e);
	slm_getclcsvc(e);
	return (0);
}

int
slm_rmc_handle_getattr(struct pscrpc_request *rq)
{
	struct srm_getattr_req *mq;
	struct srm_getattr_rep *mp;
	struct fidc_membh *fcmh;

	RSX_ALLOCREP(rq, mq, mp);
	mp->rc = slm_fcmh_get(&mq->fg, &mq->creds, &fcmh);
	if (mp->rc)
		goto out;
	mp->rc = checkcreds(&fcmh->fcmh_sstb, &mq->creds, R_OK);
	if (mp->rc)
		goto out;
	mp->attr = fcmh->fcmh_sstb;
 out:
	slm_fcmh_release(fcmh);
	return (0);
}

int
slm_rmc_handle_getbmap(struct pscrpc_request *rq)
{
	struct slash_bmap_cli_wire *cw;
	struct pscrpc_bulk_desc *desc;
	struct bmap_mds_info *bmdsi;
	struct srt_bmapdesc_buf bdb;
	const struct srm_bmap_req *mq;
	struct srm_bmap_rep *mp;
	struct bmapc_memb *bmap;
	struct iovec iov[5];
	struct mexpfcm *m;
	uint64_t cfd;
	int niov;

	bmap = NULL;
	RSX_ALLOCREP(rq, mq, mp);

	mp->rc = fdbuf_check(&mq->sfdb, &cfd, NULL, &rq->rq_peer);
	if (mp->rc)
		return (mp->rc);

	if (mq->rw != SL_READ && mq->rw != SL_WRITE)
		return (-EINVAL);

	/*
	 * Check the cfdent structure associated with the client.  If successful,
	 * it also returns the mexpfcm in the out argument.
	 */
	mp->rc = cfdlookup(rq->rq_export, cfd, &m);
	if (mp->rc)
		return (mp->rc);

	bmap = NULL;
	mp->rc = mds_bmap_load_cli(m, mq, &bmap);
	if (mp->rc)
		return (mp->rc);

	bmdsi = bmap->bcm_pri;
	cw = (struct slash_bmap_cli_wire *)bmap->bcm_od->bh_crcstates;

	niov = 2;
	iov[0].iov_base = cw;
	iov[0].iov_len = sizeof(*cw);
	iov[1].iov_base = &bdb;
	iov[1].iov_len = sizeof(bdb);

	if (mq->getreptbl) {
		struct slash_inode_handle *ih;

		niov++;
		ih = fcmh_2_inoh(bmap->bcm_fcmh);
		mp->nrepls = ih->inoh_ino.ino_nrepls;

		iov[2].iov_base = ih->inoh_ino.ino_repls;
		iov[2].iov_len = sizeof(sl_replica_t) * INO_DEF_NREPLS;

		if (mp->nrepls > INO_DEF_NREPLS) {
			niov++;
			mds_inox_ensure_loaded(ih);
			iov[3].iov_base = ih->inoh_extras->inox_repls;
			iov[3].iov_len = sizeof(ih->inoh_extras->inox_repls);
		}
	}

	if (bmap->bcm_mode & BMAP_WR) {
		/* Return the write IOS if the bmap is in write mode.
		 */
		psc_assert(bmdsi->bmdsi_wr_ion);
		mp->ios_nid = bmdsi->bmdsi_wr_ion->rmmi_resm->resm_nid;
	} else
		mp->ios_nid = LNET_NID_ANY;

	bdbuf_sign(&bdb, &mq->sfdb.sfdb_secret.sfs_fg, &rq->rq_peer,
	   (mq->rw == SL_WRITE ? mp->ios_nid : LNET_NID_ANY),
	   (mq->rw == SL_WRITE ?
	    bmdsi->bmdsi_wr_ion->rmmi_resm->resm_res->res_id : IOS_ID_ANY),
	   bmap->bcm_blkno);

	mp->rc = rsx_bulkserver(rq, &desc, BULK_PUT_SOURCE,
	    SRMC_BULK_PORTAL, iov, niov);
	if (desc)
		pscrpc_free_bulk(desc);
	mp->nblks = 1;

	return (0);
}

int
slm_rmc_handle_link(struct pscrpc_request *rq)
{
	struct fidc_membh *p, *c;
	struct srm_link_req *mq;
	struct srm_link_rep *mp;

	p = c = NULL;
	RSX_ALLOCREP(rq, mq, mp);
	mp->rc = slm_fcmh_get(&mq->fg, &mq->creds, &c);
	if (mp->rc)
		goto out;
	mp->rc = slm_fcmh_get(&mq->pfg, &mq->creds, &p);
	if (mp->rc)
		goto out;
	mq->name[sizeof(mq->name) - 1] = '\0';
	mp->rc = mdsio_link(fcmh_2_mdsio_fid(c), fcmh_2_mdsio_fid(p),
	    mq->name, &mp->fg, &mq->creds, &mp->attr);
 out:
	slm_fcmh_release(c);
	slm_fcmh_release(p);
	return (0);
}

int
slm_rmc_handle_lookup(struct pscrpc_request *rq)
{
	struct srm_lookup_req *mq;
	struct srm_lookup_rep *mp;
	struct fidc_membh *fcmh;

	RSX_ALLOCREP(rq, mq, mp);
	mp->rc = slm_fcmh_get(&mq->pfg, &mq->creds, &fcmh);
	if (mp->rc)
		goto out;
	mp->rc = checkcreds(&fcmh->fcmh_sstb, &mq->creds, X_OK);
	if (mp->rc)
		goto out;

	mq->name[sizeof(mq->name) - 1] = '\0';
	if (fcmh_2_mdsio_fid(fcmh) == SL_ROOT_INUM &&
	    strncmp(mq->name, SL_PATH_PREFIX,
	     strlen(SL_PATH_PREFIX)) == 0) {
		mp->rc = EINVAL;
		goto out;
	}
	mp->rc = mdsio_lookup(fcmh_2_mdsio_fid(fcmh),
	    mq->name, &mp->fg, NULL, &mq->creds, &mp->attr);
 out:
	slm_fcmh_release(fcmh);
	return (0);
}

int
slm_rmc_handle_mkdir(struct pscrpc_request *rq)
{
	struct srm_mkdir_req *mq;
	struct srm_mkdir_rep *mp;
	struct fidc_membh *fcmh;

	RSX_ALLOCREP(rq, mq, mp);
	mp->rc = slm_fcmh_get(&mq->pfg, &mq->creds, &fcmh);
	if (mp->rc)
		goto out;

#ifdef NAMESPACE_EXPERIMENTAL
	mp->fg.fg_fid = slm_get_next_slashid();
#endif

	mq->name[sizeof(mq->name) - 1] = '\0';
	mp->rc = mdsio_mkdir(fcmh_2_mdsio_fid(fcmh), mq->name,
	    mq->mode, &mq->creds, &mp->attr, &mp->fg, NULL);
 out:
	slm_fcmh_release(fcmh);
	return (0);
}

int
slm_rmc_translate_flags(int in, int *out)
{
	*out = 0;

	if (in & ~(SLF_READ | SLF_WRITE | SLF_APPEND | SLF_CREAT |
	    SLF_TRUNC | SLF_SYNC | SLF_EXCL))
		return (EINVAL);

	if ((in & (SLF_READ | SLF_WRITE)) == 0)
		return (EINVAL);

	if (in & SLF_WRITE) {
		if (in & SLF_READ)
			*out = O_RDWR;
		else
			*out = O_WRONLY;
	}

	if (in & SLF_CREAT)
		*out |= O_CREAT;

	if (in & SLF_EXCL)
		*out |= O_EXCL;

	*out |= O_LARGEFILE;

	return (0);
}

int
slm_rmc_handle_create(struct pscrpc_request *rq)
{
	struct srm_opencreate_rep *mp;
	struct srm_create_req *mq;
	struct fidc_membh *p, *c;
	struct slash_fidgen fg;
	struct cfdent *cfd;
	void *mdsio_data;
	int fl;

	p = c = NULL;

	RSX_ALLOCREP(rq, mq, mp);
	mq->name[sizeof(mq->name) - 1] = '\0';
	mp->rc = slm_rmc_translate_flags(mq->flags, &fl);
	if (mp->rc)
		goto out;

	mp->rc = slm_fcmh_get(&mq->pfg, &mq->creds, &p);
	if (mp->rc)
		goto out;

	mp->rc = checkcreds(&p->fcmh_sstb, &mq->creds, W_OK);
	if (mp->rc)
		goto out;

#ifdef NAMESPACE_EXPERIMENTAL
	fg.fg_fid = slm_get_next_slashid();
#endif

	mp->rc = mdsio_opencreate(fcmh_2_mdsio_fid(p), &mq->creds,
	    fl | O_EXCL | O_CREAT, mq->mode, mq->name, &fg, NULL,
	    &mp->attr, &mdsio_data);
	if (mp->rc)
		goto out;
	mdsio_frelease(&mq->creds, mdsio_data);

	mp->rc = cfdnew(fg.fg_fid, rq->rq_export,
	    SLCONNT_CLI, &cfd, CFD_FILE);
	if (mp->rc == 0) {
		fdbuf_sign(&cfd->cfd_fdb, &fg, &rq->rq_peer);
		memcpy(&mp->sfdb, &cfd->cfd_fdb,
		    sizeof(mp->sfdb));
	}

 out:
	slm_fcmh_release(c);
	slm_fcmh_release(p);
	return (0);
}

int
slm_rmc_handle_open(struct pscrpc_request *rq)
{
	struct srm_opencreate_rep *mp;
	struct srm_open_req *mq;
	struct fidc_membh *fcmh;
	struct cfdent *cfd;
	int fl, accmode;

	RSX_ALLOCREP(rq, mq, mp);
	mp->rc = slm_rmc_translate_flags(mq->flags, &fl);
	if (mp->rc)
		return (0);

	mp->rc = slm_fcmh_get(&mq->fg, &mq->creds, &fcmh);
	if (mp->rc)
		goto out;

	accmode = 0;
	if (fl & O_RDWR)
		accmode = R_OK | W_OK;
	else if (fl & O_WRONLY)
		accmode = W_OK;
	else
		accmode = R_OK;
	mp->rc = checkcreds(&fcmh->fcmh_sstb, &mq->creds, accmode);
	if (mp->rc)
		goto out;

	mp->rc = cfdnew(mq->fg.fg_fid, rq->rq_export,
	    SLCONNT_CLI, &cfd, CFD_FILE);
	if (mp->rc == 0) {
		fdbuf_sign(&cfd->cfd_fdb, &mq->fg, &rq->rq_peer);
		memcpy(&mp->sfdb, &cfd->cfd_fdb, sizeof(mp->sfdb));
		mp->attr = fcmh->fcmh_sstb;
	}

 out:
	slm_fcmh_release(fcmh);
	return (0);
}

int
slm_rmc_handle_opendir(struct pscrpc_request *rq)
{
	struct srm_opendir_req *mq;
	struct srm_opendir_rep *mp;
	struct fidc_membh *d;
	struct cfdent *cfd;

	RSX_ALLOCREP(rq, mq, mp);
	mp->rc = slm_fcmh_get(&mq->fg, &mq->creds, &d);
	if (mp->rc)
		goto out;

	mp->rc = checkcreds(&d->fcmh_sstb, &mq->creds, X_OK);
	if (mp->rc)
		goto out;

	mp->rc = cfdnew(mq->fg.fg_fid, rq->rq_export,
	    SLCONNT_CLI, &cfd, CFD_DIR);
	if (mp->rc == 0) {
		fdbuf_sign(&cfd->cfd_fdb, &mq->fg, &rq->rq_peer);
		memcpy(&mp->sfdb, &cfd->cfd_fdb, sizeof(mp->sfdb));
		mp->attr = d->fcmh_sstb;
	}

 out:
	slm_fcmh_release(d);
	return (0);
}

int
slm_rmc_handle_readdir(struct pscrpc_request *rq)
{
	struct pscrpc_bulk_desc *desc;
	struct srm_readdir_req *mq;
	struct srm_readdir_rep *mp;
	struct slash_fidgen fg;
	struct iovec iov[2];
	struct mexpfcm *m;
	size_t outsize;
	uint64_t cfd;
	int niov;

	iov[0].iov_base = NULL;
	iov[1].iov_base = NULL;

	RSX_ALLOCREP(rq, mq, mp);

	mp->rc = fdbuf_check(&mq->sfdb, &cfd, &fg, &rq->rq_peer);
	if (mp->rc)
		goto out;

	mp->rc = cfdlookup(rq->rq_export, cfd, &m);
	if (mp->rc)
		goto out;

#define MAX_READDIR_NENTS	1000
#define MAX_READDIR_BUFSIZ	(sizeof(struct srt_stat) * MAX_READDIR_NENTS)
	if (mq->size > MAX_READDIR_BUFSIZ ||
	    mq->nstbpref > MAX_READDIR_NENTS) {
		mp->rc = EINVAL;
		goto out;
	}

	mp->rc = mds_fcmh_load_fmi(m->mexpfcm_fcmh);
	if (mp->rc)
		goto out;

	iov[0].iov_base = PSCALLOC(mq->size);
	iov[0].iov_len = mq->size;

	niov = 1;
	if (mq->nstbpref) {
		niov++;
		iov[1].iov_len = mq->nstbpref * sizeof(struct srm_getattr_rep);
		iov[1].iov_base = PSCALLOC(iov[1].iov_len);
	} else {
		iov[1].iov_len = 0;
		iov[1].iov_base = NULL;
	}

	mp->rc = mdsio_readdir(&mq->creds, mq->size, mq->offset,
	    iov[0].iov_base, &outsize, iov[1].iov_base, mq->nstbpref,
	    fcmh_2_mdsio_data(m->mexpfcm_fcmh));
	mp->size = outsize;

	psc_info("mdsio_readdir rc=%d data=%p", mp->rc,
	    fcmh_2_mdsio_data(m->mexpfcm_fcmh));

	if (mp->rc)
		goto out;

	mp->rc = rsx_bulkserver(rq, &desc,
	    BULK_PUT_SOURCE, SRMC_BULK_PORTAL, iov, niov);

	if (desc)
		pscrpc_free_bulk(desc);

 out:
	PSCFREE(iov[0].iov_base);
	if (mq->nstbpref)
		PSCFREE(iov[1].iov_base);
	return (mp->rc);
}

int
slm_rmc_handle_readlink(struct pscrpc_request *rq)
{
	struct pscrpc_bulk_desc *desc;
	struct srm_readlink_req *mq;
	struct srm_readlink_rep *mp;
	struct fidc_membh *fcmh;
	struct iovec iov;
	char buf[PATH_MAX];

	RSX_ALLOCREP(rq, mq, mp);
	mp->rc = slm_fcmh_get(&mq->fg, &mq->creds, &fcmh);
	if (mp->rc)
		goto out;

	mp->rc = mdsio_readlink(fcmh_2_mdsio_fid(fcmh), buf, &mq->creds);
	if (mp->rc)
		goto out;

	iov.iov_base = buf;
	iov.iov_len = sizeof(buf);
	mp->rc = rsx_bulkserver(rq, &desc, BULK_PUT_SOURCE,
	    SRMC_BULK_PORTAL, &iov, 1);
	if (desc)
		pscrpc_free_bulk(desc);

 out:
	slm_fcmh_release(fcmh);
	return (mp->rc);
}

int
slm_rmc_handle_release(struct pscrpc_request *rq)
{
	struct srm_release_req *mq;
	struct srm_generic_rep *mp;
	struct fcoo_mds_info *fmi;
	struct slash_fidgen fg;
	struct fidc_membh *f;
	struct mexpfcm *m;
	struct cfdent *c;
	uint64_t cfd;
	int rc;

	RSX_ALLOCREP(rq, mq, mp);

	mp->rc = fdbuf_check(&mq->sfdb, &cfd, &fg, &rq->rq_peer);
	if (mp->rc)
		return (0);

	c = cfdget(rq->rq_export, cfd);
	if (!c) {
		psc_info("cfdget() failed cfd %"PRId64, cfd);
		mp->rc = ENOENT;
		return (0);
	}
	psc_assert(c->cfd_pri);
	m = c->cfd_pri;

	f = m->mexpfcm_fcmh;
	psc_assert(f->fcmh_fcoo);

	fmi = fcmh_2_fmi(f);

	MEXPFCM_LOCK(m);
	psc_assert(m->mexpfcm_fcmh);
	/* Prevent others from trying to access the mexpfcm.
	 */
	m->mexpfcm_flags |= MEXPFCM_CLOSING;
	MEXPFCM_ULOCK(m);

	rc = cfdfree(rq->rq_export, cfd);
	psc_info("cfdfree() cfd %"PRId64" rc=%d", cfd, rc);

	mp->rc = mds_inode_release(f);

	return (0);
}

int
slm_rmc_handle_rename(struct pscrpc_request *rq)
{
	char from[NAME_MAX+1], to[NAME_MAX+1];
	struct pscrpc_bulk_desc *desc;
	struct fidc_membh *op, *np;
	struct srm_generic_rep *mp;
	struct srm_rename_req *mq;
	struct iovec iov[2];

	op = np = NULL;
	RSX_ALLOCREP(rq, mq, mp);
	if (mq->fromlen <= 1 ||
	    mq->tolen <= 1) {
		mp->rc = -ENOENT;
		return (0);
	}
	if (mq->fromlen > NAME_MAX ||
	    mq->tolen > NAME_MAX) {
		mp->rc = -EINVAL;
		return (0);
	}

	mp->rc = slm_fcmh_get(&mq->opfg, &mq->creds, &op);
	if (mp->rc)
		goto out;

	mp->rc = slm_fcmh_get(&mq->npfg, &mq->creds, &np);
	if (mp->rc)
		goto out;

	iov[0].iov_base = from;
	iov[0].iov_len = mq->fromlen;
	iov[1].iov_base = to;
	iov[1].iov_len = mq->tolen;

	mp->rc = rsx_bulkserver(rq, &desc, BULK_GET_SINK,
	    SRMC_BULK_PORTAL, iov, 2);
	if (mp->rc)
		goto out;
	pscrpc_free_bulk(desc);

	from[sizeof(from) - 1] = '\0';
	to[sizeof(to) - 1] = '\0';
	mp->rc = mdsio_rename(fcmh_2_mdsio_fid(op), from,
	    fcmh_2_mdsio_fid(np), to, &mq->creds);
 out:
	slm_fcmh_release(np);
	slm_fcmh_release(op);
	return (mp->rc);
}

int
slm_rmc_handle_setattr(struct pscrpc_request *rq)
{
	struct srm_setattr_req *mq;
	struct srm_setattr_rep *mp;
	struct fidc_membh *fcmh;
	int to_set;

	RSX_ALLOCREP(rq, mq, mp);
	mp->rc = slm_fcmh_get(&mq->fg, &mq->creds, &fcmh);
	if (mp->rc)
		goto out;

	to_set = mq->to_set;
	if (to_set & SRM_SETATTRF_SIZE) {
		to_set &= ~SRM_SETATTRF_SIZE;
		to_set |= SRM_SETATTRF_FSIZE;
		if (mq->attr.sst_size == 0) {
			/* full truncate */
		} else {
			to_set |= SRM_SETATTRF_PTRUNCGEN;
			/* partial truncate */
			fcmh_2_ptruncgen(fcmh)++;
		}
	}
	/* If the file is open, mdsio_data will be valid and used.
	 * Otherwise, it will be NULL, and we'll use the mdsio_fid.
	 */
	mp->rc = mdsio_setattr(fcmh_2_mdsio_fid(fcmh), &mq->attr, to_set,
	    &mq->creds, &mp->attr, fcmh_2_mdsio_data(fcmh));

 out:
	slm_fcmh_release(fcmh);
	return (0);
}

int
slm_rmc_handle_set_newreplpol(struct pscrpc_request *rq)
{
	struct srm_set_newreplpol_req *mq;
	struct slash_inode_handle *ih;
	struct srm_generic_rep *mp;
	struct fidc_membh *fcmh;

	RSX_ALLOCREP(rq, mq, mp);

	if (mq->pol < 0 || mq->pol >= NBRP) {
		mp->rc = EINVAL;
		return (0);
	}

	mp->rc = slm_fcmh_get(&mq->fg, &mq->creds, &fcmh);
	if (mp->rc)
		goto out;

	ih = fcmh_2_inoh(fcmh);
	mp->rc = mds_inox_ensure_loaded(ih);
	if (mp->rc == 0) {
		ih->inoh_extras->inox_newbmap_policy = mq->pol;
		ih->inoh_flags |= INOH_EXTRAS_DIRTY;
		mds_inode_sync(ih);
	}
 out:
	slm_fcmh_release(fcmh);
	return (0);
}

int
slm_rmc_handle_set_bmapreplpol(struct pscrpc_request *rq)
{
	struct srm_set_bmapreplpol_req *mq;
	struct slash_inode_handle *ih;
	struct bmap_mds_info *bmdsi;
	struct srm_generic_rep *mp;
	struct fidc_membh *fcmh;
	struct bmapc_memb *bcm;

	RSX_ALLOCREP(rq, mq, mp);

	if (mq->pol < 0 || mq->pol >= NBRP) {
		mp->rc = EINVAL;
		return (0);
	}

	mp->rc = slm_fcmh_get(&mq->fg, &mq->creds, &fcmh);
	if (mp->rc)
		goto out;
	ih = fcmh_2_inoh(fcmh);
	if (!mds_bmap_exists(fcmh, mq->bmapno)) {
		mp->rc = SLERR_BMAP_INVALID;
		goto out;
	}
	mp->rc = mds_bmap_load(fcmh, mq->bmapno, &bcm);
	if (mp->rc)
		goto out;

	BMAP_LOCK(bcm);
	bmdsi = bmap_2_bmdsi(bcm);
	bcm->bcm_od->bh_repl_policy = mq->pol;
	bmdsi->bmdsi_flags |= BMIM_LOGCHG;
	mds_repl_bmap_rel(bcm);

 out:
	slm_fcmh_release(fcmh);
	return (0);
}

int
slm_rmc_handle_statfs(struct pscrpc_request *rq)
{
	struct srm_statfs_req *mq;
	struct srm_statfs_rep *mp;
	struct statvfs sfb;

	RSX_ALLOCREP(rq, mq, mp);
	mp->rc = mdsio_statfs(&sfb);
	sl_externalize_statfs(&sfb, &mp->ssfb);
	return (0);
}

int
slm_rmc_handle_symlink(struct pscrpc_request *rq)
{
	struct pscrpc_bulk_desc *desc;
	struct srm_symlink_req *mq;
	struct srm_symlink_rep *mp;
	struct fidc_membh *p;
	struct iovec iov;
	char linkname[PATH_MAX];

	p = NULL;

	RSX_ALLOCREP(rq, mq, mp);
	mq->name[sizeof(mq->name) - 1] = '\0';
	if (mq->linklen == 0) {
		mp->rc = ENOENT;
		goto out;
	}
	if (mq->linklen >= PATH_MAX) {
		mp->rc = ENAMETOOLONG;
		goto out;
	}

	mp->rc = slm_fcmh_get(&mq->pfg, &mq->creds, &p);
	if (mp->rc)
		goto out;

	iov.iov_base = linkname;
	iov.iov_len = mq->linklen;
	mp->rc = rsx_bulkserver(rq, &desc, BULK_GET_SINK,
	    SRMC_BULK_PORTAL, &iov, 1);
	if (mp->rc)
		goto out;
	pscrpc_free_bulk(desc);

	linkname[sizeof(linkname) - 1] = '\0';
	mp->rc = mdsio_symlink(linkname, fcmh_2_mdsio_fid(p), mq->name,
	    &mq->creds, &mp->attr, &mp->fg, NULL);
 out:
	slm_fcmh_release(p);
	return (mp->rc);
}

int
slm_rmc_handle_unlink(struct pscrpc_request *rq, int isfile)
{
	struct srm_unlink_req *mq;
	struct srm_unlink_rep *mp;
	struct fidc_membh *p;

	RSX_ALLOCREP(rq, mq, mp);
	mp->rc = slm_fcmh_get(&mq->pfg, &mq->creds, &p);
	if (mp->rc)
		goto out;

	mq->name[sizeof(mq->name) - 1] = '\0';
	if (isfile)
		mp->rc = mdsio_unlink(fcmh_2_mdsio_fid(p),
		    mq->name, &mq->creds);
	else
		mp->rc = mdsio_rmdir(fcmh_2_mdsio_fid(p),
		    mq->name, &mq->creds);
	/* XXX update fidc to reflect change */
 out:
	slm_fcmh_release(p);
	return (0);
}

int
slm_rmc_handle_addreplrq(struct pscrpc_request *rq)
{
	struct srm_generic_rep *mp;
	struct srm_replrq_req *mq;

	RSX_ALLOCREP(rq, mq, mp);
	mp->rc = mds_repl_addrq(&mq->fg,
	    mq->bmapno, mq->repls, mq->nrepls);
	return (0);
}

int
slm_rmc_handle_delreplrq(struct pscrpc_request *rq)
{
	struct srm_generic_rep *mp;
	struct srm_replrq_req *mq;

	RSX_ALLOCREP(rq, mq, mp);
	mp->rc = mds_repl_delrq(&mq->fg,
	    mq->bmapno, mq->repls, mq->nrepls);
	return (0);
}

int
slm_rmc_handle_getreplst(struct pscrpc_request *rq)
{
	struct srm_replst_master_req *mq;
	struct srm_replst_master_rep *mp;
	struct slm_replst_workreq *rsw;

	RSX_ALLOCREP(rq, mq, mp);

	rsw = PSCALLOC(sizeof(*rsw));
	rsw->rsw_fg = mq->fg;
	rsw->rsw_cid = mq->id;
	rsw->rsw_csvc = slm_getclcsvc(rq->rq_export);
	lc_add(&slm_replst_workq, rsw);
	return (0);
}

int
slm_rmc_handler(struct pscrpc_request *rq)
{
	int rc = 0;

	switch (rq->rq_reqmsg->opc) {
	case SRMT_REPL_ADDRQ:
		rc = slm_rmc_handle_addreplrq(rq);
		break;
	case SRMT_CONNECT:
		rc = slm_rmc_handle_connect(rq);
		break;
	case SRMT_CREATE:
		rc = slm_rmc_handle_create(rq);
		break;
	case SRMT_REPL_DELRQ:
		rc = slm_rmc_handle_delreplrq(rq);
		break;
	case SRMT_GETATTR:
		rc = slm_rmc_handle_getattr(rq);
		break;
	case SRMT_GETBMAP:
		rc = slm_rmc_handle_getbmap(rq);
		break;
	case SRMT_REPL_GETST:
		rc = slm_rmc_handle_getreplst(rq);
		break;
	case SRMT_LINK:
		rc = slm_rmc_handle_link(rq);
		break;
	case SRMT_MKDIR:
		rc = slm_rmc_handle_mkdir(rq);
		break;
	case SRMT_LOOKUP:
		rc = slm_rmc_handle_lookup(rq);
		break;
	case SRMT_OPEN:
		rc = slm_rmc_handle_open(rq);
		break;
	case SRMT_OPENDIR:
		rc = slm_rmc_handle_opendir(rq);
		break;
	case SRMT_READDIR:
		rc = slm_rmc_handle_readdir(rq);
		break;
	case SRMT_READLINK:
		rc = slm_rmc_handle_readlink(rq);
		break;
	case SRMT_RELEASE:
		rc = slm_rmc_handle_release(rq);
		break;
	case SRMT_RENAME:
		rc = slm_rmc_handle_rename(rq);
		break;
	case SRMT_RMDIR:
		rc = slm_rmc_handle_unlink(rq, 0);
		break;
	case SRMT_SETATTR:
		rc = slm_rmc_handle_setattr(rq);
		break;
	case SRMT_SET_NEWREPLPOL:
		rc = slm_rmc_handle_set_newreplpol(rq);
		break;
	case SRMT_SET_BMAPREPLPOL:
		rc = slm_rmc_handle_set_bmapreplpol(rq);
		break;
	case SRMT_STATFS:
		rc = slm_rmc_handle_statfs(rq);
		break;
	case SRMT_SYMLINK:
		rc = slm_rmc_handle_symlink(rq);
		break;
	case SRMT_UNLINK:
		rc = slm_rmc_handle_unlink(rq, 1);
		break;
	default:
		psc_errorx("Unexpected opcode %d", rq->rq_reqmsg->opc);
		rq->rq_status = -ENOSYS;
		return (pscrpc_error(rq));
	}
	target_send_reply_msg(rq, rc, 0);
	return (rc);
}
