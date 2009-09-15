/* $Id$ */

#ifndef _SLASH_INODE_H_
#define _SLASH_INODE_H_

#include <sys/types.h>
#include <sys/stat.h>

#include "pfl/types.h"
#include "psc_util/assert.h"
#include "psc_util/crc.h"
#include "psc_util/lock.h"

#include "cache_params.h"
#include "fid.h"

/* To save space in the bmaps, replica stores are kept in the sl-replicas
 *   xattr.  Each bmap uses an array of char's as a bitmap to track which
 *   stores the bmap is replicated to.  Additional bits are used to specify
 *   the freshness of the replica bmaps.  '100' would mean that the bmap
 *   is up-to-date, '110' would mean that the bmap is only one generation
 *   back and therefore may take partial updates.  111 means that the bmap
 *   is more than one generation old.
 * '00' - bmap is not replicated to this ios.
 * '01' - bmap is > one generation back.
 * '10' - bmap is one generation back.
 * '11' - bmap is replicated to the ios and current.
 */
#define SL_MAX_REPLICAS     64
#define SL_BITS_PER_REPLICA 2
#define SL_REPLICA_MASK     (uint8_t)((1 << SL_BITS_PER_REPLICA)-1)
#define SL_REPLICA_NBYTES   ((SL_MAX_REPLICAS * SL_BITS_PER_REPLICA) / NBBY)

#define SL_DEF_SNAPSHOTS    1
#define SL_MAX_GENS_PER_BLK 4

#define SL_SITE_BITS 16
#define SL_RES_BITS  15
#define SL_MDS_BITS  1

#define SL_BMAP_SIZE  SLASH_BMAP_SIZE
#define SL_CRC_SIZE   1048576
#define SL_CRCS_PER_BMAP (SL_BMAP_SIZE / 1048576)

/* Define metafile offsets
 */
#define SL_INODE_START_OFF  0x0000ULL
#define SL_BMAP_START_OFF   0x1000ULL
#define SL_EXTRAS_START_OFF 0x0400ULL

#define SL_NULL_CRC 0x436f5d7c450ed606ULL

#define SL_REPL_INACTIVE 0
#define SL_REPL_TOO_OLD  1
#define SL_REPL_OLD      2
#define SL_REPL_ACTIVE   3

typedef u32 sl_mds_id_t;
typedef u64 sl_inum_t;
typedef u32 sl_blkno_t;  /* block number type */
typedef u32 sl_ios_id_t; /* io server id: 16 bit site id
			  *               15 bit resource id
			  *                1 bit metadata svr bool
			  */

#define IOS_ID_ANY (~(sl_ios_id_t)0)
#define BLKNO_ANY  (~(sl_blkno_t)0)

#define SL_MDS_ID_BITS   8 /* These 8 bits compose the first bits of the inode
			    *  number */
#define SL_MDS_ID_MASK ((2 << SL_MDS_ID_BITS) - 1)

/*
 * sl_global_id_build - produce a unique 32 bit identifier from the
 *	object's site and resource id's.
 * @site_id:  id number of the resource's site
 * @res_id:   id number within the site
 * @mds_bool: is this a metadata server?
 */
static inline sl_ios_id_t
sl_global_id_build(u32 site_id, u32 res_id, u32 mds_bool)
{
	sl_ios_id_t ios_id = 0;

	psc_assert(site_id  <= ((1 << SL_SITE_BITS))-1);
	psc_assert(res_id   <= ((1 << SL_RES_BITS))-1);
	psc_assert(mds_bool <= ((1 << SL_MDS_BITS))-1);

	ios_id = site_id << SL_SITE_BITS;
	ios_id |= (res_id + mds_bool);

	return ios_id;
}

static inline u32
sl_glid_to_resid(sl_ios_id_t glid)
{
	sl_ios_id_t tmp = 0;

	tmp = ((1 << SL_SITE_BITS)-1) << (SL_RES_BITS + SL_MDS_BITS);

	return (u32)(glid & ~tmp);
}

/*
 * Point to an offset within the linear metadata file which holds a
 * snapshot.  Snapshots are read-only and their metadata may not be
 * expanded.  Once the offset is established the slash_block structure
 * is used to index up to sn_nblks.
 */
