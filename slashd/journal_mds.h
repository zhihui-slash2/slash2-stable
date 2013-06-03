/* $Id$ */
/*
 * %PSCGPL_START_COPYRIGHT%
 * -----------------------------------------------------------------------------
 * Copyright (c) 2007-2012, Pittsburgh Supercomputing Center (PSC).
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

#ifndef _JOURNAL_MDS_H_
#define _JOURNAL_MDS_H_

#include "lnet/types.h"

#include "slashd/mdsio.h"
#include "slashrpc.h"

#define SLJ_MDS_JNENTS			(128 * 1024)		/* 131072 */
#define SLJ_MDS_READSZ			1024			/* SLJ_MDS_JNENTS % SLJ_MDS_READSZ == 0 */
#define SLJ_MDS_NCRCS			MAX_BMAP_INODE_PAIRS

/**
 * slmds_jent_bmap_crc - Log for bmap CRC updates from IONs.
 * @sjbc_fid: file ID.
 * @sjbc_bmapno: which bmap region.
 * @sjbc_ion: the ion who sent the request.
 * @sjbc_crc: array of slots and crcs.
 * Notes: this is presumed to be the most common entry in the journal.
 */
struct slmds_jent_bmap_crc {
	/*
	 * We can't use ZFS ID here because the create operation may not
	 * make it to the disk.  When we redo the creation, we will get
	 * a different ZFS ID.
	 */
	slfid_t				sjbc_fid;
	sl_bmapno_t			sjbc_bmapno;
	sl_ios_id_t			sjbc_iosid;		/* which IOS got the I/O */
	 int32_t			sjbc_ncrcs;
	uint32_t			sjbc_utimgen;
	uint64_t			sjbc_fsize;		/* new st_size */
	uint64_t			sjbc_aggr_nblks;	/* total st_blocks */
	uint64_t			sjbc_repl_nblks;	/* IOS' st_blocks */
	 int32_t			sjbc_extend;		/* XXX flags */
	struct srt_bmap_crcwire		sjbc_crc[SLJ_MDS_NCRCS];
} __packed;

/**
 * slmds_jent_bmap_repls - Log for updated replication state of a bmap,
 *	from replication activity, write I/O which invalidates valid
 *	replicas, etc.
 * @sjbr_fid: what file.
 * @sjbr_bmapno: which bmap.
 * @sjbr_bgen: the new bmap generation.
 * @sjbr_replpol: bmap's replication policy.
 * @sjbr_nrepls: number of items in @sjbr_repls.
 * @sjbr_repls: the bmap's replica bitmap.
 */
struct slmds_jent_bmap_repls {
	slfid_t				sjbr_fid;
	sl_bmapno_t			sjbr_bmapno;
	sl_bmapgen_t			sjbr_bgen;
	uint32_t			sjbr_replpol;
	uint32_t			sjbr_nrepls;
	uint8_t				sjbr_repls[SL_REPLICA_NBYTES];
} __packed;

/**
 * slmds_jent_ino_repls - Log for an updated inode+inox replicas table.
 * @sjir_fid: what file.
 * @sjir_replpol: new bmap default replication policy.
 * @sjir_nrepls: the number of replicas after this update.
 * @sjir_repls: the replicas table.
 */
struct slmds_jent_ino_repls {
	slfid_t				sjir_fid;
	uint32_t			sjir_replpol;
	uint32_t			sjir_nrepls;
	sl_ios_id_t			sjir_repls[SL_MAX_REPLICAS];
} __packed;

/**
 * slmds_jent_ino_repls - Log for a bmap -> ION assignment.
 * @sjir_lastcli: client's NID+PID.
 * @sjir_ios: I/O system ID.
 * @sjir_fid: file.
 * @sjir_seq: bmap sequence number.
 * @sjir_flags:
 * @sjir_bmapno: which bmap.
 * @sjir_start: issue timestamp.
 */
struct slmds_jent_bmap_assign {
	lnet_process_id_t		sjba_lastcli;
	sl_ios_id_t			sjba_ios;
	slfid_t				sjba_fid;
	uint64_t			sjba_seq;
	uint32_t			sjba_flags;
	sl_bmapno_t			sjba_bmapno;
	uint64_t			sjba_start;
} __packed;

struct slmds_jent_bmapseq {
	uint64_t			sjbsq_high_wm;
	uint64_t			sjbsq_low_wm;
} __packed;

/*
 * This is a "jumbo" journal entry that combines at most three changes
 * into one.
 */
struct slmds_jent_assign_rep {
	uint32_t			sjar_flags;
	 int32_t			sjar_elem;
	struct slmds_jent_ino_repls	sjar_ino;
	struct slmds_jent_bmap_repls	sjar_rep;
	struct slmds_jent_bmap_assign	sjar_bmap;
} __packed;

#define	SLJ_ASSIGN_REP_NONE		0x00
#define	SLJ_ASSIGN_REP_INO		0x01
#define	SLJ_ASSIGN_REP_REP		0x02
#define	SLJ_ASSIGN_REP_BMAP		0x04
#define	SLJ_ASSIGN_REP_FREE		0x08

#define SJ_NAMESPACE_MAGIC		UINT64_C(0xabcd12345678dcba)

#define SJ_NAMESPACE_RECLAIM		0x01

/*
 * For easy seek within a system log file, each entry has a fixed length
 * of 512 bytes (going 1024 allows us to support longer names to make
 * some POSIX tests happy).  But when we send log entries over the
 * network, we condense them (especially the names) to save network
 * bandwidth.
 */
struct slmds_jent_namespace {
	uint64_t			sjnm_magic;		/* debugging */
	 uint8_t			sjnm_op;		/* enum namespace_operation */
	 uint8_t			sjnm_namelen;		/* NUL not included */
	 uint8_t			sjnm_namelen2;		/* NUL not included */
	 uint8_t			sjnm_flag;		/* need garbage collection */

	uint64_t			sjnm_parent_fid;	/* parent dir FID */
	uint64_t			sjnm_target_fid;

	uint64_t			sjnm_target_gen;	/* reclaim only */
	uint64_t			sjnm_new_parent_fid;	/* rename only  */

	uint32_t			sjnm_mask;		/* attribute mask */

	uint32_t			sjnm_mode;		/* file permission */
	 int32_t			sjnm_uid;		/* user ID of owner */
	 int32_t			sjnm_gid;		/* group ID of owner */
	uint64_t			sjnm_atime;		/* time of last access */
	uint64_t			sjnm_atime_ns;
	uint64_t			sjnm_mtime;		/* time of last modification */
	uint64_t			sjnm_mtime_ns;
	uint64_t			sjnm_ctime;		/* time of last status change */
	uint64_t			sjnm_ctime_ns;

	uint64_t			sjnm_size;		/* file size */

	char				sjnm_name[SL_TWO_NAME_MAX]; /* one or two names */
} __packed;

/*
 * The combined size of the standard header of each log entry (i.e.
 * struct psc_journal_enthdr) and its data, if any, must occupy less
 * than or this size.
 */
#define	SLJ_MDS_ENTSIZE			512

#endif /* _JOURNAL_MDS_H_ */
