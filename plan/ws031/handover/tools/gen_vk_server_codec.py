#!/usr/bin/env python3
"""WS031 E-127: the executor's struct codec, derived from libvulkan's own.

libvulkan's codec.c is the ground truth of the wire: for every Vulkan record it has an encoder (what the
library SENDS) and, for output records, a decoder (what the library EXPECTS BACK).  The executor needs
the mirror image -- a decoder for each encoder, an encoder for each decoder -- and writing those by hand
is where the wire silently drifts.  This tool reads codec.c and emits the mirror, statement by
statement, and stops at the first statement it does not recognise instead of guessing.

usage: gen_vk_server_codec.py <repo root>     (writes src/drivers/gpu/i915/render/vulkan-codec.inc)
"""
import re, sys

root = sys.argv[1].rstrip("/") + "/"
src = open(root + "userland/base/libvulkan/codec.c").read()

# ---- strip comments, keep code lines ----
src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
lines = [l.rstrip() for l in src.split("\n")]

funcs = []   # (kind, type, body lines)
i = 0
while i < len(lines):
    m = re.match(r"^vulkan_(encode|decode)_(Vk\w+)\($", lines[i])
    if m:
        j = i
        while lines[j] != "{":
            j += 1
        k = j + 1
        body = []
        while lines[k] != "}":
            if lines[k].strip():
                body.append(lines[k].strip())
            k += 1
        funcs.append((m.group(1), m.group(2), body))
        i = k
    i += 1

class Unknown(Exception):
    pass

def elem_stmt_dec(stmt, ptr):
    """One loop-body statement of an ENCODER, turned into a decode of element [index] of `mem`."""
    e = re.escape("record->" + ptr + "[index]")
    if re.fullmatch(r"vulkan_write_u32\(writer, (vulkan_wire_image_layout\()?%s\)?\);" % e, stmt):
        return 4, "{ uint32_t v = drv_i915_wire_read_u32(r); kern_memcpy((char *)mem + index * 4u, &v, 4u); }"
    if re.fullmatch(r"vulkan_write_float\(writer, %s\);" % e, stmt):
        return 4, "{ uint32_t v = drv_i915_wire_read_u32(r); kern_memcpy((char *)mem + index * 4u, &v, 4u); }"
    if re.fullmatch(r"vulkan_write_u64\(writer, %s\);" % e, stmt):
        return 8, "{ uint64_t v = drv_i915_wire_read_u64(r); kern_memcpy((char *)mem + index * 8u, &v, 8u); }"
    if re.fullmatch(r"vulkan_encode_handle\(writer, \(uint64_t\)(\(uintptr_t\))?%s\);" % e, stmt):
        return 8, "{ uint64_t v = drv_i915_wire_read_u64(r); kern_memcpy((char *)mem + index * 8u, &v, 8u); }"
    if re.fullmatch(r"vulkan_write_string\(writer, %s\);" % e, stmt):
        return "sizeof(char *)", "{ const char *v = i915_vkc_read_string(r, a); kern_memcpy((char *)mem + index * sizeof(char *), &v, sizeof(v)); }"
    m = re.fullmatch(r"vulkan_encode_(Vk\w+)\(writer, &%s\);" % e, stmt)
    if m:
        t = m.group(1)
        return "sizeof(%s)" % t, "i915_vkc_dec_%s(r, a, &((%s *)mem)[index]);" % (t, t)
    raise Unknown("loop body: " + stmt)

