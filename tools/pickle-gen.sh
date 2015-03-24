#!/bin/sh
# $Id$
# %PSC_START_COPYRIGHT%
# -----------------------------------------------------------------------------
# Copyright (c) 2010-2013, Pittsburgh Supercomputing Center (PSC).
#
# Permission to use, copy, modify, and distribute this software
# for any purpose with or without fee is hereby granted, provided
# that the above copyright notice and this permission notice
# appear in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
# WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
# WARRANTIES OF MERCHANTABILITY AND FITNESS.  IN NO EVENT SHALL
# THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR
# CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
# LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
# NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
# CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
#
# Pittsburgh Supercomputing Center	phone: 412.268.4960  fax: 412.268.5832
# 300 S. Craig Street			e-mail: remarks@psc.edu
# Pittsburgh, PA 15213			web: http://www.psc.edu/
# -----------------------------------------------------------------------------
# %PSC_END_COPYRIGHT%

usage()
{
	echo "usage: $0 rootdir hasvers makeprog localdefsmk" >&2
	exit 1
}

if getopts "" c; then
	usage
fi

if [ $# -ne 4 ]; then
	usage
fi

rootdir=$1
hasvers=$2
make=$3
localdefsmk=$4

cat <<EOF >&2
================================================================================
System compatibility probes started on `hostname`
================================================================================
EOF

{
	echo "# generated by `id -un` from `hostname` on `date`"
	cd $rootdir/compat
	make | sort
	echo "PICKLE_HAS_VERSION=$hasvers"
} >$localdefsmk

cat <<EOF >&2
================================================================================
System compatibility probes finished on `hostname`
================================================================================
EOF
