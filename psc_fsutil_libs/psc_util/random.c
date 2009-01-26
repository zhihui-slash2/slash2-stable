/* $Id$ */

#include <sys/types.h>

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "psc_util/lock.h"
#include "psc_util/log.h"
#include "psc_util/random.h"

#define _PATH_URANDOM "/dev/urandom"
#define SMALL_BUF 128

psc_spinlock_t	 psc_random_lock = LOCK_INITIALIZER;
unsigned char	 psc_random_buf[SMALL_BUF];
ssize_t		 psc_random_siz;
unsigned char	*psc_random_pos;

/**
 * psc_random_getbyte: get a byte from our random data buffer and refill
 *	our buffer if needed.
 * Notes: not thread-safe!
 */
__static uint64_t
psc_random_getbyte(void)
{
	int fd;

	if (psc_random_pos &&
	    psc_random_pos >= psc_random_buf + psc_random_siz)
		psc_random_pos = NULL;

	if (psc_random_pos == NULL) {
		fd = open(_PATH_URANDOM, O_RDONLY, 0);
		if (fd == -1)
			psc_fatal("open %s", _PATH_URANDOM);
		if ((psc_random_siz = read(fd, psc_random_buf,
		    sizeof(psc_random_buf))) == -1)
			psc_fatal("read %s", _PATH_URANDOM);
		if (psc_random_siz == 0)
			psc_fatalx("EOF on %s", _PATH_URANDOM);
		psc_random_pos = psc_random_buf;
		close(fd);
	}
	return (*psc_random_pos++);
}

/**
 * psc_random32: get a random 32-bit number from /dev/urandom.
 */
uint32_t
psc_random32(void)
{
	uint32_t r;

	spinlock(&psc_random_lock);
	r = psc_random_getbyte();
	r |= psc_random_getbyte() << 8;
	r |= psc_random_getbyte() << 16;
	r |= psc_random_getbyte() << 24;
	freelock(&psc_random_lock);
	return (r);
}

/**
 * psc_random64: get a random 64-bit number from /dev/urandom.
 */
uint64_t
psc_random64(void)
{
	uint64_t r;

	spinlock(&psc_random_lock);
	r = psc_random_getbyte();
	r |= psc_random_getbyte() << 8;
	r |= psc_random_getbyte() << (8*2);
	r |= psc_random_getbyte() << (8*3);
	r |= psc_random_getbyte() << (8*4);
	r |= psc_random_getbyte() << (8*5);
	r |= psc_random_getbyte() << (8*6);
	r |= psc_random_getbyte() << (8*7);
	freelock(&psc_random_lock);
	return (r);
}

/**
 * psc_random32u: get a uniformly distributed random 32-bit number
 *	from /dev/urandom.
 * @max: bound.
 */
uint32_t
psc_random32u(uint32_t max)
{
	uint32_t r, min;

	if (max < 2)
		return (0);

	min = 0x100000000UL % max;

	for (;;) {
		r = psc_random32();
		if (r >= min)
			break;
	}
	return (r % max);
}
