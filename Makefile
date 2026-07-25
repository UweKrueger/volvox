SHELL := bash

VOLVOXVERSION=0.02pre-$(shell git rev-parse --short HEAD)
# VOLVOXVERSION=0.02

ifeq ($(shell test -d C\:/Windows && echo -n true),true)
	TAR := tar
	UNAME := Windows
else
	TAR := bsdtar
	UNAME := $(shell uname)-$(shell uname -m)
endif
TARFILE := volvox-$(UNAME)-$(VOLVOXVERSION).txz

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

package: $(TARFILE)

$(TARFILE): volvox
	$(TAR) -cJf $@ --exclude .gitignore --exclude Attic bin lib doc