typedef struct slash_snapshot {
	off_t  sn_off;
	size_t sn_nblks;
	time_t sn_date;
} sl_snap_t;

/*
 * Defines a storage system which holds a block or blocks of the
 * respective file.  A number of these structures are statically
 * allocated within the inode and are fixed for the life of the file
 * and apply to snapshots as well as the active file.  This structure
 * saves us from storing the iosystem id within each block at the cost
 * of limiting the number of iosystems which may manage our blocks.
 */
typedef struct slash_replica {
	sl_ios_id_t bs_id;     /* id of this block store    */
} sl_replica_t;

/*
 * Associate a crc with a generation id for a block.
 */
typedef struct slash_gencrc {
	psc_crc_t gc_crc;
} sl_gcrc_t;

/*
 * Slim block structure just holds a generation number and a
 * validation bit.  The io server id is held in the block store array.
 */
typedef struct slash_block_gen {
	unsigned int bl_gen; /* generation number     */
} sl_blkgen_t;


struct slash_bmap_cli_wire {
	u8 bh_crcstates[SL_CRCS_PER_BMAP];
	u8 bh_repls[SL_REPLICA_NBYTES];
};

/** 
 * slash_bmap_od - slash bmap on-disk structure.  This structure maps the
 *    persistent state of the bmap within the inode's metafile.  
 * @bh_gen: current generation number.
 * @bh_crcs: the crc table, one 8 byte crc per sliver.
 * @bh_crcstates: some bits for describing the state of a sliver.
 * @bh_repls: bitmap used for tracking the repication status of this bmap.
 * @bh_bhcrc: on-disk checksum.
 */
struct slash_bmap_od {
	sl_blkgen_t bh_gen;
	sl_gcrc_t   bh_crcs[SL_CRCS_PER_BMAP];
	u8          bh_crcstates[SL_CRCS_PER_BMAP];
	u8          bh_repls[SL_REPLICA_NBYTES];
	psc_crc_t   bh_bhcrc;
};

#define BMAP_OD_SZ (sizeof(struct slash_bmap_od))
#define BMAP_OD_CRCSZ (sizeof(struct slash_bmap_od)-(sizeof(psc_crc_t)))

#define slash_bmap_wire slash_bmap_od

enum slash_bmap_slv_states {
	BMAP_SLVR_DATA = (1<<0), /* Data present, otherwise slvr is hole */
	BMAP_SLVR_CRC  = (1<<1)  /* Valid CRC */
	//XXX ATM, 6 bits are left 
};

#define INO_DEF_NREPLS 4

/*
 * The inode structure lives at the beginning of the metafile and holds
 * the block store array along with snapshot pointers.
 *
 * Replica tables are held here as opposed to
 */
struct slash_inode_od {
	struct slash_fidgen ino_fg;
	uint16_t      ino_version;
	uint16_t      ino_flags;
	uint32_t      ino_bsz;                    /* bmap size               */
	uint32_t      ino_nrepls;                 /* if 0, use ino_prepl     */
	uint32_t      ino_csnap;                  /* current snapshot        */
	uint64_t      ino_lblk;                   /* last bmap               */
	sl_replica_t  ino_repls[INO_DEF_NREPLS];  /* embed a few replicas    */
	psc_crc_t     ino_crc;                    /* crc of the inode        */
};
#define INO_OD_SZ (sizeof(struct slash_inode_od))
#define INO_OD_CRCSZ (sizeof(struct slash_inode_od)-(sizeof(psc_crc_t)))

#define INO_VERSION 0x01

enum slash_inode_flags {
	INO_FL_HAVE_EXTRAS = (1<<0)
};

struct slash_inode_extras_od {
	sl_snap_t     inox_snaps[SL_DEF_SNAPSHOTS];/* snapshot pointers      */
	sl_replica_t  inox_repls[SL_MAX_REPLICAS]; /* replicas              */
	psc_crc_t     inox_crc; 
};
#define INOX_OD_SZ (sizeof(struct slash_inode_extras_od))
#define INOX_OD_CRCSZ							\
	(sizeof(struct slash_inode_extras_od) - (sizeof(psc_crc_t)))


/* File extended attribute names. */
#define SFX_INODE	"sl-inode"
#define SFX_REPLICAS    "sl-replicas"

#endif /* _SLASH_INODE_H_ */
