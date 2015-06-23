#!/bin/sh
# $Id$
# %PSC_COPYRIGHT%

# pfl_daemon.sh: routines for launching daemons, including
# features such as: auto-restart on crash; e-mailing of coredump
# and stack traces; avoidance of multiple daemon instances
#
# see http://github.com/pscedu/pfl

host=$(hostname -s)
nodaemonize=0
name=$prog
ud=/usr/local
dir=/local
filter=
verbose=0
prof=

vprint()
{
	[ $verbose -eq 1 ] && echo "$@"
}

loadprof()
{
	local _h=${1%%%*}
	local t0=${1#*%}
	local _p=${t0%%%*}
	local _fl=${t0#*%}
	local dobreak=0
	shift

	vprint "considering host $_h, prog $_p"

	[ x"${_h%%.*}" = x"$host" ] || return 1
	[ x"$_p" = x"$prog" ] || return 1
	[ -n "$_fl" ] || return 0

	vprint "applying profile"

	while :; do
		fl=${_fl%%%*}
		_fl=${_fl#*%}
		[ x"$fl" = x"$_fl" ] && dobreak=1
		fl=$(echo $fl | perl -pe 's/\\x(..)/chr hex $1/e')

		case $fl in
		args=*)	xargs=${fl#args=};;
		bounce)	;;
		ctl=*)	ctl=${fl#ctl=};;
		dir=*)	dir=${fl#dir=};;
		name=*)	name=${fl#name=};;
		narg=*)	narg=${fl#narg=};;
		share)	;;
		tag=*)	[ x"$1" = x"${fl#tag=}" ] || return 1 ;;
		*)	export $fl;;
		esac
		[ $dobreak -eq 1 ] && break
	done

	[ $# -gt $narg ] && usage

	vprint "profile applied: $t0"

	return 0
}

apply_host_prefs()
{
	local narg=0

	vprint "looking for profile for host $host, prog $prog"

	for dir in							\
	    /local							\
	    /ufs/local							\
	    /usr/local							\
	    /opt
	do
		fn=$dir/pfl_daemon.cfg
		if [ -f $fn ]; then
			av="$@"
			[ $verbose -eq 1 ] && set -x
			[ -f $fn.local ] && . $fn.local
			[ $verbose -eq 1 ] && set +x
			. $fn
			vprint "scanning profiles from $fn"
			[ $# -eq 0 ] && die "unknown deployment: ${av[0]}"
			for ln; do
				loadprof $ln ${av[@]} || continue
				vprint "deployment $prof, host $host"
				return
			done
			warn "no profile for this host; assuming defaults"
			[ ${#av[@]} -gt $narg ] && usage
			return
		fi
	done
	die "cannot find pfl_daemon.cfg"
}

warn()
{
	echo "$@" >&2
}

die()
{
	echo "$0: $@"
	exit 1
}

mygdb()
{
	shift
	local tmpfn=/tmp/gdbinv.$RANDOM
	{
		echo set args $@
		echo set confirm off
		echo set height 0
		echo set history save on
		echo set history size 10000
		echo set print pretty
		echo r
	} > $tmpfn
	export GDBHISTFILE=c/$prog.$id.gdbhist

	# hack for some systems
	[ -e /bin/bash ] && export SHELL=/bin/bash

	exec gdb -f -q -x $tmpfn $prog
}

postproc()
{
	ex=$1

	cf=c/$prog.$id.core
	mv -f *core* $cf 2>/dev/null

	if [ -e "$cf" -a -n "$mail_to" ]; then
		chmod og+r $cf

		cmdfile=/tmp/gdbcmd.$id
		echo thr ap all bt > $cmdfile
		{
			echo To: $mail_to
			echo From: $mail_from
			echo Subject: [sysbug] $prof $host $name down
			echo
			echo core file is $base/$cf
			echo binary is $base/c/$prog.$id
			echo log is $base/log/$host.$name/$tm
			[ $ex -gt 128 ] && echo exited via signal $((ex-128))
			echo --------------------------------------------------
			tail $PSC_LOG_FILE_LINK
			echo --------------------------------------------------
			gdb -batch -c $cf -x $cmdfile c/$prog.$id 2>&1 | $src/tools/filter-pstack
		} | sendmail -t
		echo binary was $base/c/$prog.$id
		echo log file was $base/log/$host.$name/$tm
		rm $cmdfile
	else
		rm c/$prog.$id
	fi

	trap '' EXIT
	[ $ex -eq 0 ] && exit
}

cleanup()
{
	$ctl stop
	postproc 1
	exit 0
}

preproc()
{
	PSC_TIMEOUT=5 $ctl -p sys.version >/dev/null && \
	    die "another instance detected; exiting"

	src=$dir/src/p
	mkdir -p $(dirname $PSC_LOG_FILE)
	cd $base
	mkdir -p c

	find log/$host.$name/ -type f -mtime +30 -exec rm {} \;
	find c/ -type f -size 0 -exec rm {} \;

	# delete core files with no accompanying executable
#	n=[0-9]
#	for i in c/*.$n$n$n c/*.$n$n$n$n c/*.$n$n$n$n$n; do
#		[ -e $i.core ] || rm $i
#	done

	while :; do
		id=$RANDOM
		[ -e c/$prog.$id ] || break
		sleep 1
	done

	mv -f *core* c/ 2>/dev/null
	cp `which $prog` c/$prog.$id
	tm=$(date +%s)
	trap cleanup EXIT
}

vsleep()
{
	local amt=0

	[ $SECONDS -gt 30 ] && return

	case $ATTEMPT in
	1)	let amt=10	ATTEMPT++	;;
	2)	let amt=30	ATTEMPT++	;;
	3)	let amt=60	ATTEMPT++	;;
	4)	let amt=600	ATTEMPT++	;;
	6)	let amt=1800	ATTEMPT++	;;
	7)	let amt=3600			;;
	*)	ATTEMPT=1;
	esac
	export ATTEMPT

	echo restarting after $amt seconds...
	sleep $amt
}

_rundaemon()
{
	preproc
	"$@"
	postproc $?
	vsleep
	exec $0 "${bkav[@]}"
}

rundaemon()
{
	vprint "launching daemon"

	if [ $nodaemonize -eq 0 ]; then
		_rundaemon "$@" &
		disown
	else
		_rundaemon "$@"
	fi
}
