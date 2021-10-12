all: volvox

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
