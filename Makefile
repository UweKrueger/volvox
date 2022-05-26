all: volvox

# do 'make distclean' when changing optimization options or compiler version

.PHONY: debug
debug:
	cd src && $(MAKE) CC=gcc OPT="-O0 -ggdb"

volvox: srcdir

.PHONY: srcdir
srcdir:
	cd src && $(MAKE)

.PHONY: clean
clean:
	cd src && $(MAKE) clean

.PHONY: distclean
distclean:
	cd src && $(MAKE) distclean

.PHONY: depend
depend:
	cd src && $(MAKE) .depend
