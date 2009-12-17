#!/bin/sh
# $Id$
# XXX preserve authors

usage()
{
	echo "usage: $0 file ..." >&2
	exit 1
}

if getopts "" c; then
	usage
fi

shift $(($OPTIND - 1))

if [ $# -eq 0 ]; then
	usage;
fi

for i; do
	perl -W -i - $i <<'EOF'
local $/;

my $data = <>;

my $startyr = 2006;		# obviously not always correct...
my $yr;

if ($data =~ m{/\* \$Id: \Q$ARGV\E \d+ (\d+)-}) {
	$yr = $1;
} else {
	$yr = 1900 + (localtime((stat $ARGV)[9]))[5];
}

if ($yr < $startyr) {
	warn "$ARGV: $yr from Id tag before $startyr\n";
	$yr = $startyr;
}

my $cpyears = $startyr;
$cpyears .= "-$yr" if $yr > $startyr;

# Recognize short tags for copyright generation
$data =~ s{/\* %PSC_COPYRIGHT% \*/
}
{/\*
 \* %PSC_START_COPYRIGHT%
 \* -----------------------------------------------------------------------------
 \* -----------------------------------------------------------------------------
 \* %PSC_END_COPYRIGHT%
 \*/
};

$data =~ s
{/\*
 \* %PSC_START_COPYRIGHT%
 \* -----------------------------------------------------------------------------
 \* -----------------------------------------------------------------------------(.*?)
 \* %PSC_END_COPYRIGHT%
 \*/
}
{/*
 * %PSC_START_COPYRIGHT%
 * -----------------------------------------------------------------------------
 * Copyright (c) $cpyears, Pittsburgh Supercomputing Center (PSC).
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
 * -----------------------------------------------------------------------------$1
 * %PSC_END_COPYRIGHT%
 */
}s;

print $data;
EOF
done
