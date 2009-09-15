/* $Id$ */

#ifndef _PFL_ACSVC_H_
#define _PFL_ACSVC_H_

struct psc_thread;

enum {
	ACSOP_ACCESS,
	ACSOP_CHMOD,
	ACSOP_CHOWN,
	ACSOP_LINK,
	ACSOP_LSTAT,
	ACSOP_MKDIR,
	ACSOP_MKNOD,
	ACSOP_OPEN,
	ACSOP_READLINK,
	ACSOP_RENAME,
	ACSOP_RMDIR,
	ACSOP_STAT,
	ACSOP_STATFS,
	ACSOP_SYMLINK,
	ACSOP_TRUNCATE,
	ACSOP_UNLINK,
	ACSOP_UTIMES
};

struct psc_thread *
	acsvc_init(int, const char *, char **);
int	access_fsop(int, uid_t, gid_t, const char *, ...);

#endif /* _PFL_ACSVC_H_ */
