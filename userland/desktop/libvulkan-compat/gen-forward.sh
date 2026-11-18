#!/bin/sh
# zedBSD; Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# Generates the process-wide forwarding table and the maintained export list.
set -eu
TABLE=$1
OUT=$2
mkdir -p "$OUT"
LC_ALL=C awk -F '\t' '
/^#/ || NF == 0 { next }
{ count++; block[count]=$1; name[count]=$2; ret[count]=$3; params[count]=$4; args[count]=$5; kind[count]=$6; exported[count]=$7 }
END {
 print "/* Generated from functions.tsv; declarations and definitions are included separately. */"
 print "#ifdef COMPAT_FORWARD_DECLARATIONS"
 for (i=1; i<=count; i++) if (kind[i]=="F") {
  print "/* The backend trampoline for " name[i] ", published once before any call. */"
  print "static PFN_" name[i] " next_" name[i] ";\n"
 }
 print "/* Maintained function ownership; immutable for the lifetime of the process. */"
 print "static const struct compat_name compat_names[] = {"
 for (i=1; i<=count; i++) {
  pointer=exported[i]=="y" ? "(PFN_vkVoidFunction)" name[i] : "NULL"
  print "\t{ \"" name[i] "\", \047" kind[i] "\047, " pointer " },"
 }
 print "};\n#else"
 for (i=1; i<=count; i++) if (kind[i]=="F") {
  print "/*\n * Forwards " name[i] " to the backend without changing its handles.\n */"
  print "VKAPI_ATTR " ret[i] " VKAPI_CALL\n" name[i] "("
  n=split(params[i], p, ", ")
  for (j=1; j<=n; j++) print "\t" p[j] (j<n ? "," : ")")
  print "{"
  if (ret[i]!="void") print "\t" ret[i] " answer;\n"
  print "\t/* Enters the backend boundary and detects symbol recursion. */"
  print "\tcompat_enter(\"" name[i] "\");"
  print "\tif (next_" name[i] " == NULL)\n\t\tcompat_missing(\"" name[i] "\");\n"
  print "\t/* Calls the backend trampoline with the original arguments. */"
  print "\t" (ret[i]=="void" ? "" : "answer = ") "next_" name[i] "(" args[i] ");\n"
  print "\t/* Leaves the completed backend call. */\n\tcompat_leave();\n"
  if (ret[i]=="VkResult") {
   print "\t/* Preserves a backend failure or a non-success status. */"
   print "\tif (answer != VK_SUCCESS)\n\t\treturn answer;\n"
  }
  print "\t/* Succeeded: preserves the backend\047s answer. */"
  print "\treturn" (ret[i]=="void" ? "" : " answer") ";\n}\n"
 }
 print "/*\n * Publishes backend trampolines before the first external call.\n */"
 print "void\ncompat_forward_fill(\n\tvoid *handle)\n{"
 for (i=1; i<=count; i++) if (kind[i]=="F") {
  print "\t/* Resolves the backend\047s exported " name[i] " trampoline. */"
  print "\tnext_" name[i] " = (PFN_" name[i] ")dlsym(handle, \"" name[i] "\");\n"
 }
 print "\t/* Succeeded: the process-wide trampoline table is published. */\n\treturn;\n}\n#endif"
}' "$TABLE" > "$OUT/forward.inc.new"
LC_ALL=C awk -F '\t' 'BEGIN {print "{ global:"} !/^#/ && $7=="y" {print "\t" $2 ";"} END {print "local: *; };"}' "$TABLE" > "$OUT/exports.map.new"
mv "$OUT/forward.inc.new" "$OUT/forward.inc"
mv "$OUT/exports.map.new" "$OUT/exports.map"