def gen_dec(t, body):
    out = []
    n = 0
    L = len(body)
    while n < L:
        s = body[n]
        if s in ("size_t index;", "uint64_t count;", "return;"):
            n += 1; continue
        if s == "if (writer->error != VK_SUCCESS)":
            n += 2; continue
        m = re.fullmatch(r"vulkan_write_u32\(writer, (?:vulkan_wire_image_layout\()?record->([\w.\[\]]+)\)?\);", s)
        if m:
            f = m.group(1)
            out.append("record->%s = (__typeof__(record->%s))drv_i915_wire_read_u32(r);" % (f, f)); n += 1; continue
        m = re.fullmatch(r"vulkan_write_u64\(writer, record->([\w.\[\]]+)\);", s)
        if m:
            f = m.group(1)
            out.append("record->%s = (__typeof__(record->%s))drv_i915_wire_read_u64(r);" % (f, f)); n += 1; continue
        if s == "vulkan_write_u64(writer, 0);":
            out.append("(void)drv_i915_wire_read_u64(r);   /* pNext: no chain on this record */"); n += 1; continue
        m = re.fullmatch(r"vulkan_encode_(image|buffer)_external\(writer, record->pNext\);", s)
        if m:
            out.append("i915_vkc_skip_external_chain(r);   /* the %s external-memory declaration */" % m.group(1))
            n += 1; continue
        m = re.fullmatch(r"vulkan_write_float\(writer, record->([\w.\[\]]+)\);", s)
        if m:
            out.append("i915_vkc_read_float(r, &record->%s);" % m.group(1)); n += 1; continue
        m = re.fullmatch(r"vulkan_encode_handle\(writer, \(uint64_t\)(?:\(uintptr_t\))?record->([\w.\[\]]+)\);", s)
        if m:
            f = m.group(1)
            out.append("record->%s = (__typeof__(record->%s))(uintptr_t)drv_i915_wire_read_u64(r);" % (f, f)); n += 1; continue
        m = re.fullmatch(r"vulkan_encode_(Vk\w+)\(writer, &record->([\w.\[\]]+)\);", s)
        if m:
            out.append("i915_vkc_dec_%s(r, a, &record->%s);" % (m.group(1), m.group(2))); n += 1; continue
        m = re.fullmatch(r"vulkan_write_string\(writer, record->(\w+)\);", s)
        if m:
            out.append("record->%s = i915_vkc_read_string(r, a);" % m.group(1)); n += 1; continue
        m = re.fullmatch(r"vulkan_write_bytes\(writer, record->(\w+), (\w+)\);", s)
        if m:
            out.append("i915_vkc_read_bytes(r, record->%s, %s);" % (m.group(1), m.group(2))); n += 1; continue

        # ---- fixed-extent array: count = N; write_u64(count); for ... ----
        m = re.fullmatch(r"count = (\w+);", s)
        if m and m.group(1) != "0" and body[n + 1] == "vulkan_write_u64(writer, count);":
            N = m.group(1)
            out.append("count = drv_i915_wire_read_u64(r);")
            out.append("if (count != %s) { r->error = 1; return; }" % N)
            n += 2
            mb = re.fullmatch(r"vulkan_write_bytes\(writer, record->(\w+), (\w+)\);", body[n])
            if mb:
                out.append("i915_vkc_read_bytes(r, record->%s, %s);" % (mb.group(1), mb.group(2)))
                n += 1; continue
            assert body[n] == "for (index = 0;", body[n]
            stmt = body[n + 3]
            assert body[n + 4] == "}", body[n + 4]
            m2 = re.fullmatch(r"vulkan_write_u32\(writer, record->([\w.]+)\[index\]\);", stmt)
            m3 = re.fullmatch(r"vulkan_write_float\(writer, record->([\w.]+)\[index\]\);", stmt)
            m4 = re.fullmatch(r"vulkan_encode_(Vk\w+)\(writer, &record->([\w.]+)\[index\]\);", stmt)
            if m2:
                f = m2.group(1)
                out.append("for (index = 0; index < count && r->error == 0; index++)")
                out.append("\trecord->%s[index] = (__typeof__(record->%s[index]))drv_i915_wire_read_u32(r);" % (f, f))
            elif m3:
                out.append("for (index = 0; index < count && r->error == 0; index++)")
                out.append("\ti915_vkc_read_float(r, &record->%s[index]);" % m3.group(1))
            elif m4:
                out.append("for (index = 0; index < count && r->error == 0; index++)")
                out.append("\ti915_vkc_dec_%s(r, a, &record->%s[index]);" % (m4.group(1), m4.group(2)))
            else:
                raise Unknown("fixed array body in %s: %s" % (t, stmt))
            n += 5; continue

        # ---- optional / counted payload: count = 0; if (...) count = X; write_u64(count); ... ----
        if s == "count = 0;":
            n += 1
            # the condition may span lines; skip to the assignment
            while not re.fullmatch(r"count = (?!0;).+;", body[n]):
                n += 1
            n += 1
            assert body[n] == "vulkan_write_u64(writer, count);", (t, body[n])
            n += 1
            nxt = body[n]
            out.append("count = drv_i915_wire_read_u64(r);")
            if nxt == "for (index = 0;":
                stmt = body[n + 3]
                ptr = re.search(r"record->(\w+)\[index\]", stmt).group(1)
                size, dec = elem_stmt_dec(stmt, ptr)
                assert body[n + 4] == "}", body[n + 4]
                out.append("record->%s = NULL;" % ptr)
                out.append("if (count != 0) {")
                out.append("\tvoid *mem = i915_vkc_array(r, a, count, %s);" % size)
                out.append("\tfor (index = 0; mem != NULL && index < count && r->error == 0; index++)")
                out.append("\t\t" + dec)
                out.append("\trecord->%s = mem;" % ptr)
                out.append("}")
                n += 5; continue
            if nxt == "if (count != 0)":
                m2 = re.fullmatch(r"vulkan_encode_(Vk\w+)\(writer, record->(\w+)\);", body[n + 1])
                if not m2:
                    raise Unknown("optional record in %s: %s" % (t, body[n + 1]))
                tt, ptr = m2.group(1), m2.group(2)
                out.append("record->%s = NULL;" % ptr)
                out.append("if (count != 0) {")
                out.append("\tvoid *mem = i915_vkc_array(r, a, 1u, sizeof(%s));" % tt)
                out.append("\tif (mem != NULL)")
                out.append("\t\ti915_vkc_dec_%s(r, a, (%s *)mem);" % (tt, tt))
                out.append("\trecord->%s = mem;" % ptr)
                out.append("}")
                n += 2; continue
            m2 = re.fullmatch(r"vulkan_write_bytes\(writer, record->(\w+), \(size_t\)count\);", nxt)
            if m2:
                ptr = m2.group(1)
                out.append("record->%s = NULL;" % ptr)
                out.append("if (count != 0) {")
                out.append("\tvoid *mem = i915_vkc_array(r, a, count, 1u);")
                out.append("\tif (mem != NULL)")
                out.append("\t\ti915_vkc_read_bytes(r, mem, (size_t)count);")
                out.append("\trecord->%s = mem;" % ptr)
                out.append("}")
                n += 1; continue
            raise Unknown("counted payload in %s: %s" % (t, nxt))
        raise Unknown("encoder %s: %s" % (t, s))
    return out

