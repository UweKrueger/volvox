VOLVOXVERSION=0.01-current-$(shell git rev-parse --short HEAD)
# VOLVOXVERSION=0.01

all: volvox

# do 'make clean' when changing optimization options or compiler version

.PHONY: debug
debug:
	cd src && $(MAKE) OPT="-O0 -glldb" VOLVOXVERSION=$(VOLVOXVERSION)

.PHONY: volvox
volvox: srcdir

.PHONY: srcdir
srcdir:
	cd src && $(MAKE) VOLVOXVERSION=$(VOLVOXVERSION)

.PHONY: clean
clean:
	cd src && $(MAKE) clean

.PHONY: depend
depend:
	cd src && $(MAKE) .depend
