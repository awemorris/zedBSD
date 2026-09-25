# ws034-p019: the userland test configuration with OpenSSL and the CA bundle.
include plan/ws035/tests/config-amd64-userland.mk
ZEDBSD_USER_PROGRAMS += openssl ca-certificates
