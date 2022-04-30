cl -c -O2 echoer.c
link echoer.obj
cl -c -O2 /MD spawntest.c
link -out:spawntest.exe spawntest.obj ..\lib\libvolvox.lib
copy /Y ..\libvolvox.dll .
