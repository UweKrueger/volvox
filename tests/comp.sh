#!/bin/sh
gcc -o echoer echoer.c -O2
gcc -o spawntest spawntest.c -O2 -L../lib -lvolvox -Wl,-rpath,../lib
