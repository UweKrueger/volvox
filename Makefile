all: volvox

.PHONY: gcc
gcc:
	cd src && make -f Makefile.gcc

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
