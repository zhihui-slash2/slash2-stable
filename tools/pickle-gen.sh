#!/bin/sh
# $Id$

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

{
	echo "# generated by `whoami` from `hostname` on `date`"

	for i in $rootdir/compat/*; do
		cd $i >/dev/null || continue

		name=$(echo ${i##*/} | tr 'a-z' 'A-Z')
		$make clean >/dev/null
		$make >/dev/null && echo "PICKLE_HAVE_$name=1"
		cd - >/dev/null
	done

	echo "PICKLE_HAS_VERSION=$hasvers"
} > $localdefsmk
