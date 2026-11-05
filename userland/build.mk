PACKAGE=

build:
	@echo "Building package $(PACKAGE)..."
	cd $(PACKAGE) && make -f build.mk build install
