# Test-only bounded bootstrap pool; normal configurations do not include this.
WS025_EARLY_LOW_PAGES ?= 275
.PHONY: ws025-force-early-arena
$(BUILD)/src/hal/amd64/page.o: AMD64_CPPFLAGS += -DWS025_EARLY_LOW_PAGES=$(WS025_EARLY_LOW_PAGES)
$(BUILD)/src/hal/amd64/page.o: ws025-force-early-arena
