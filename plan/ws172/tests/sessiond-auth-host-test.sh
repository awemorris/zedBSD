#!/bin/sh
# The host test of sessiond's authentication (ws172-p002): the counts' rules and the exchange with a fake
# passkey, under ASan and UBSan.
# usage: plan/ws172/tests/sessiond-auth-host-test.sh   (from the repository's top)
set -eu
OUT=${OUT:-build/ws172-sessiond-host}
mkdir -p "$OUT"
UID_SELF=$(id -u)

# The fake passkey: its answer depends on the secret (the 4th line of an auth, the 3rd of a change).
cat > "$OUT/passkey" <<SCRIPT
#!/bin/sh
read -r operation
read -r name
case "\$operation" in
styles) echo "ok uid=$UID_SELF styles=password,pin"; exit 0 ;;
enrolled) echo "ok uid=$UID_SELF pin=1 fido2=0"; exit 0 ;;
auth) read -r style; read -r secret ;;
*) read -r secret ;;
esac
case "\$secret" in
right) if [ "\$operation" = enroll-fido2 ]; then echo "ok uid=$UID_SELF id=Q1"; else echo "ok uid=$UID_SELF"; fi ;;
touch) echo "status touch"; echo "ok uid=$UID_SELF" ;;
otheruid) echo "ok uid=$((UID_SELF + 1))" ;;
hang) /bin/sleep 100 ;;
stubborn) trap '' TERM; /bin/sleep 100 ;;
silent) exit 2 ;;
*) echo "fail bad-secret"; exit 1 ;;
esac
SCRIPT
chmod 0700 "$OUT/passkey"
mkdir -p "$OUT/db"
chmod 0700 "$OUT/db"

cc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -I. \
	-DSESSIOND_PASSKEY="\"$PWD/$OUT/passkey\"" -DSESSIOND_PASSKEY_MS=1500LL -DSESSIOND_PASSKEY_KEY_MS=1500LL \
	-DSESSIOND_PASSKEY_GRACE_MS=500LL -DSESSIOND_PIN_TRIES_DIRECTORY="\"$PWD/$OUT/db\"" \
	-DSESSIOND_PIN_TRIES_PATH="\"$PWD/$OUT/db/pin-tries\"" \
	-o "$OUT/sessiond-auth-host-test" plan/ws172/tests/sessiond-auth-host-test.c userland/desktop/sessiond/auth.c \
	userland/desktop/sessiond/auth-policy.c
timeout 120 "$OUT/sessiond-auth-host-test" 2> "$OUT/log"
