#!/bin/sh

usage() {
	echo "Usage: $0 [-v] [cc|ccmain|volvox|clean]" 1>&2
	exit 1
}

makeclean() {
	rm $VERBOSE -f *.o *.obj *.O *.OBJ capi capi.exe
}

VERBOSE=""
if [ -z $CC ]; then
	CC=cc
fi

if [ $# -ge 1 ] && [ "$1" = "-v" ]; then
	VERBOSE="-v"
	shift
fi

if [ $# -gt 1 ]; then
	echo "At most 1 target allowed" 1>&2
	usage
fi

if [ $# = 0 ] || [ "$1" = "volvox" ]; then
	makeclean
	$CC $VERBOSE -c -O2 capi_use.c
	../../volvox $VERBOSE -o capi capi_cdef.vx capi_use.o
elif [ "$1" = "cc" ]; then
	makeclean
	$CC $VERBOSE -c -O2 capi_use.c
	../../volvox $VERBOSE -c capi_cdef.vx
	$CC $VERBOSE -o capi capi_use.o capi_cdef.o -L../../lib -Wl,-rpath,../../lib -lvolvox
elif [ "$1" = "ccmain" ]; then
	makeclean
	$CC $VERBOSE -c -O2 capi_use_main.c
	../../volvox $VERBOSE -c -Mcapi_cdefs_init capi_cdef_nomain.vx
	$CC $VERBOSE -o capi capi_use_main.o capi_cdef_nomain.o -L../../lib -Wl,-rpath,../../lib -lvolvox
elif [ "$1" = "clean" ]; then
	makeclean
	exit 0
else
	echo "unexpected parameter \"$1\"" 1>&2
	usage
fi

if [ -x ./capi ]; then
	./capi
else
	echo "unable to make executable" 1>&2
	exit 1
fi
