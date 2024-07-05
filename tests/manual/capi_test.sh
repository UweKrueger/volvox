#!/bin/sh
rm -f *.o capi
../../volvox -c capi_cdef.vx
cc -c -O2 capi_use.c
cc -o capi capi_use.o capi_cdef.o -L../../lib -Wl,-rpath,../../lib -lvolvox
./capi