def gen_enc(t, body):
    out = []
    n = 0
    L = len(body)
    while n < L:
        s = body[n]
        if s in ("size_t index;", "uint64_t count;", "uint64_t wide;", "return;"):
            n += 1; continue
        if s == "if (reader->error != VK_SUCCESS)":
            n += 2; continue
        m = re.fullmatch(r"record->([\w.\[\]]+) = \(\w+\)vulkan_read_u32\(reader\);", s)
        if m:
            out.append("drv_i915_wire_reply_u32(w, (uint32_t)record->%s);" % m.group(1)); n += 1; continue
        m = re.fullmatch(r"record->([\w.\[\]]+) = \(\w+\)vulkan_read_u64\(reader\);", s)
        if m:
            out.append("drv_i915_wire_reply_u64(w, (uint64_t)record->%s);" % m.group(1)); n += 1; continue
        m = re.fullmatch(r"record->([\w.\[\]]+) = \(float\)vulkan_read_float\(reader\);", s)
        if m:
            out.append("i915_vkc_reply_float(w, &record->%s);" % m.group(1)); n += 1; continue
        m = re.fullmatch(r"vulkan_decode_(Vk\w+)\(reader, &record->([\w.\[\]]+)\);", s)
        if m:
            out.append("i915_vkc_enc_%s(w, &record->%s);" % (m.group(1), m.group(2))); n += 1; continue
        if s == "wide = vulkan_read_u64(reader);":
            assert body[n + 1] == "if (wide > SIZE_MAX) {", body[n + 1]
            m2 = re.fullmatch(r"record->(\w+) = \(size_t\)wide;", body[n + 5])
            assert m2, body[n + 5]
            out.append("drv_i915_wire_reply_u64(w, (uint64_t)record->%s);" % m2.group(1))
            n += 6; continue
        if s == "count = vulkan_read_u64(reader);":
            m2 = re.fullmatch(r"if \(count != (\w+)\) \{", body[n + 1])
            assert m2, (t, body[n + 1])
            N = m2.group(1)
            assert body[n + 4] == "}", body[n + 4]
            n += 5
            nxt = body[n]
            out.append("drv_i915_wire_reply_u64(w, (uint64_t)%s);" % N)
            m3 = re.fullmatch(r"vulkan_read_bytes\(reader, record->(\w+), (\w+)\);", nxt)
            if m3:
                out.append("i915_vkc_reply_bytes(w, record->%s, %s);" % (m3.group(1), m3.group(2)))
                n += 1; continue
            assert nxt == "for (index = 0;", (t, nxt)
            stmt = body[n + 3]
            assert body[n + 4] == "}", body[n + 4]
            m4 = re.fullmatch(r"record->([\w.]+)\[index\] = \(\w+\)vulkan_read_u32\(reader\);", stmt)
            m5 = re.fullmatch(r"record->([\w.]+)\[index\] = \(float\)vulkan_read_float\(reader\);", stmt)
            m6 = re.fullmatch(r"vulkan_decode_(Vk\w+)\(reader, &record->([\w.]+)\[index\]\);", stmt)
            out.append("for (index = 0; index < (size_t)%s; index++)" % N)
            if m4:
                out.append("\tdrv_i915_wire_reply_u32(w, (uint32_t)record->%s[index]);" % m4.group(1))
            elif m5:
                out.append("\ti915_vkc_reply_float(w, &record->%s[index]);" % m5.group(1))
            elif m6:
                out.append("\ti915_vkc_enc_%s(w, &record->%s[index]);" % (m6.group(1), m6.group(2)))
            else:
                raise Unknown("decoder loop in %s: %s" % (t, stmt))
            n += 5; continue
        raise Unknown("decoder %s: %s" % (t, s))
    return out

