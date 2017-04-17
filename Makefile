default:
	$(MAKE) -C src
.DEFAULT:
	$(MAKE) -C src $@
.PHONY: default .DEFAULT
