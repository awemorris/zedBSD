#!/bin/sh
# ws199-p001 i03: sessiond's requests of a security key's own (KEYINFO, KEYPIN, KEYRESET) and the words and delays
# that changed with them (section 4.4, 4.6, review-2 N1/N6/N7/N8, review-3 R5), with a fake passkey, under ASan and UBSan.
# usage: plan/ws199/tests/sessiond-keys-host-test.sh   (from the repository's top)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
OUT=${OUT:-build/ws199-sessiond-keys-host}
mkdir -p "$OUT"
UID_SELF=$(id -u)

# The fake passkey: its answer depends on the operation and the secrets.
cat > "$OUT/passkey" <<SCRIPT
#!/bin/sh
read -r operation
read -r name
case "\$operation" in
styles) echo "ok uid=$UID_SELF styles=password,pin"; exit 0 ;;
enrolled) echo "ok uid=$UID_SELF pin=1 fido2=1"; exit 0 ;;
key-info) echo "ok uid=$UID_SELF count=1 name=59 pin=1 retries=8 min=4"; exit 0 ;;
key-set-pin) read -r pin; if [ "\$pin" = right ]; then echo "ok uid=$UID_SELF"; exit 0; fi; echo "fail pin-policy"; exit 1 ;;
key-change-pin) read -r pin; read -r fresh; if [ "\$pin" = right ]; then echo "ok uid=$UID_SELF"; exit 0; fi; echo "fail bad-key-pin"; exit 1 ;;
key-reset)
	read -r password
	case "\$password" in
	right) echo "status verified"; echo "status replug"; echo "status touch"; echo "ok uid=$UID_SELF removed=1"; exit 0 ;;
	late) echo "status verified"; echo "status replug"; echo "fail not-allowed"; exit 1 ;;
	*) echo "fail bad-secret"; exit 1 ;;
	esac ;;
enroll-fido2) read -r password; read -r label; read -r pin; echo "fail bad-key-pin"; exit 1 ;;
auth) read -r style; read -r secret; /bin/sleep 100 ;;
*) echo "fail bad-request"; exit 1 ;;
esac
SCRIPT
chmod 0700 "$OUT/passkey"

cc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -I. \
	-DSESSIOND_PASSKEY="\"$PWD/$OUT/passkey\"" -DSESSIOND_PASSKEY_MS=3000LL -DSESSIOND_PASSKEY_KEY_MS=3000LL \
	-DSESSIOND_PASSKEY_RESET_MS=3000LL -DSESSIOND_PASSKEY_GRACE_MS=500LL \
	-o "$OUT/sessiond-keys-host-test" plan/ws199/tests/sessiond-keys-host-test.c userland/desktop/sessiond/auth.c \
	userland/desktop/sessiond/auth-policy.c
timeout 120 "$OUT/sessiond-keys-host-test" 2> "$OUT/log"