protos, defs = [], []
for kind, t, body in funcs:
    if kind == "encode":
        code = gen_dec(t, body)
        sig = "static void i915_vkc_dec_%s(struct i915_wire_reader *r, struct i915_wire_arena *a, %s *record)" % (t, t)
        pre = ["size_t index = 0;", "uint64_t count = 0;", "(void)index; (void)count; (void)a;", "if (r->error != 0)", "\treturn;"]
    else:
        code = gen_enc(t, body)
        sig = "static void i915_vkc_enc_%s(struct i915_wire_writer *w, const %s *record)" % (t, t)
        pre = ["size_t index = 0;", "(void)index;"]
    protos.append("__attribute__((unused)) " + sig + ";")
    defs.append(sig + "\n{\n" + "\n".join("\t" + l for l in pre + code) + "\n}\n")

hdr = """/*
 * GENERATED by plan/ws031/handover/tools/gen_vk_server_codec.py from userland/base/libvulkan/codec.c.
 * Do not edit: regenerate.  For every record libvulkan ENCODES there is a decoder here
 * (i915_vkc_dec_<Type>), and for every record it DECODES an encoder (i915_vkc_enc_<Type>), each the
 * statement-by-statement mirror of the library's own function, so the two ends cannot drift apart.
 * Pointers a decoder fills point into the command's arena and live until the command returns.
 */
"""
open(root + "src/drivers/gpu/i915/render/vulkan-codec.inc", "w").write(
    hdr + "\n" + "\n".join(protos) + "\n\n" + "\n".join(defs))
print("generated %d decoders, %d encoders" % (sum(1 for f in funcs if f[0] == "encode"),
                                                sum(1 for f in funcs if f[0] == "decode")))
