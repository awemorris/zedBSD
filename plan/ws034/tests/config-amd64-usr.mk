# ws034-p037: the SSH harness image plus the packages whose files moved or
# depend on ones that moved (OpenSSL's libraries, remacs' dictionary).
include plan/ws035/tests/config-amd64-guest.mk
ZEDBSD_USER_PROGRAMS += zlib expat ca-certificates curl remacs
