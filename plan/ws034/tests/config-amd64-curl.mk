# ws034-p017: the userland test configuration with curl (and what it requires).
include plan/ws035/tests/config-amd64-userland.mk
ZEDBSD_USER_PROGRAMS += curl openssl zlib ca-certificates
