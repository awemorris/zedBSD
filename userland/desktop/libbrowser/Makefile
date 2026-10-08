include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# libbrowser (ws074-p057): the Web browser engine as a component, plan/ws074/design.md §19 -- HTML, CSS,
# the layout, JavaScript, the DOM, the network, the images and the CPU and Vulkan renderers -- behind
# <browser/browser.h> (userland/desktop/include/browser/browser.h).  /bin/browser is its shell; any program can show HTML with it.
# The engine sources, private headers, tables and shaders belong to this library.
# The app in desktop/browser uses only <browser/browser.h>; exports.map keeps the implementation private.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
LIBBROWSER_DIR := userland/desktop/libbrowser

# The tables made from published lists (the HTML named character references, the Unicode case
# mappings) are committed: tools/regenerate.sh downloads the lists at their pinned SHA-256 and runs the
# generators, and the licences' notices are installed with the library (plan/ws074/design.md §17 D4).
LIBBROWSER_SOURCES := $(LIBBROWSER_DIR)/base/arena.c \
	$(LIBBROWSER_DIR)/base/buffer.c \
	$(LIBBROWSER_DIR)/base/vector.c \
	$(LIBBROWSER_DIR)/base/utf.c \
	$(LIBBROWSER_DIR)/base/hash.c \
	$(LIBBROWSER_DIR)/base/file.c \
	$(LIBBROWSER_DIR)/base/unicode.c \
	$(LIBBROWSER_DIR)/base/unicode-case-table.c \
	$(LIBBROWSER_DIR)/vm/heap.c \
	$(LIBBROWSER_DIR)/vm/string.c \
	$(LIBBROWSER_DIR)/vm/atom.c \
	$(LIBBROWSER_DIR)/vm/shape.c \
	$(LIBBROWSER_DIR)/vm/object.c \
	$(LIBBROWSER_DIR)/vm/function.c \
	$(LIBBROWSER_DIR)/vm/realm.c \
	$(LIBBROWSER_DIR)/vm/code.c \
	$(LIBBROWSER_DIR)/vm/number.c \
	$(LIBBROWSER_DIR)/vm/operation.c \
	$(LIBBROWSER_DIR)/vm/access.c \
	$(LIBBROWSER_DIR)/vm/spread.c \
	$(LIBBROWSER_DIR)/vm/class.c \
	$(LIBBROWSER_DIR)/vm/promise.c \
	$(LIBBROWSER_DIR)/vm/generator.c \
	$(LIBBROWSER_DIR)/vm/interpreter.c \
	$(LIBBROWSER_DIR)/js/lexer.c \
	$(LIBBROWSER_DIR)/js/parser.c \
	$(LIBBROWSER_DIR)/js/parse_expr.c \
	$(LIBBROWSER_DIR)/js/parse_stmt.c \
	$(LIBBROWSER_DIR)/js/dump.c \
	$(LIBBROWSER_DIR)/js/scope.c \
	$(LIBBROWSER_DIR)/js/emit.c \
	$(LIBBROWSER_DIR)/js/compile.c \
	$(LIBBROWSER_DIR)/js/compile_expr.c \
	$(LIBBROWSER_DIR)/js/script.c \
	$(LIBBROWSER_DIR)/js/builtin.c \
	$(LIBBROWSER_DIR)/js/builtin_error.c \
	$(LIBBROWSER_DIR)/js/builtin_object.c \
	$(LIBBROWSER_DIR)/js/builtin_function.c \
	$(LIBBROWSER_DIR)/js/builtin_array.c \
	$(LIBBROWSER_DIR)/js/builtin_string.c \
	$(LIBBROWSER_DIR)/js/builtin_regexp.c \
	$(LIBBROWSER_DIR)/js/regexp.c \
	$(LIBBROWSER_DIR)/js/builtin_date.c \
	$(LIBBROWSER_DIR)/js/builtin_json.c \
	$(LIBBROWSER_DIR)/js/builtin_number.c \
	$(LIBBROWSER_DIR)/js/builtin_math.c \
	$(LIBBROWSER_DIR)/js/builtin_global.c \
	$(LIBBROWSER_DIR)/js/builtin_promise.c \
	$(LIBBROWSER_DIR)/js/builtin_generator.c \
	$(LIBBROWSER_DIR)/js/builtin_symbol.c \
	$(LIBBROWSER_DIR)/js/builtin_iterator.c \
	$(LIBBROWSER_DIR)/js/builtin_collection.c \
	$(LIBBROWSER_DIR)/js/builtin_uri.c \
	$(LIBBROWSER_DIR)/js/tool.c \
	$(LIBBROWSER_DIR)/html/input.c \
	$(LIBBROWSER_DIR)/html/entities-table.c \
	$(LIBBROWSER_DIR)/html/tokenizer.c \
	$(LIBBROWSER_DIR)/html/parser.c \
	$(LIBBROWSER_DIR)/html/modes.c \
	$(LIBBROWSER_DIR)/html/body.c \
	$(LIBBROWSER_DIR)/html/foreign.c \
	$(LIBBROWSER_DIR)/dom/node.c \
	$(LIBBROWSER_DIR)/dom/attribute.c \
	$(LIBBROWSER_DIR)/dom/xml.c \
	$(LIBBROWSER_DIR)/dom/removal.c \
	$(LIBBROWSER_DIR)/dom/adopt.c \
	$(LIBBROWSER_DIR)/dom/names.c \
	$(LIBBROWSER_DIR)/dom/control.c \
	$(LIBBROWSER_DIR)/dom/form.c \
	$(LIBBROWSER_DIR)/dom/select.c \
	$(LIBBROWSER_DIR)/dom/radio.c \
	$(LIBBROWSER_DIR)/dom/serialize.c \
	$(LIBBROWSER_DIR)/xml/parser.c \
	$(LIBBROWSER_DIR)/xml/dom.c \
	$(LIBBROWSER_DIR)/css/tokenizer.c \
	$(LIBBROWSER_DIR)/css/parser.c \
	$(LIBBROWSER_DIR)/css/rule-model.c \
	$(LIBBROWSER_DIR)/css/values.c \
	$(LIBBROWSER_DIR)/css/cascade.c \
	$(LIBBROWSER_DIR)/css/index.c \
	$(LIBBROWSER_DIR)/css/media.c \
	$(LIBBROWSER_DIR)/css/ua.c \
	$(LIBBROWSER_DIR)/bind/window.c \
	$(LIBBROWSER_DIR)/bind/frame.c \
	$(LIBBROWSER_DIR)/bind/frame-resource.c \
	$(LIBBROWSER_DIR)/bind/svg-text.c \
	$(LIBBROWSER_DIR)/bind/svg-length.c \
	$(LIBBROWSER_DIR)/bind/style-context.c \
	$(LIBBROWSER_DIR)/bind/style-sheet.c \
	$(LIBBROWSER_DIR)/bind/cssom.c \
	$(LIBBROWSER_DIR)/bind/environment.c \
	$(LIBBROWSER_DIR)/bind/query.c \
	$(LIBBROWSER_DIR)/bind/geometry.c \
	$(LIBBROWSER_DIR)/bind/style.c \
	$(LIBBROWSER_DIR)/bind/encoding.c \
	$(LIBBROWSER_DIR)/bind/storage.c \
	$(LIBBROWSER_DIR)/bind/markup.c \
	$(LIBBROWSER_DIR)/bind/media.c \
	$(LIBBROWSER_DIR)/bind/computed.c \
	$(LIBBROWSER_DIR)/bind/exception.c \
	$(LIBBROWSER_DIR)/bind/node.c \
	$(LIBBROWSER_DIR)/bind/cloning.c \
	$(LIBBROWSER_DIR)/bind/insertion.c \
	$(LIBBROWSER_DIR)/bind/mixin.c \
	$(LIBBROWSER_DIR)/bind/element.c \
	$(LIBBROWSER_DIR)/bind/click.c \
	$(LIBBROWSER_DIR)/bind/submit.c \
	$(LIBBROWSER_DIR)/bind/html-reflection.c \
	$(LIBBROWSER_DIR)/bind/html-input.c \
	$(LIBBROWSER_DIR)/bind/html-textarea.c \
	$(LIBBROWSER_DIR)/bind/text.c \
	$(LIBBROWSER_DIR)/bind/xml.c \
	$(LIBBROWSER_DIR)/bind/document.c \
	$(LIBBROWSER_DIR)/bind/implementation.c \
	$(LIBBROWSER_DIR)/bind/traversal.c \
	$(LIBBROWSER_DIR)/bind/iterator.c \
	$(LIBBROWSER_DIR)/bind/collection.c \
	$(LIBBROWSER_DIR)/bind/table.c \
	$(LIBBROWSER_DIR)/bind/table-row.c \
	$(LIBBROWSER_DIR)/bind/select.c \
	$(LIBBROWSER_DIR)/bind/range.c \
	$(LIBBROWSER_DIR)/bind/event.c \
	$(LIBBROWSER_DIR)/bind/handler.c \
	$(LIBBROWSER_DIR)/bind/timer.c \
	$(LIBBROWSER_DIR)/bind/input.c \
	$(LIBBROWSER_DIR)/net/url.c \
	$(LIBBROWSER_DIR)/net/host.c \
	$(LIBBROWSER_DIR)/net/data.c \
	$(LIBBROWSER_DIR)/net/http.c \
	$(LIBBROWSER_DIR)/net/tls.c \
	$(LIBBROWSER_DIR)/net/cookie.c \
	$(LIBBROWSER_DIR)/net/loader.c \
	$(LIBBROWSER_DIR)/page/page.c \
	$(LIBBROWSER_DIR)/page/link.c \
	$(LIBBROWSER_DIR)/page/resource.c \
	$(LIBBROWSER_DIR)/page/frame-load.c \
	$(LIBBROWSER_DIR)/page/script.c \
	$(LIBBROWSER_DIR)/page/geometry.c \
	$(LIBBROWSER_DIR)/page/storage.c \
	$(LIBBROWSER_DIR)/page/images.c \
	$(LIBBROWSER_DIR)/page/media.c \
	$(LIBBROWSER_DIR)/page/sheets.c \
	$(LIBBROWSER_DIR)/page/fonts.c \
	$(LIBBROWSER_DIR)/page/network.c \
	$(LIBBROWSER_DIR)/page/input.c \
	$(LIBBROWSER_DIR)/page/form.c \
	$(LIBBROWSER_DIR)/view/view.c \
	$(LIBBROWSER_DIR)/image/decode.c \
	$(LIBBROWSER_DIR)/image/jpeg.c \
	$(LIBBROWSER_DIR)/image/png.c \
	$(LIBBROWSER_DIR)/image/gif.c \
	$(LIBBROWSER_DIR)/text/font.c \
	$(LIBBROWSER_DIR)/text/linebreak.c \
	$(LIBBROWSER_DIR)/text/woff.c \
	$(LIBBROWSER_DIR)/layout/box.c \
	$(LIBBROWSER_DIR)/layout/block.c \
	$(LIBBROWSER_DIR)/layout/flex.c \
	$(LIBBROWSER_DIR)/layout/grid.c \
	$(LIBBROWSER_DIR)/layout/table.c \
	$(LIBBROWSER_DIR)/layout/inline.c \
	$(LIBBROWSER_DIR)/layout/float.c \
	$(LIBBROWSER_DIR)/layout/position.c \
	$(LIBBROWSER_DIR)/layout/replaced.c \
	$(LIBBROWSER_DIR)/layout/control.c \
	$(LIBBROWSER_DIR)/layout/dump.c \
	$(LIBBROWSER_DIR)/layout/hit.c \
	$(LIBBROWSER_DIR)/layout/bounds.c \
	$(LIBBROWSER_DIR)/paint/list.c \
	$(LIBBROWSER_DIR)/paint/software.c \
	$(LIBBROWSER_DIR)/paint/vulkan.c

# The library, and the notices of the licences of its committed tables.
LIBBROWSER_IMAGE_DATA := $(if $(ZEDBSD_TOPLEVEL_BUILD),/lib/libbrowser.so=$(BUILD)/dynamic/libbrowser.so) \
	/usr/share/licenses/browser/WHATWG-entities=userland/base/licenses/browser/WHATWG-entities \
	/usr/share/licenses/browser/Unicode-License-V3=userland/base/licenses/browser/Unicode-License-V3

$(eval $(call ZEDBSD_USERLAND_PACKAGE,libbrowser,Web browser engine library,amd64,n,library,$(LIBBROWSER_SOURCES),y,desktop,desktop/libbrowser,desktop/libtruetype desktop/libvulkan desktop/libmedia base/libjpeg-compat base/libpng-compat base/libz-compat base/libgif-compat,,shared-library,$(LIBBROWSER_IMAGE_DATA),userland/desktop/include/browser/browser.h,lib))
