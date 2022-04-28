cl -c -O2 echoer.c
link echoer.obj
cl -c -O2 /MD spawntest.c
link -out:spawntest.exe spawntest.obj ..\src\wstatic.obj ..\lib\libvolvox.lib
copy /Y ..\libvolvox.dll .
