default:
	$(MAKE) -C src
test: default
	$(MAKE) -C tests test
clean:
	$(MAKE) -C src clean
	$(MAKE) -C tests clean
.DEFAULT:
	$(MAKE) -C src $@
.PHONY: default test clean .DEFAULT
