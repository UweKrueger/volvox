all: volvox

volvox: srcdir

.PHONY: srcdir
srcdir:
	cd src && $(MAKE)

.PHONY: clean
clean:
	rm -f volvox
	cd src && $(MAKE) clean
