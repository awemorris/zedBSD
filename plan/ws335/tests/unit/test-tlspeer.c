/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the test tool tlspeer.h itself.
 *
 * The signer against the CAVP ECDSA signature generation vectors (FIPS
 * 186-4 SigGen.txt, which give d and k) and against libcrypto-compat's
 * ECDSA verification; the certificates by parsing them with cryc-x509 and
 * verifying their signatures; the ClientHello reader on the ClientHellos
 * of RFC 8448 and on broken ones.
 *
 * The server against RFC 8448: the server sides of sections 3 (1-RTT)
 * and 5 (HelloRetryRequest) are replayed through tlspeer with the
 * trace's randoms, keys, cookie, EncryptedExtensions and (RSA)
 * CertificateVerify given as raw bytes, and every server message and
 * record must equal the trace's, and every client record of the trace
 * must be taken.
 *
 * The server with its own ECDSA certificates: a client written here (from
 * RFC 8446 and RFC 5246, with libssl-compat's key schedule and record
 * protection) runs TLS 1.3 handshakes over x25519 and over secp256r1 after
 * a HelloRetryRequest, and TLS 1.2 ECDHE handshakes, checking that the
 * CertificateVerify and ServerKeyExchange signatures verify with
 * cryc_ecdsa_verify, that every record opens with sslc_record_open, that
 * the Finished values equal sslc_tls13_finished / sslc_tls12_finished,
 * and that data and KeyUpdates pass both ways, also with records of one
 * byte and input one byte at a time.  Then each break and option is
 * checked to do what it says.
 */

#include "check.h"
#include "vecfile.h"
#include "tlspeer.h"

#include "userland/base/libcrypto-compat/cryc-sigver.h"
#include "userland/base/libcrypto-compat/cryc-x509.h"

/*
 * The special values the client checks, written here again from the RFC
 * so that a mistake in the tool's copies shows: the HelloRetryRequest
 * random, the TLS 1.2 downgrade mark, the CertificateVerify context.
 * Values referenced: RFC 8446 sections 4.1.3 and 4.4.3.
 */
static const uint8_t retry_random[32] = {
	0xcf, 0x21, 0xad, 0x74, 0xe5, 0x9a, 0x61, 0x11, 0xbe, 0x1d, 0x8c, 0x02, 0x1e, 0x65, 0xb8, 0x91,
	0xc2, 0xa2, 0x11, 0x16, 0x7a, 0xbb, 0x8c, 0x5e, 0x07, 0x9e, 0x09, 0xe2, 0xc8, 0xa8, 0x33, 0x9c
};
static const uint8_t downgrade_mark[8] = { 'D', 'O', 'W', 'N', 'G', 'R', 'D', 0x01 };
static const char cv_context[] = "TLS 1.3, server CertificateVerify";

/* The largest buffer the test moves records through. */
#define BIG	300000U

/* One labelled value of RFC 8448 (its section's number, step, label and hex). */
struct trace_value {
	char section;
	char *step;
	char *label;
	char *hex;
};

/* The values of RFC 8448, loaded once (owned here, freed at the end). */
static struct trace_value trace[640];
static size_t trace_count;

/* 1 when the client below feeds the peer one byte at a time. */
static int feed_bytewise;

/*
 * The client side written here: its transcript, TLS 1.3 schedule, record
 * directions, the bytes from the peer not yet read as records, the
 * handshake bytes being reassembled, the data and alerts received.
 */
struct client {
	int tls13;
	const struct sslc_suite *suite;
	enum cryc_hash_id hash;
	size_t hash_size;
	struct cryc_hash transcript;
	struct sslc_tls13_schedule schedule;
	struct sslc_record_dir read;
	struct sslc_record_dir write;
	struct sslc_traffic_keys read12;
	uint8_t client_secret[CRYC_HASH_MAX_SIZE];
	uint8_t server_secret[CRYC_HASH_MAX_SIZE];
	uint8_t in[BIG];
	size_t in_used;
	uint8_t hs[TLSPEER_MESSAGE_MAX];
	size_t hs_used;
	uint8_t data[TLSPEER_DATA_MAX];
	size_t data_size;
	unsigned ccs;
	unsigned alerts;
	unsigned alert;
	unsigned long records;
	int failed;
};

/* What the client reads from a ServerHello. */
struct server_hello {
	uint8_t random[32];
	uint8_t session_id[32];
	size_t session_id_size;
	uint16_t suite;
	uint16_t extensions[TLSPEER_EXT_MAX];
	size_t extension_count;
	uint16_t group;
	uint8_t key[TLSPEER_SHARE_KEY_MAX];
	size_t key_size;
	uint16_t version;
	size_t cookie_size;
};

/* Copies a C string into a new block. */
static char *
copy_string(
	const char *text)
{
	char *copy;
	size_t size;

	/* With its terminator. */
	size = strlen(text) + 1U;
	copy = malloc(size);
	if (copy == NULL) {
		printf("FAIL out of memory\n");
		exit(EXIT_FAILURE);
	}
	memcpy(copy, text, size);
	return copy;
}

/* Loads the RFC 8448 values; returns 0, or -1 when the vector file is missing. */
static int
trace_load(void)
{
	struct vec_file file;
	const char *section;
	const char *step;
	const char *label;
	const char *hex;

	/* Every record with all four fields. */
	if (vec_open(&file, "rfc8448") != 0)
		return -1;
	while (vec_next(&file) && trace_count < sizeof(trace) / sizeof(trace[0])) {
		section = vec_get(&file, "section");
		step = vec_get(&file, "step");
		label = vec_get(&file, "label");
		hex = vec_get(&file, "hex");
		if (section == NULL || step == NULL || label == NULL || hex == NULL)
			continue;
		trace[trace_count].section = section[0];
		trace[trace_count].step = copy_string(step);
		trace[trace_count].label = copy_string(label);
		trace[trace_count].hex = copy_string(hex);
		trace_count++;
	}
	vec_close(&file);
	return 0;
}

/* Frees the RFC 8448 values. */
static void
trace_free(void)
{
	size_t index;

	/* Each copy. */
	for (index = 0; index < trace_count; index++) {
		free(trace[index].step);
		free(trace[index].label);
		free(trace[index].hex);
	}
	trace_count = 0;
}

/*
 * Decodes the nth value (from 0) of section whose step contains step and
 * whose label is label into out; returns its size (a missing value is a
 * failed check and gives 0).
 */
static size_t
trace_get(
	char section,
	const char *step,
	const char *label,
	unsigned nth,
	uint8_t *out,
	size_t room)
{
	size_t index;
	size_t size;

	/* The nth match. */
	for (index = 0; index < trace_count; index++) {
		if (trace[index].section != section)
			continue;
		if (strstr(trace[index].step, step) == NULL || strcmp(trace[index].label, label) != 0)
			continue;
		if (nth > 0U) {
			nth--;
			continue;
		}
		size = hex_decode(trace[index].hex, out, room);
		if (size == (size_t)-1)
			break;
		return size;
	}
	printf("FAIL RFC 8448 %c: no \"%s\" / \"%s\"\n", section, step, label);
	CHECK(0);
	return 0;
}

/* Decodes a DER Ecdsa-Sig-Value into r and s of size bytes each (strict enough for the signer's output). */
static int
signature_decode(
	const uint8_t *der,
	size_t der_size,
	size_t size,
	uint8_t *r,
	uint8_t *s)
{
	struct tlspeer_reader reader;
	struct tlspeer_reader sequence;
	struct tlspeer_reader integer;
	uint8_t *outs[2];
	unsigned which;
	unsigned tag;

	/* SEQUENCE (short or one-byte long form, enough for P-256 and P-384). */
	outs[0] = r;
	outs[1] = s;
	tlspeer_reader_init(&reader, der, der_size);
	tag = tlspeer_get_int(&reader, 1);
	if (tag != 0x30U || der_size < 2U)
		return EBADMSG;
	if (der[1] == 0x81U)
		tlspeer_get_int(&reader, 1);
	tlspeer_get_vector(&reader, 1, 6, 255, &sequence);
	if (!tlspeer_reader_done(&reader))
		return EBADMSG;

	/* Two positive INTEGERs, minimal, of at most size bytes. */
	for (which = 0; which < 2U; which++) {
		tag = tlspeer_get_int(&sequence, 1);
		tlspeer_get_vector(&sequence, 1, 1, size + 1U, &integer);
		if (tag != 0x02U || integer.error != 0)
			return EBADMSG;
		if ((integer.data[0] & 0x80U) != 0U)
			return EBADMSG;
		if (integer.size > 1U && integer.data[0] == 0U && (integer.data[1] & 0x80U) == 0U)
			return EBADMSG;
		if (integer.data[0] == 0U) {
			integer.data++;
			integer.size--;
		}
		if (integer.size > size)
			return EBADMSG;
		memset(outs[which], 0, size);
		memcpy(outs[which] + size - integer.size, integer.data, integer.size);
	}
	if (!tlspeer_reader_done(&sequence))
		return EBADMSG;
	return 0;
}

/* Verifies a DER ECDSA signature over message (hashed with hash) with an uncompressed key on curve. */
static int
verify_signature(
	enum cryc_curve_id curve,
	const uint8_t *key,
	size_t key_size,
	enum cryc_hash_id hash,
	const uint8_t *message,
	size_t message_size,
	const uint8_t *signature,
	size_t signature_size)
{
	struct cryc_ecp ec;
	uint8_t digest[CRYC_HASH_MAX_SIZE];
	uint8_t r[CRYC_ECP_MAX_BYTES];
	uint8_t s[CRYC_ECP_MAX_BYTES];
	int error;

	/* The curve, the digest, r and s, the verification. */
	error = cryc_ecp_init(&ec, curve);
	if (error != 0)
		return error;
	error = cryc_hash_digest(hash, message, message_size, digest);
	if (error != 0)
		return error;
	error = signature_decode(signature, signature_size, ec.size, r, s);
	if (error != 0)
		return error;
	return cryc_ecdsa_verify(&ec, key, key_size, digest, cryc_hash_size(hash), r, s);
}

/*
 * Also verifies with cryc_signature_verify (the X.509 and TLS path)
 * once that module is implemented; until then it answers ENOSYS and is
 * not counted.
 */
static void
check_sigver(
	enum cryc_curve_id curve,
	const uint8_t *key,
	size_t key_size,
	enum cryc_sig_alg alg,
	const uint8_t *message,
	size_t message_size,
	const uint8_t *signature,
	size_t signature_size,
	int expected)
{
	static int noted;
	struct cryc_pubkey pubkey;
	struct cryc_sig_params params;
	int error;

	/* The key and the algorithm. */
	memset(&pubkey, 0, sizeof(pubkey));
	pubkey.type = CRYC_KEY_EC;
	pubkey.curve = curve;
	pubkey.point = key;
	pubkey.point_size = key_size;
	memset(&params, 0, sizeof(params));
	params.alg = alg;

	/* The verdict, unless the module is still a skeleton. */
	error = cryc_signature_verify(&pubkey, &params, message, message_size, signature, signature_size);
	if (error == ENOSYS) {
		if (!noted)
			printf("note: cryc_signature_verify answers ENOSYS; its checks are skipped\n");
		noted = 1;
		return;
	}
	CHECK(error == expected);
}

/*
 * The signer.
 */

/* One CAVP SigGen vector (FIPS 186-4 ECDSA, SigGen.txt). */
struct siggen {
	enum cryc_curve_id curve;
	enum cryc_hash_id hash;
	const char *msg;
	const char *d;
	const char *qx;
	const char *qy;
	const char *k;
	const char *r;
	const char *s;
};

/*
 * The first three records of [P-256,SHA-256] and the first two of
 * [P-384,SHA-384].
 * Values referenced: NIST CAVP 186-4ecdsatestvectors.zip SigGen.txt
 * (SHA-256 95d0a19c03f6d2dc16e9e18fd5c3318e1952e0bc88a45127c996c35b4a15f4e2).
 */
static const struct siggen siggen_vectors[] = {
	{ CRYC_CURVE_P256, CRYC_HASH_SHA256,
	  "5905238877c77421f73e43ee3da6f2d9e2ccad5fc942dcec0cbd25482935faaf416983fe165b1a045ee2bcd2e6dca3bdf46c4310a7461f9a37960ca672d3feb5473e253605fb1ddfd28065b53cb5858a8ad28175bf9bd386a5e471ea7a65c17cc934a9d791e91491eb3754d03799790fe2d308d16146d5c9b0d0debd97d79ce8",
	  "519b423d715f8b581f4fa8ee59f4771a5b44c8130b4e3eacca54a56dda72b464",
	  "1ccbe91c075fc7f4f033bfa248db8fccd3565de94bbfb12f3c59ff46c271bf83",
	  "ce4014c68811f9a21a1fdb2c0e6113e06db7ca93b7404e78dc7ccd5ca89a4ca9",
	  "94a1bbb14b906a61a280f245f9e93c7f3b4a6247824f5d33b9670787642a68de",
	  "f3ac8061b514795b8843e3d6629527ed2afd6b1f6a555a7acabb5e6f79c8c2ac",
	  "8bf77819ca05a6b2786c76262bf7371cef97b218e96f175a3ccdda2acc058903" },
	{ CRYC_CURVE_P256, CRYC_HASH_SHA256,
	  "c35e2f092553c55772926bdbe87c9796827d17024dbb9233a545366e2e5987dd344deb72df987144b8c6c43bc41b654b94cc856e16b96d7a821c8ec039b503e3d86728c494a967d83011a0e090b5d54cd47f4e366c0912bc808fbb2ea96efac88fb3ebec9342738e225f7c7c2b011ce375b56621a20642b4d36e060db4524af1",
	  "0f56db78ca460b055c500064824bed999a25aaf48ebb519ac201537b85479813",
	  "e266ddfdc12668db30d4ca3e8f7749432c416044f2d2b8c10bf3d4012aeffa8a",
	  "bfa86404a2e9ffe67d47c587ef7a97a7f456b863b4d02cfc6928973ab5b1cb39",
	  "6d3e71882c3b83b156bb14e0ab184aa9fb728068d3ae9fac421187ae0b2f34c6",
	  "976d3a4e9d23326dc0baa9fa560b7c4e53f42864f508483a6473b6a11079b2db",
	  "1b766e9ceb71ba6c01dcd46e0af462cd4cfa652ae5017d4555b8eeefe36e1932" },
	{ CRYC_CURVE_P256, CRYC_HASH_SHA256,
	  "3c054e333a94259c36af09ab5b4ff9beb3492f8d5b4282d16801daccb29f70fe61a0b37ffef5c04cd1b70e85b1f549a1c4dc672985e50f43ea037efa9964f096b5f62f7ffdf8d6bfb2cc859558f5a393cb949dbd48f269343b5263dcdb9c556eca074f2e98e6d94c2c29a677afaf806edf79b15a3fcd46e7067b7669f83188ee",
	  "e283871239837e13b95f789e6e1af63bf61c918c992e62bca040d64cad1fc2ef",
	  "74ccd8a62fba0e667c50929a53f78c21b8ff0c3c737b0b40b1750b2302b0bde8",
	  "29074e21f3a0ef88b9efdf10d06aa4c295cc1671f758ca0e4cd108803d0f2614",
	  "ad5e887eb2b380b8d8280ad6e5ff8a60f4d26243e0124c2f31a297b5d0835de2",
	  "35fb60f5ca0f3ca08542fb3cc641c8263a2cab7a90ee6a5e1583fac2bb6f6bd1",
	  "ee59d81bc9db1055cc0ed97b159d8784af04e98511d0a9a407b99bb292572e96" },
	{ CRYC_CURVE_P384, CRYC_HASH_SHA384,
	  "6b45d88037392e1371d9fd1cd174e9c1838d11c3d6133dc17e65fa0c485dcca9f52d41b60161246039e42ec784d49400bffdb51459f5de654091301a09378f93464d52118b48d44b30d781eb1dbed09da11fb4c818dbd442d161aba4b9edc79f05e4b7e401651395b53bd8b5bd3f2aaa6a00877fa9b45cadb8e648550b4c6cbe",
	  "201b432d8df14324182d6261db3e4b3f46a8284482d52e370da41e6cbdf45ec2952f5db7ccbce3bc29449f4fb080ac97",
	  "c2b47944fb5de342d03285880177ca5f7d0f2fcad7678cce4229d6e1932fcac11bfc3c3e97d942a3c56bf34123013dbf",
	  "37257906a8223866eda0743c519616a76a758ae58aee81c5fd35fbf3a855b7754a36d4a0672df95d6c44a81cf7620c2d",
	  "dcedabf85978e090f733c6e16646fa34df9ded6e5ce28c6676a00f58a25283db8885e16ce5bf97f917c81e1f25c9c771",
	  "50835a9251bad008106177ef004b091a1e4235cd0da84fff54542b0ed755c1d6f251609d14ecf18f9e1ddfe69b946e32",
	  "0475f3d30c6463b646e8d3bf2455830314611cbde404be518b14464fdb195fdcc92eb222e61f426a4a592c00a6a89721" },
	{ CRYC_CURVE_P384, CRYC_HASH_SHA384,
	  "d768f41e6e8ec2125d6cf5786d1ba96668ac6566c5cdbbe407f7f2051f3ad6b1acdbfe13edf0d0a86fa110f405406b69085219b5a234ebdb93153241f785d45811b3540d1c37424cc7194424787a51b79679266484c787fb1ded6d1a26b9567d5ea68f04be416caf3be9bd2cafa208fe2a9e234d3ae557c65d3fe6da4cb48da4",
	  "23d9f4ea6d87b7d6163d64256e3449255db14786401a51daa7847161bf56d494325ad2ac8ba928394e01061d882c3528",
	  "5d42d6301c54a438f65970bae2a098cbc567e98840006e356221966c86d82e8eca515bca850eaa3cd41f175f03a0cbfd",
	  "4aef5a0ceece95d382bd70ab5ce1cb77408bae42b51a08816d5e5e1d3da8c18fcc95564a752730b0aabea983ccea4e2e",
	  "67ba379366049008593eac124f59ab017358892ee0c063d38f3758bb849fd25d867c3561563cac1532a323b228dc0890",
	  "fb318f4cb1276282bb43f733a7fb7c567ce94f4d02924fc758635ab2d1107108bf159b85db080cdc3b30fbb5400016f3",
	  "588e3d7af5da03eae255ecb1813100d95edc243476b724b22db8e85377660d7645ddc1c2c2ee4eaea8b683dbe22f86ca" }
};

/* Checks the signer on the CAVP vectors, on its fixed k, and on k out of range. */
static void
test_signer(void)
{
	struct tlspeer_key key;
	uint8_t msg[256];
	uint8_t d[48];
	uint8_t q[97];
	uint8_t k[48];
	uint8_t r[48];
	uint8_t s[48];
	uint8_t r2[48];
	uint8_t s2[48];
	uint8_t expected_r[48];
	uint8_t expected_s[48];
	uint8_t digest[CRYC_HASH_MAX_SIZE];
	uint8_t der[120];
	uint8_t bad[48];
	size_t msg_size;
	size_t der_size;
	size_t size;
	size_t index;
	size_t hash_size;
	int error;

	/* Each vector: Q from d, (r, s) from k, and the signature verifies. */
	for (index = 0; index < sizeof(siggen_vectors) / sizeof(siggen_vectors[0]); index++) {
		msg_size = hex_decode(siggen_vectors[index].msg, msg, sizeof(msg));
		size = hex_decode(siggen_vectors[index].d, d, sizeof(d));
		hex_decode(siggen_vectors[index].qx, q + 1, sizeof(q) - 1U);
		hex_decode(siggen_vectors[index].qy, q + 1 + size, sizeof(q) - 1U - size);
		q[0] = 0x04;
		hex_decode(siggen_vectors[index].k, k, sizeof(k));
		hex_decode(siggen_vectors[index].r, expected_r, sizeof(expected_r));
		hex_decode(siggen_vectors[index].s, expected_s, sizeof(expected_s));
		CHECK(tlspeer_key_init(&key, siggen_vectors[index].curve, d) == 0);
		CHECK(key.public_size == 1U + 2U * size);
		CHECK_BYTES(key.public_key, q, 1U + 2U * size);
		cryc_hash_digest(siggen_vectors[index].hash, msg, msg_size, digest);
		hash_size = cryc_hash_size(siggen_vectors[index].hash);
		CHECK(tlspeer_ecdsa_sign(&key, digest, hash_size, k, r, s) == 0);
		CHECK_BYTES(r, expected_r, size);
		CHECK_BYTES(s, expected_s, size);
		CHECK(cryc_ecdsa_verify(&key.ec, key.public_key, key.public_size, digest, hash_size, r, s) == 0);

		/* The fixed k: deterministic, verifies, differs from the next message's. */
		CHECK(tlspeer_ecdsa_sign(&key, digest, hash_size, NULL, r, s) == 0);
		CHECK(tlspeer_ecdsa_sign(&key, digest, hash_size, NULL, r2, s2) == 0);
		CHECK_BYTES(r2, r, size);
		CHECK_BYTES(s2, s, size);
		CHECK(cryc_ecdsa_verify(&key.ec, key.public_key, key.public_size, digest, hash_size, r, s) == 0);
		digest[0] ^= 1U;
		CHECK(tlspeer_ecdsa_sign(&key, digest, hash_size, NULL, r2, s2) == 0);
		CHECK(memcmp(r2, r, size) != 0);
		CHECK(cryc_ecdsa_verify(&key.ec, key.public_key, key.public_size, digest, hash_size, r, s) == EBADMSG);
		digest[0] ^= 1U;

		/* The DER form decodes back to the fixed-k signature and verifies. */
		CHECK(tlspeer_ecdsa_sign_der(&key, digest, hash_size, der, sizeof(der), &der_size) == 0);
		CHECK(signature_decode(der, der_size, size, r2, s2) == 0);
		CHECK_BYTES(r2, r, size);
		CHECK_BYTES(s2, s, size);
		CHECK(verify_signature(siggen_vectors[index].curve, key.public_key, key.public_size, siggen_vectors[index].hash, msg, msg_size, der, der_size) == 0);
		check_sigver(siggen_vectors[index].curve, key.public_key, key.public_size,
		    siggen_vectors[index].curve == CRYC_CURVE_P256 ? CRYC_SIG_ECDSA_SHA256 : CRYC_SIG_ECDSA_SHA384,
		    msg, msg_size, der, der_size, 0);
		CHECK(tlspeer_ecdsa_sign_der(&key, digest, hash_size, der, 8, &der_size) == ENOBUFS);
		CHECK(der_size == 0U);

		/* k = 0 and k = n are refused. */
		memset(bad, 0, sizeof(bad));
		CHECK(tlspeer_ecdsa_sign(&key, digest, hash_size, bad, r2, s2) == EINVAL);
		cryc_bn_encode(key.ec.order.modulus, key.ec.limbs, bad, size);
		CHECK(tlspeer_ecdsa_sign(&key, digest, hash_size, bad, r2, s2) == EINVAL);
		bad[size - 1U] -= 1U;
		CHECK(tlspeer_ecdsa_sign(&key, digest, hash_size, bad, r2, s2) == 0);
		CHECK(cryc_ecdsa_verify(&key.ec, key.public_key, key.public_size, digest, hash_size, r2, s2) == 0);
	}

	/* d = 0 and d = n are refused; an unknown curve too. */
	memset(d, 0, sizeof(d));
	CHECK(tlspeer_key_init(&key, CRYC_CURVE_P256, d) == EINVAL);
	error = tlspeer_key_init(&key, CRYC_CURVE_NONE, d);
	CHECK(error == ENOTSUP);
	CHECK(tlspeer_key_from_label(&key, CRYC_CURVE_P256, "tlspeer root") == 0);
	cryc_bn_encode(key.ec.order.modulus, key.ec.limbs, d, 32);
	CHECK(tlspeer_key_init(&key, CRYC_CURVE_P256, d) == EINVAL);

	/* A hash shorter than the order (SHA-256 on P-384) is E as it is; longer is cut. */
	CHECK(tlspeer_key_from_label(&key, CRYC_CURVE_P384, "tlspeer leaf") == 0);
	cryc_hash_digest(CRYC_HASH_SHA256, (const uint8_t *)"abc", 3, digest);
	CHECK(tlspeer_ecdsa_sign(&key, digest, 32, NULL, r, s) == 0);
	CHECK(cryc_ecdsa_verify(&key.ec, key.public_key, key.public_size, digest, 32, r, s) == 0);
	CHECK(tlspeer_key_from_label(&key, CRYC_CURVE_P256, "tlspeer leaf") == 0);
	cryc_hash_digest(CRYC_HASH_SHA512, (const uint8_t *)"abc", 3, digest);
	CHECK(tlspeer_ecdsa_sign(&key, digest, 64, NULL, r, s) == 0);
	CHECK(cryc_ecdsa_verify(&key.ec, key.public_key, key.public_size, digest, 64, r, s) == 0);
}

/*
 * The certificates.
 */

/* Checks one parsed certificate against what it was made from, and its signature with the issuer's key. */
static void
check_cert(
	const uint8_t *der,
	size_t der_size,
	const struct tlspeer_key *subject_key,
	const struct tlspeer_key *issuer_key,
	int is_ca,
	const char *host)
{
	struct cryc_x509 cert;
	enum cryc_curve_id curve;
	enum cryc_hash_id hash;
	enum cryc_sig_alg alg;
	uint8_t digest[CRYC_SHA256_SIZE];
	int error;

	/* It parses, with the fields as made. */
	error = cryc_x509_parse(&cert, der, der_size);
	CHECK(error == 0);
	if (error != 0)
		return;
	curve = subject_key->ec.size == 48U ? CRYC_CURVE_P384 : CRYC_CURVE_P256;
	CHECK(cert.version == 3U);
	CHECK(cert.not_before == TLSPEER_NOT_BEFORE_TIME);
	CHECK(cert.not_after == TLSPEER_NOT_AFTER_TIME);
	CHECK(cert.key.type == CRYC_KEY_EC);
	CHECK(cert.key.curve == curve);
	CHECK(cert.key.point_size == subject_key->public_size);
	CHECK(cert.key.point_size == subject_key->public_size && memcmp(cert.key.point, subject_key->public_key, subject_key->public_size) == 0);
	CHECK((cert.flags & CRYC_X509_HAS_BASIC_CONSTRAINTS) != 0U);
	CHECK(((cert.flags & CRYC_X509_CA) != 0U) == (is_ca != 0));
	CHECK((cert.flags & CRYC_X509_HAS_KEY_USAGE) != 0U);
	CHECK((cert.flags & CRYC_X509_UNKNOWN_CRITICAL) == 0U);
	CHECK(cert.ski_size == 20U);
	cryc_hash_digest(CRYC_HASH_SHA256, subject_key->public_key, subject_key->public_size, digest);
	CHECK(cert.ski_size == 20U && memcmp(cert.ski, digest, 20) == 0);
	cryc_hash_digest(CRYC_HASH_SHA256, issuer_key->public_key, issuer_key->public_size, digest);
	if (is_ca == 0)
		CHECK(cert.aki_size == 20U && memcmp(cert.aki, digest, 20) == 0);
	if (is_ca != 0) {
		CHECK(cert.key_usage == (CRYC_KU_KEY_CERT_SIGN | CRYC_KU_CRL_SIGN));
		CHECK((cert.flags & CRYC_X509_SELF_ISSUED) != 0U);
		CHECK(cert.aki_size == 0U);
	} else {
		CHECK(cert.key_usage == CRYC_KU_DIGITAL_SIGNATURE);
		CHECK(cert.ext_key_usage == CRYC_EKU_SERVER_AUTH);
		CHECK((cert.flags & CRYC_X509_HAS_SAN) != 0U);
		CHECK(cert.aki_size == 20U);
		CHECK(cert.san_size == 2U + strlen(host));
		CHECK(cert.san_size >= 2U && cert.san[0] == 0x82U && memcmp(cert.san + 2, host, strlen(host)) == 0);
	}

	/* The signature over the tbsCertificate verifies with the issuer's key, and not after a change. */
	hash = issuer_key->ec.size == 48U ? CRYC_HASH_SHA384 : CRYC_HASH_SHA256;
	alg = issuer_key->ec.size == 48U ? CRYC_SIG_ECDSA_SHA384 : CRYC_SIG_ECDSA_SHA256;
	CHECK(cert.signature_alg.alg == alg);
	CHECK(cert.signature_size > 1U);
	CHECK(verify_signature(curve, issuer_key->public_key, issuer_key->public_size, hash, cert.tbs, cert.tbs_size, cert.signature, cert.signature_size) == 0);
	CHECK(verify_signature(curve, issuer_key->public_key, issuer_key->public_size, hash, cert.tbs, cert.tbs_size - 1U, cert.signature, cert.signature_size) == EBADMSG);
	check_sigver(curve, issuer_key->public_key, issuer_key->public_size, alg, cert.tbs, cert.tbs_size, cert.signature, cert.signature_size, 0);
}

/* Checks the PKI of both curves and a certificate with an address. */
static void
test_pki(void)
{
	static const uint8_t address[4] = { 192, 0, 2, 7 };
	struct tlspeer_pki pki;
	struct tlspeer_cert spec;
	struct cryc_x509 cert;
	uint8_t der[TLSPEER_CERT_MAX];
	uint8_t again[TLSPEER_CERT_MAX];
	size_t size;
	size_t again_size;

	/* P-256 and P-384: root and leaf parse and verify. */
	CHECK(tlspeer_pki_init(&pki, CRYC_CURVE_P256, "server.test") == 0);
	check_cert(pki.root, pki.root_size, &pki.root_key, &pki.root_key, 1, NULL);
	check_cert(pki.leaf, pki.leaf_size, &pki.leaf_key, &pki.root_key, 0, "server.test");
	CHECK(tlspeer_pki_init(&pki, CRYC_CURVE_P384, "p384.server.test") == 0);
	check_cert(pki.root, pki.root_size, &pki.root_key, &pki.root_key, 1, NULL);
	check_cert(pki.leaf, pki.leaf_size, &pki.leaf_key, &pki.root_key, 0, "p384.server.test");

	/* Deterministic: the same spec gives the same bytes. */
	CHECK(tlspeer_pki_init(&pki, CRYC_CURVE_P256, "server.test") == 0);
	memset(&spec, 0, sizeof(spec));
	spec.subject = "192.0.2.7";
	spec.issuer = "tlspeer root CA";
	spec.subject_key = &pki.leaf_key;
	spec.issuer_key = &pki.root_key;
	spec.serial = 0x80000001U;
	spec.address = address;
	spec.address_size = 4;
	spec.not_before = TLSPEER_NOT_BEFORE;
	spec.not_after = TLSPEER_NOT_AFTER;
	CHECK(tlspeer_make_cert(&spec, der, sizeof(der), &size) == 0);
	CHECK(tlspeer_make_cert(&spec, again, sizeof(again), &again_size) == 0);
	CHECK(size == again_size);
	CHECK_BYTES(der, again, size);
	CHECK(cryc_x509_parse(&cert, der, size) == 0);
	CHECK(cert.serial_size == 5U && cert.serial[0] == 0U && cert.serial[1] == 0x80U);
	CHECK(cert.san_size == 6U && cert.san[0] == 0x87U && memcmp(cert.san + 2, address, 4) == 0);

	/* Too little room, and a spec without its parts. */
	CHECK(tlspeer_make_cert(&spec, der, 100, &size) == ENOBUFS);
	CHECK(size == 0U);
	spec.serial = 0;
	CHECK(tlspeer_make_cert(&spec, der, sizeof(der), &size) == EINVAL);
}

/*
 * The ClientHello reader.
 */

/* Rewrites the 3-byte message length after the header of a hand-cut ClientHello. */
static void
fix_length(
	uint8_t *message,
	size_t size)
{
	/* The body is the rest. */
	cryc_store_be24(message + 1, (uint32_t)(size - 4U));
}

/* Checks the reader on the ClientHellos of RFC 8448 sections 3 and 5 and on broken ones. */
static void
test_parser(void)
{
	static const uint16_t suites[3] = { 0x1301, 0x1303, 0x1302 };
	static const uint16_t groups[9] = { 0x001d, 0x0017, 0x0018, 0x0019, 0x0100, 0x0101, 0x0102, 0x0103, 0x0104 };
	struct tlspeer_hello hello;
	uint8_t message[1024];
	uint8_t broken[1100];
	uint8_t key[64];
	size_t size;
	size_t cut;
	size_t index;
	size_t refused;

	/* Section 3: the fields of the trace's ClientHello. */
	size = trace_get('3', "{client} construct a ClientHello", "ClientHello", 0, message, sizeof(message));
	trace_get('3', "{client} create an ephemeral x25519", "public key", 0, key, sizeof(key));
	CHECK(tlspeer_parse_client_hello(message, size, &hello) == 0);
	CHECK(hello.legacy_version == 0x0303U);
	CHECK_BYTES(hello.random, message + 6, 32);
	CHECK(hello.session_id_size == 0U);
	CHECK(hello.suite_count == 3U && memcmp(hello.suites, suites, sizeof(suites)) == 0);
	CHECK(hello.has_scsv == 0);
	CHECK(hello.compression_count == 1U && hello.compressions[0] == 0U);
	CHECK(hello.has_server_name == 1 && strcmp(hello.server_name, "server") == 0);
	CHECK(hello.group_count == 9U && memcmp(hello.groups, groups, sizeof(groups)) == 0);
	CHECK(hello.share_count == 1U && hello.shares[0].group == SSLC_GROUP_X25519);
	CHECK(hello.shares[0].key_size == 32U);
	CHECK_BYTES(hello.shares[0].key, key, 32);
	CHECK(hello.version_count == 1U && hello.versions[0] == SSLC_TLS13);
	CHECK(hello.scheme_count == 15U && hello.schemes[0] == 0x0403U);
	CHECK(hello.has_renegotiation_info == 1 && hello.renegotiation_info_size == 0U);
	CHECK(hello.has_cookie == 0);
	CHECK(hello.extension_count == 9U);
	CHECK(hello.extensions[0] == SSLC_EXT_SERVER_NAME);
	CHECK(tlspeer_hello_has(&hello, SSLC_EXT_SESSION_TICKET) == 1);
	CHECK(tlspeer_hello_has(&hello, SSLC_EXT_EXTENDED_MASTER_SECRET) == 0);

	/* Section 5: the second ClientHello carries the cookie and a secp256r1 share. */
	size = trace_get('5', "{client} construct a ClientHello", "ClientHello", 1, message, sizeof(message));
	CHECK(tlspeer_parse_client_hello(message, size, &hello) == 0);
	CHECK(hello.has_cookie == 1 && hello.cookie_size == 0x72U);
	CHECK(hello.share_count == 1U && hello.shares[0].group == SSLC_GROUP_SECP256R1 && hello.shares[0].key_size == 65U);

	/* Every truncation with a consistent length, and every truncation as it is, is refused. */
	size = trace_get('3', "{client} construct a ClientHello", "ClientHello", 0, message, sizeof(message));
	refused = 0;
	for (cut = 0; cut < size; cut++) {
		memcpy(broken, message, cut);
		if (tlspeer_parse_client_hello(broken, cut, &hello) != 0)
			refused++;
		if (cut >= 4U) {
			fix_length(broken, cut);
			if (cut == 4U + 2U + 32U + 1U + 2U + 6U + 2U) {
				/* Cut right after the compression methods: a ClientHello without extensions, valid. */
				CHECK(tlspeer_parse_client_hello(broken, cut, &hello) == 0);
				CHECK(hello.has_extensions == 0);
				refused++;
				continue;
			}
			if (tlspeer_parse_client_hello(broken, cut, &hello) != 0)
				refused++;
		}
	}
	CHECK(refused == size + size - 4U);

	/* A trailing byte, a duplicate extension, a bad type, an odd suite list. */
	memcpy(broken, message, size);
	broken[size] = 0;
	fix_length(broken, size + 1U);
	CHECK(tlspeer_parse_client_hello(broken, size + 1U, &hello) == EINVAL);
	memcpy(broken, message, size);
	broken[0] = SSLC_HS_SERVER_HELLO;
	CHECK(tlspeer_parse_client_hello(broken, size, &hello) == EINVAL);
	memcpy(broken, message, size);
	broken[4U + 2U + 32U + 1U + 1U] = 5;
	CHECK(tlspeer_parse_client_hello(broken, size, &hello) == EINVAL);

	/* A duplicate: the last extension (record_size_limit, 00 1c 00 02 40 01) added twice. */
	memcpy(broken, message, size);
	memcpy(broken + size, message + size - 6U, 6);
	fix_length(broken, size + 6U);
	index = 4U + 2U + 32U + 1U + 2U + 6U + 2U;
	cryc_store_be16(broken + index, cryc_load_be16(message + index) + 6U);
	CHECK(tlspeer_parse_client_hello(broken, size + 6U, &hello) == EINVAL);

	/* The same with a new type parses (the extension block arithmetic is right). */
	broken[size + 1U] = 0x7f;
	CHECK(tlspeer_parse_client_hello(broken, size + 6U, &hello) == 0);
	CHECK(hello.extension_count == 10U && hello.extensions[9] == 0x007fU);
}

/*
 * The client side.
 */

/* Starts the client for suite (the peer's). */
static void
client_init(
	struct client *client,
	uint16_t suite)
{
	/* Plaintext both ways, the transcript in the suite's hash. */
	memset(client, 0, sizeof(*client));
	client->suite = sslc_tls_suite(suite);
	client->tls13 = client->suite->version == SSLC_TLS13;
	client->hash = client->suite->hash;
	client->hash_size = cryc_hash_size(client->hash);
	cryc_hash_init(&client->transcript, client->hash);
	sslc_record_init(&client->read);
	sslc_record_init(&client->write);
}

/* Writes the client's transcript hash. */
static void
client_hash(
	const struct client *client,
	uint8_t *out)
{
	struct cryc_hash copy;

	/* A finished copy. */
	copy = client->transcript;
	cryc_hash_final(&copy, out);
}

/* Seals one record from the client and gives it to the peer; returns the peer's answer. */
static int
client_send(
	struct client *client,
	struct tlspeer *peer,
	unsigned type,
	const uint8_t *data,
	size_t size,
	int first_hello)
{
	static uint8_t record[SSLC_RECORD_BUFFER];
	size_t written;
	size_t index;
	int error;

	/* The record. */
	error = sslc_record_seal(&client->write, type, first_hello, data, size, record, sizeof(record), &written);
	CHECK(error == 0);
	if (error != 0)
		return error;

	/* At once or one byte at a time. */
	if (!feed_bytewise)
		return tlspeer_input(peer, record, written);
	for (index = 0; index < written; index++) {
		error = tlspeer_input(peer, record + index, 1);
		if (error != 0)
			return error;
	}
	return 0;
}

/* Takes what the peer wrote into the client's buffer (one byte per call when bytewise, to test splits). */
static void
client_pull(
	struct client *client,
	struct tlspeer *peer)
{
	size_t taken;

	/* Everything there is, in pieces of 1 or of the whole room. */
	do {
		taken = 0;
		CHECK(tlspeer_take(peer, client->in + client->in_used, feed_bytewise ? 1U : sizeof(client->in) - client->in_used, &taken) == 0);
		client->in_used += taken;
	} while (taken > 0U && client->in_used < sizeof(client->in));
}

/*
 * Reads the next record from the buffer, opens it with the current read
 * keys and files its content; returns 1, or 0 when no whole record waits.
 */
static int
client_record(
	struct client *client)
{
	static uint8_t record[SSLC_RECORD_BUFFER];
	struct sslc_record opened;
	size_t length;
	unsigned type;
	int error;

	/* A whole record. */
	if (client->in_used < SSLC_RECORD_HEADER)
		return 0;
	length = SSLC_RECORD_HEADER + cryc_load_be16(client->in + 3);
	if (client->in_used < length || length > sizeof(record))
		return 0;
	memcpy(record, client->in, length);
	memmove(client->in, client->in + length, client->in_used - length);
	client->in_used -= length;
	client->records++;

	/* Plaintext before keys and CCS; else opened. */
	type = record[0];
	if (client->read.suite == NULL || type == SSLC_CT_CHANGE_CIPHER_SPEC) {
		opened.type = type;
		opened.data = record + SSLC_RECORD_HEADER;
		opened.size = length - SSLC_RECORD_HEADER;
	} else {
		error = sslc_record_open(&client->read, record, length, &opened);
		if (error != 0) {
			client->failed = error;
			return 1;
		}
	}

	/* Filed by type; a TLS 1.2 CCS switches the read keys. */
	switch (opened.type) {
	case SSLC_CT_CHANGE_CIPHER_SPEC:
		CHECK(opened.size == 1U && opened.data[0] == 1U);
		client->ccs++;
		if (!client->tls13 && client->read12.suite != NULL)
			CHECK(sslc_record_set_keys(&client->read, 0, &client->read12) == 0);
		break;
	case SSLC_CT_HANDSHAKE:
		CHECK(opened.size <= sizeof(client->hs) - client->hs_used);
		memcpy(client->hs + client->hs_used, opened.data, opened.size);
		client->hs_used += opened.size;
		break;
	case SSLC_CT_APPLICATION_DATA:
		CHECK(opened.size <= sizeof(client->data) - client->data_size);
		if (opened.size > 0U)
			memcpy(client->data + client->data_size, opened.data, opened.size);
		client->data_size += opened.size;
		break;
	case SSLC_CT_ALERT:
		CHECK(opened.size == 2U);
		client->alerts++;
		client->alert = opened.data[1];
		break;
	default:
		CHECK(0);
		break;
	}
	return 1;
}

/* Gives the next whole handshake message from the peer into out; returns its size, or 0 when none comes. */
static size_t
client_message(
	struct client *client,
	struct tlspeer *peer,
	uint8_t *out,
	size_t room)
{
	size_t length;

	/* Records until a whole message is there. */
	client_pull(client, peer);
	for (;;) {
		if (client->hs_used >= 4U) {
			length = 4U + cryc_load_be24(client->hs + 1);
			if (client->hs_used >= length && length <= room) {
				memcpy(out, client->hs, length);
				memmove(client->hs, client->hs + length, client->hs_used - length);
				client->hs_used -= length;
				return length;
			}
		}
		if (client_record(client) == 0 || client->failed != 0)
			return 0;
	}
}

/* Reads every record that waits (data, alerts); returns how many. */
static unsigned long
client_drain(
	struct client *client,
	struct tlspeer *peer)
{
	unsigned long before;

	/* All of them. */
	before = client->records;
	client_pull(client, peer);
	while (client->failed == 0 && client_record(client) != 0)
		;
	return client->records - before;
}

/* Parses a ServerHello or HelloRetryRequest message (RFC 8446 4.1.3; RFC 5246 7.4.1.3), as the client checks it. */
static int
parse_server_hello(
	const uint8_t *message,
	size_t size,
	struct server_hello *hello)
{
	struct tlspeer_reader reader;
	struct tlspeer_reader body;
	struct tlspeer_reader vector;
	struct tlspeer_reader extensions;
	struct tlspeer_reader data;
	struct tlspeer_reader key;
	uint32_t type;

	/* The header and the fixed fields. */
	memset(hello, 0, sizeof(*hello));
	tlspeer_reader_init(&reader, message, size);
	type = tlspeer_get_int(&reader, 1);
	tlspeer_get_vector(&reader, 3, 38, 0xffffffU, &body);
	if (type != SSLC_HS_SERVER_HELLO || !tlspeer_reader_done(&reader))
		return EINVAL;
	if (tlspeer_get_int(&body, 2) != 0x0303U)
		return EINVAL;
	memcpy(hello->random, tlspeer_get(&body, 32), 32);
	tlspeer_get_vector(&body, 1, 0, 32, &vector);
	if (vector.error != 0)
		return EINVAL;
	if (vector.size > 0U)
		memcpy(hello->session_id, vector.data, vector.size);
	hello->session_id_size = vector.size;
	hello->suite = (uint16_t)tlspeer_get_int(&body, 2);
	if (tlspeer_get_int(&body, 1) != 0U || body.error != 0)
		return EINVAL;
	if (body.position == body.size)
		return 0;

	/* The extensions. */
	tlspeer_get_vector(&body, 2, 0, 65535, &extensions);
	if (!tlspeer_reader_done(&body))
		return EINVAL;
	while (extensions.position < extensions.size) {
		type = tlspeer_get_int(&extensions, 2);
		tlspeer_get_vector(&extensions, 2, 0, 65535, &data);
		if (data.error != 0 || hello->extension_count >= TLSPEER_EXT_MAX)
			return EINVAL;
		hello->extensions[hello->extension_count++] = (uint16_t)type;
		if (type == SSLC_EXT_KEY_SHARE) {
			hello->group = (uint16_t)tlspeer_get_int(&data, 2);
			if (data.position < data.size) {
				tlspeer_get_vector(&data, 2, 1, TLSPEER_SHARE_KEY_MAX, &key);
				if (key.error != 0)
					return EINVAL;
				memcpy(hello->key, key.data, key.size);
				hello->key_size = key.size;
			}
		} else if (type == SSLC_EXT_SUPPORTED_VERSIONS) {
			hello->version = (uint16_t)tlspeer_get_int(&data, 2);
		} else if (type == SSLC_EXT_COOKIE) {
			hello->cookie_size = tlspeer_get_int(&data, 2);
		}
	}
	return 0;
}

/* Returns 1 when the ServerHello has an extension of type. */
static int
hello_has(
	const struct server_hello *hello,
	uint16_t type)
{
	size_t index;

	/* A search. */
	for (index = 0; index < hello->extension_count; index++) {
		if (hello->extensions[index] == type)
			return 1;
	}
	return 0;
}

/* Gives the leaf's public key and curve from a Certificate message (TLS 1.3 when tls13). */
static int
leaf_key(
	const uint8_t *message,
	size_t size,
	int tls13,
	struct cryc_x509 *cert)
{
	struct tlspeer_reader reader;
	struct tlspeer_reader context;
	struct tlspeer_reader list;
	struct tlspeer_reader entry;

	/* The context (1.3), the list, the first entry. */
	tlspeer_reader_init(&reader, message + 4, size - 4U);
	if (tls13)
		tlspeer_get_vector(&reader, 1, 0, 255, &context);
	tlspeer_get_vector(&reader, 3, 1, 0xffffffU, &list);
	tlspeer_get_vector(&list, 3, 1, 0xffffffU, &entry);
	if (entry.error != 0)
		return EINVAL;
	return cryc_x509_parse(cert, entry.data, entry.size);
}

/*
 * Runs the client's half of a TLS 1.3 handshake with the peer, checking
 * everything the client checks; hello2 is the second ClientHello for a
 * HelloRetryRequest (NULL: none expected).  private_key is the client's
 * x25519 scalar or secp256r1 key, for the share its ClientHellos carry.
 * Returns 0 when connected.
 */
static int
client_handshake13(
	struct client *client,
	struct tlspeer *peer,
	const uint8_t *hello1,
	size_t hello1_size,
	const uint8_t *hello2,
	size_t hello2_size,
	const uint8_t *private_key)
{
	static uint8_t message[TLSPEER_MESSAGE_MAX];
	static uint8_t cert_message[TLSPEER_MESSAGE_MAX];
	struct server_hello hello;
	struct sslc_traffic_keys keys;
	struct cryc_x509 cert;
	struct cryc_ecp ec;
	uint8_t digest[CRYC_HASH_MAX_SIZE];
	uint8_t header[4];
	uint8_t shared[48];
	uint8_t content[200];
	uint8_t verify[CRYC_HASH_MAX_SIZE];
	size_t content_size;
	size_t size;
	int has_cert;
	int error;

	/* The first ClientHello. */
	cryc_hash_update(&client->transcript, hello1, hello1_size);
	CHECK(client_send(client, peer, SSLC_CT_HANDSHAKE, hello1, hello1_size, 1) == 0);

	/* A HelloRetryRequest: the transcript restarts (4.4.1), the second ClientHello. */
	size = client_message(client, peer, message, sizeof(message));
	CHECK(size > 0U && parse_server_hello(message, size, &hello) == 0);
	if (memcmp(hello.random, retry_random, 32) == 0) {
		CHECK(hello2 != NULL);
		if (hello2 == NULL)
			return EPROTO;
		client_hash(client, digest);
		cryc_hash_init(&client->transcript, client->hash);
		header[0] = SSLC_HS_MESSAGE_HASH;
		header[1] = 0;
		header[2] = 0;
		header[3] = (uint8_t)client->hash_size;
		cryc_hash_update(&client->transcript, header, 4);
		cryc_hash_update(&client->transcript, digest, client->hash_size);
		cryc_hash_update(&client->transcript, message, size);
		cryc_hash_update(&client->transcript, hello2, hello2_size);
		CHECK(client_send(client, peer, SSLC_CT_HANDSHAKE, hello2, hello2_size, 0) == 0);
		size = client_message(client, peer, message, sizeof(message));
		CHECK(size > 0U && parse_server_hello(message, size, &hello) == 0);
	}

	/* The ServerHello: 1.3, the peer's suite, a share for the group; the shared secret. */
	CHECK(hello.version == SSLC_TLS13);
	CHECK(hello.suite == client->suite->id);
	cryc_hash_update(&client->transcript, message, size);
	if (hello.group == SSLC_GROUP_X25519) {
		CHECK(hello.key_size == 32U);
		error = cryc_x25519(shared, private_key, hello.key);
	} else {
		cryc_ecp_init(&ec, CRYC_CURVE_P256);
		error = cryc_ecc_ecdh(&ec, private_key, hello.key, hello.key_size, shared);
	}
	CHECK(error == 0);
	if (error != 0)
		return error;

	/* The handshake keys; the server's must be the peer's epoch 1. */
	client_hash(client, digest);
	CHECK(sslc_tls13_start(&client->schedule, client->hash) == 0);
	CHECK(sslc_tls13_handshake(&client->schedule, shared, 32, digest) == 0);
	CHECK(sslc_tls13_traffic_keys(client->hash, client->suite, client->schedule.server_handshake, &keys) == 0);
	CHECK_BYTES(keys.key, peer->write_keys[1].key, keys.key_size);
	CHECK_BYTES(keys.iv, peer->write_keys[1].iv, 12);
	CHECK(sslc_record_set_keys(&client->read, 1, &keys) == 0);

	/* EncryptedExtensions, CertificateRequest, Certificate, CertificateVerify, Finished. */
	has_cert = 0;
	memset(&cert, 0, sizeof(cert));
	for (;;) {
		size = client_message(client, peer, message, sizeof(message));
		CHECK(size >= 4U);
		if (size < 4U)
			return EPROTO;
		if (message[0] == SSLC_HS_CERTIFICATE) {
			memcpy(cert_message, message, size);
			CHECK(leaf_key(cert_message, size, 1, &cert) == 0);
			has_cert = 1;
		} else if (message[0] == SSLC_HS_CERTIFICATE_VERIFY) {
			/* Over 64 spaces, the context, 0 and the transcript hash, with the leaf key (4.4.3). */
			CHECK(has_cert);
			memset(content, 0x20, 64);
			memcpy(content + 64, cv_context, sizeof(cv_context));
			content_size = 64U + sizeof(cv_context);
			client_hash(client, content + content_size);
			content_size += client->hash_size;
			CHECK(size >= 8U && cryc_load_be16(message + 4) == peer->config.scheme);
			CHECK(cryc_load_be16(message + 6) == size - 8U);
			error = verify_signature(cert.key.curve, cert.key.point, cert.key.point_size,
			    cert.key.curve == CRYC_CURVE_P384 ? CRYC_HASH_SHA384 : CRYC_HASH_SHA256,
			    content, content_size, message + 8, size - 8U);
			CHECK(error == 0 || (peer->breaks & TLSPEER_BREAK_SIGNATURE) != 0U);
			if (error != 0)
				return EBADMSG;
			check_sigver(cert.key.curve, cert.key.point, cert.key.point_size,
			    cert.key.curve == CRYC_CURVE_P384 ? CRYC_SIG_ECDSA_SHA384 : CRYC_SIG_ECDSA_SHA256,
			    content, content_size, message + 8, size - 8U, 0);
		} else if (message[0] == SSLC_HS_FINISHED) {
			client_hash(client, digest);
			CHECK(sslc_tls13_finished(client->hash, client->schedule.server_handshake, digest, verify) == 0);
			CHECK(size == 4U + client->hash_size);
			if (memcmp(message + 4, verify, client->hash_size) != 0)
				return EBADMSG;
			cryc_hash_update(&client->transcript, message, size);
			break;
		}
		cryc_hash_update(&client->transcript, message, size);
	}

	/* The application secrets; the server's keys must be the peer's epoch 2. */
	client_hash(client, digest);
	CHECK(sslc_tls13_master(&client->schedule, digest) == 0);
	memcpy(client->server_secret, client->schedule.server_application, client->hash_size);
	memcpy(client->client_secret, client->schedule.client_application, client->hash_size);
	CHECK(sslc_tls13_traffic_keys(client->hash, client->suite, client->server_secret, &keys) == 0);
	CHECK_BYTES(keys.key, peer->write_keys[2].key, keys.key_size);

	/* The client's flight under its handshake keys: an empty Certificate when asked, then Finished. */
	CHECK(sslc_tls13_traffic_keys(client->hash, client->suite, client->schedule.client_handshake, &keys) == 0);
	CHECK_BYTES(keys.key, peer->client_handshake_keys.key, keys.key_size);
	CHECK(sslc_record_set_keys(&client->write, 1, &keys) == 0);
	if (peer->request_sent) {
		message[0] = SSLC_HS_CERTIFICATE;
		cryc_store_be24(message + 1, (uint32_t)(1U + peer->config.request_context_size + 3U));
		message[4] = (uint8_t)peer->config.request_context_size;
		if (peer->config.request_context_size > 0U)
			memcpy(message + 5, peer->config.request_context, peer->config.request_context_size);
		cryc_store_be24(message + 5 + peer->config.request_context_size, 0);
		size = 8U + peer->config.request_context_size;
		cryc_hash_update(&client->transcript, message, size);
		CHECK(client_send(client, peer, SSLC_CT_HANDSHAKE, message, size, 0) == 0);
	}
	client_hash(client, digest);
	CHECK(sslc_tls13_finished(client->hash, client->schedule.client_handshake, digest, verify) == 0);
	message[0] = SSLC_HS_FINISHED;
	cryc_store_be24(message + 1, (uint32_t)client->hash_size);
	memcpy(message + 4, verify, client->hash_size);
	cryc_hash_update(&client->transcript, message, 4U + client->hash_size);
	error = client_send(client, peer, SSLC_CT_HANDSHAKE, message, 4U + client->hash_size, 0);
	CHECK(error == 0);
	CHECK(peer->state == TLSPEER_CONNECTED);

	/* Application keys both ways. */
	CHECK(sslc_tls13_traffic_keys(client->hash, client->suite, client->server_secret, &keys) == 0);
	CHECK(sslc_record_set_keys(&client->read, 1, &keys) == 0);
	CHECK(sslc_tls13_traffic_keys(client->hash, client->suite, client->client_secret, &keys) == 0);
	CHECK(sslc_record_set_keys(&client->write, 1, &keys) == 0);
	return error;
}

/* Moves one side's secret to the next generation and installs it on dir. */
static void
client_update(
	struct client *client,
	uint8_t *secret,
	struct sslc_record_dir *dir)
{
	struct sslc_traffic_keys keys;

	/* RFC 8446 7.2. */
	CHECK(sslc_tls13_update_secret(client->hash, secret) == 0);
	CHECK(sslc_tls13_traffic_keys(client->hash, client->suite, secret, &keys) == 0);
	CHECK(sslc_record_set_keys(dir, 1, &keys) == 0);
}

/* Exchanges data both ways, a ticket, and KeyUpdates both ways, after a TLS 1.3 handshake. */
static void
exchange13(
	struct client *client,
	struct tlspeer *peer)
{
	static const uint8_t key_update_requested[5] = { SSLC_HS_KEY_UPDATE, 0, 0, 1, 1 };
	static uint8_t message[TLSPEER_MESSAGE_MAX];
	static const char hello[] = "GET / HTTP/1.1\r\n\r\n";
	static const char answer[] = "HTTP/1.1 200 OK\r\n\r\n";
	size_t size;

	/* Data from the client is opened by the peer. */
	CHECK(client_send(client, peer, SSLC_CT_APPLICATION_DATA, (const uint8_t *)hello, sizeof(hello) - 1U, 0) == 0);
	CHECK(peer->data_size == sizeof(hello) - 1U && memcmp(peer->data, hello, sizeof(hello) - 1U) == 0);

	/* A ticket and data from the peer are opened by the client. */
	CHECK(tlspeer_send_ticket(peer) == 0);
	CHECK(tlspeer_send_data(peer, (const uint8_t *)answer, sizeof(answer) - 1U) == 0);
	size = client_message(client, peer, message, sizeof(message));
	CHECK(size == 4U + 4U + 4U + 2U + 2U + 16U + 2U && message[0] == SSLC_HS_NEW_SESSION_TICKET);
	client_drain(client, peer);
	CHECK(client->data_size == sizeof(answer) - 1U && memcmp(client->data, answer, sizeof(answer) - 1U) == 0);

	/* A KeyUpdate from the client that requests one: the peer answers under its old keys, then moves. */
	CHECK(client_send(client, peer, SSLC_CT_HANDSHAKE, key_update_requested, 5, 0) == 0);
	client_update(client, client->client_secret, &client->write);
	CHECK(tlspeer_send_data(peer, (const uint8_t *)"after", 5) == 0);
	size = client_message(client, peer, message, sizeof(message));
	CHECK(size == 5U && message[0] == SSLC_HS_KEY_UPDATE && message[4] == 0U);
	client_update(client, client->server_secret, &client->read);
	client->data_size = 0;
	client_drain(client, peer);
	CHECK(client->failed == 0);
	CHECK(client->data_size == 5U && memcmp(client->data, "after", 5) == 0);

	/* The client's new keys reach the peer. */
	peer->data_size = 0;
	CHECK(client_send(client, peer, SSLC_CT_APPLICATION_DATA, (const uint8_t *)"new", 3, 0) == 0);
	CHECK(peer->data_size == 3U && memcmp(peer->data, "new", 3) == 0);

	/* A KeyUpdate from the peer that requests one. */
	CHECK(tlspeer_send_key_update(peer, 1) == 0);
	CHECK(tlspeer_send_data(peer, (const uint8_t *)"x", 1) == 0);
	size = client_message(client, peer, message, sizeof(message));
	CHECK(size == 5U && message[4] == 1U);
	client_update(client, client->server_secret, &client->read);
	client->data_size = 0;
	client_drain(client, peer);
	CHECK(client->data_size == 1U && client->data[0] == 'x');

	/* close_notify both ways. */
	CHECK(client_send(client, peer, SSLC_CT_ALERT, (const uint8_t *)"\001\000", 2, 0) == 0);
	CHECK(peer->close_notify == 1 && peer->alert_protected == 1);
	CHECK(tlspeer_send_alert(peer, SSLC_ALERT_WARNING, SSLC_ALERT_CLOSE_NOTIFY) == 0);
	client_drain(client, peer);
	CHECK(client->alerts == 1U && client->alert == SSLC_ALERT_CLOSE_NOTIFY);
	CHECK(peer->error == 0);
}

/*
 * RFC 8448 replays.
 */

/* Takes every byte the peer wrote into out; returns the count. */
static size_t
take_all(
	struct tlspeer *peer,
	uint8_t *out,
	size_t room)
{
	size_t taken;

	/* One call takes all that fits. */
	CHECK(tlspeer_take(peer, out, room, &taken) == 0);
	return taken;
}

/* Replays the server side of RFC 8448 section 3 (1-RTT). */
static void
test_rfc8448_simple(void)
{
	static uint8_t expected[4096];
	static uint8_t got[4096];
	static uint8_t value[4096];
	static uint8_t ee[256];
	static uint8_t cv[512];
	static uint8_t certificate[1024];
	static uint8_t cert_der[1024];
	struct tlspeer_config config;
	struct tlspeer_pki pki;
	struct tlspeer *peer;
	struct tlspeer_item item;
	size_t ee_size;
	size_t cv_size;
	size_t certificate_size;
	size_t size;
	size_t got_size;
	int error;

	/* The trace's server: its random, x25519 key, EncryptedExtensions, RSA certificate and CertificateVerify. */
	CHECK(tlspeer_pki_init(&pki, CRYC_CURVE_P256, "server") == 0);
	CHECK(tlspeer_config_init(&config, SSLC_TLS13, &pki) == 0);
	size = trace_get('3', "{server} construct a ServerHello", "ServerHello", 0, value, sizeof(value));
	CHECK(size > 38U);
	memcpy(config.random, value + 6, 32);
	trace_get('3', "{server} create an ephemeral x25519", "private key", 0, config.x25519_private, 32);
	ee_size = trace_get('3', "{server} construct an EncryptedExtensions", "EncryptedExtensions", 0, ee, sizeof(ee));
	config.ee_raw = ee + 6;
	config.ee_raw_size = ee_size - 6U;
	cv_size = trace_get('3', "{server} construct a CertificateVerify", "CertificateVerify", 0, cv, sizeof(cv));
	config.cv_raw = cv;
	config.cv_raw_size = cv_size;
	certificate_size = trace_get('3', "{server} construct a Certificate", "Certificate", 0, certificate, sizeof(certificate));
	size = cryc_load_be24(certificate + 8);
	CHECK(size + 13U == certificate_size);
	memcpy(cert_der, certificate + 11, size);
	config.chain[0] = cert_der;
	config.chain_size[0] = size;
	error = tlspeer_new(&peer, &config);
	CHECK(error == 0);
	if (error != 0)
		return;

	/* Messages: ServerHello, EncryptedExtensions, Certificate, CertificateVerify, Finished as the trace's. */
	size = trace_get('3', "{client} construct a ClientHello", "ClientHello", 0, value, sizeof(value));
	CHECK(tlspeer_handshake(peer, value, size) == 0);
	size = trace_get('3', "{server} construct a ServerHello", "ServerHello", 0, expected, sizeof(expected));
	CHECK(tlspeer_next(peer, &item) == 1 && item.epoch == 0U && item.kind == TLSPEER_ITEM_HANDSHAKE);
	CHECK(item.size == size);
	CHECK_BYTES(item.data, expected, size);
	CHECK(tlspeer_next(peer, &item) == 1 && item.epoch == 1U && item.size == ee_size);
	CHECK_BYTES(item.data, ee, ee_size);
	CHECK(tlspeer_next(peer, &item) == 1 && item.size == certificate_size);
	CHECK_BYTES(item.data, certificate, certificate_size);
	CHECK(tlspeer_next(peer, &item) == 1 && item.size == cv_size);
	size = trace_get('3', "{server} construct a Finished", "Finished", 0, expected, sizeof(expected));
	CHECK(tlspeer_next(peer, &item) == 1 && item.size == size && item.epoch == 1U);
	CHECK_BYTES(item.data, expected, size);
	CHECK(tlspeer_next(peer, &item) == 0);

	/* The keys as the trace's. */
	size = trace_get('3', "{server} derive write traffic keys for handshake data", "key expanded", 0, expected, sizeof(expected));
	CHECK_BYTES(peer->write_keys[1].key, expected, size);
	size = trace_get('3', "{server} derive write traffic keys for handshake data", "iv expanded", 0, expected, sizeof(expected));
	CHECK_BYTES(peer->write_keys[1].iv, expected, size);
	size = trace_get('3', "{server} derive write traffic keys for application data", "key expanded", 0, expected, sizeof(expected));
	CHECK_BYTES(peer->write_keys[2].key, expected, size);
	size = trace_get('3', "{server} derive read traffic keys for handshake data", "key expanded", 0, expected, sizeof(expected));
	CHECK_BYTES(peer->client_handshake_keys.key, expected, size);
	size = trace_get('3', "{client} derive write traffic keys for application data", "key expanded", 0, expected, sizeof(expected));
	CHECK_BYTES(peer->client_application_keys.key, expected, size);
	tlspeer_free(peer);

	/* Records: the same server, fed the trace's client records, writes the trace's server records. */
	CHECK(tlspeer_new(&peer, &config) == 0);
	size = trace_get('3', "{client} send handshake record", "complete record", 0, value, sizeof(value));
	CHECK(tlspeer_input(peer, value, size) == 0);
	got_size = take_all(peer, got, sizeof(got));
	size = trace_get('3', "{server} send handshake record", "complete record", 0, expected, sizeof(expected));
	CHECK(got_size > size);
	CHECK_BYTES(got, expected, size);
	got_size -= size;
	memmove(got, got + size, got_size);
	size = trace_get('3', "{server} send handshake record", "complete record", 1, expected, sizeof(expected));
	CHECK(got_size == size);
	CHECK_BYTES(got, expected, size);

	/* The client's Finished record connects. */
	size = trace_get('3', "{client} send handshake record", "complete record", 1, value, sizeof(value));
	CHECK(tlspeer_input(peer, value, size) == 0);
	CHECK(peer->state == TLSPEER_CONNECTED);

	/* The trace's NewSessionTicket, sent as a hand-made message, is the trace's record. */
	size = trace_get('3', "{server} construct a NewSessionTicket", "NewSessionTicket", 0, value, sizeof(value));
	CHECK(tlspeer_send_handshake(peer, value, size) == 0);
	got_size = take_all(peer, got, sizeof(got));
	size = trace_get('3', "{server} send handshake record", "complete record", 2, expected, sizeof(expected));
	CHECK(got_size == size);
	CHECK_BYTES(got, expected, size);

	/* Application data both ways. */
	size = trace_get('3', "{client} send application_data record", "complete record", 0, value, sizeof(value));
	CHECK(tlspeer_input(peer, value, size) == 0);
	size = trace_get('3', "{client} send application_data record", "payload", 0, expected, sizeof(expected));
	CHECK(peer->data_size == size);
	CHECK_BYTES(peer->data, expected, size);
	size = trace_get('3', "{server} send application_data record", "payload", 0, value, sizeof(value));
	CHECK(tlspeer_send_data(peer, value, size) == 0);
	got_size = take_all(peer, got, sizeof(got));
	size = trace_get('3', "{server} send application_data record", "complete record", 0, expected, sizeof(expected));
	CHECK(got_size == size);
	CHECK_BYTES(got, expected, size);

	/* The alerts. */
	size = trace_get('3', "{client} send alert record", "complete record", 0, value, sizeof(value));
	CHECK(tlspeer_input(peer, value, size) == 0);
	CHECK(peer->close_notify == 1 && peer->alert_count == 1U && peer->alert_protected == 1);
	CHECK(tlspeer_send_alert(peer, SSLC_ALERT_WARNING, SSLC_ALERT_CLOSE_NOTIFY) == 0);
	got_size = take_all(peer, got, sizeof(got));
	size = trace_get('3', "{server} send alert record", "complete record", 0, expected, sizeof(expected));
	CHECK(got_size == size);
	CHECK_BYTES(got, expected, size);
	CHECK(peer->error == 0);

	/* SAME_RECORD after a KeyUpdate: KeyUpdate (5) and a NewSessionTicket (34) share one record under the old keys. */
	peer->breaks |= TLSPEER_BREAK_SAME_RECORD;
	CHECK(tlspeer_send_key_update(peer, 0) == 0);
	CHECK(tlspeer_send_ticket(peer) == 0);
	got_size = take_all(peer, got, sizeof(got));
	CHECK(got_size == 5U + 5U + 34U + 1U + 16U);
	CHECK(cryc_load_be16(got + 3) == got_size - 5U);
	CHECK(peer->write_epochs == 4U && peer->write_dir_epoch == 2U);
	tlspeer_free(peer);

	/* A client Finished with one bit changed is refused. */
	CHECK(tlspeer_new(&peer, &config) == 0);
	size = trace_get('3', "{client} construct a ClientHello", "ClientHello", 0, value, sizeof(value));
	CHECK(tlspeer_handshake(peer, value, size) == 0);
	size = trace_get('3', "{client} construct a Finished", "Finished", 0, value, sizeof(value));
	value[size - 1U] ^= 0x80U;
	CHECK(tlspeer_handshake(peer, value, size) == EBADMSG);
	CHECK(peer->state == TLSPEER_FAILED && peer->why != NULL);
	CHECK(tlspeer_handshake(peer, value, size) == EBADMSG);
	tlspeer_free(peer);

	/* A client record with one bit changed is refused. */
	CHECK(tlspeer_new(&peer, &config) == 0);
	size = trace_get('3', "{client} send handshake record", "complete record", 0, value, sizeof(value));
	CHECK(tlspeer_input(peer, value, size) == 0);
	size = trace_get('3', "{client} send handshake record", "complete record", 1, value, sizeof(value));
	value[size - 1U] ^= 1U;
	CHECK(tlspeer_input(peer, value, size) == EBADMSG);
	tlspeer_free(peer);
}

/* Replays the server side of RFC 8448 section 5 (HelloRetryRequest, then secp256r1). */
static void
test_rfc8448_retry(void)
{
	static uint8_t expected[4096];
	static uint8_t got[4096];
	static uint8_t value[4096];
	static uint8_t ee[256];
	static uint8_t cv[512];
	static uint8_t certificate[1024];
	static uint8_t cert_der[1024];
	static uint8_t cookie[TLSPEER_COOKIE_MAX];
	struct tlspeer_hello hello;
	struct tlspeer_config config;
	struct tlspeer_pki pki;
	struct tlspeer *peer;
	size_t ee_size;
	size_t size;
	size_t got_size;
	int error;

	/* The trace's server: the cookie (from the second ClientHello), its secp256r1 key and messages. */
	CHECK(tlspeer_pki_init(&pki, CRYC_CURVE_P256, "server") == 0);
	CHECK(tlspeer_config_init(&config, SSLC_TLS13, &pki) == 0);
	size = trace_get('5', "{client} construct a ClientHello", "ClientHello", 1, value, sizeof(value));
	CHECK(tlspeer_parse_client_hello(value, size, &hello) == 0);
	memcpy(cookie, hello.cookie, hello.cookie_size);
	config.cookie = cookie;
	config.cookie_size = hello.cookie_size;
	config.hello_retry = 1;
	config.group = SSLC_GROUP_SECP256R1;
	trace_get('5', "{server} create an ephemeral P-256", "private key", 0, config.p256_private, 32);
	size = trace_get('5', "{server} construct a ServerHello", "ServerHello", 1, value, sizeof(value));
	memcpy(config.random, value + 6, 32);
	ee_size = trace_get('5', "{server} construct an EncryptedExtensions", "EncryptedExtensions", 0, ee, sizeof(ee));
	config.ee_raw = ee + 6;
	config.ee_raw_size = ee_size - 6U;
	config.cv_raw_size = trace_get('5', "{server} construct a CertificateVerify", "CertificateVerify", 0, cv, sizeof(cv));
	config.cv_raw = cv;
	trace_get('5', "{server} construct a Certificate", "Certificate", 0, certificate, sizeof(certificate));
	config.chain_size[0] = cryc_load_be24(certificate + 8);
	memcpy(cert_der, certificate + 11, config.chain_size[0]);
	config.chain[0] = cert_der;
	error = tlspeer_new(&peer, &config);
	CHECK(error == 0);
	if (error != 0)
		return;

	/* The first ClientHello record: the HelloRetryRequest record. */
	size = trace_get('5', "{client} send handshake record", "complete record", 0, value, sizeof(value));
	CHECK(tlspeer_input(peer, value, size) == 0);
	got_size = take_all(peer, got, sizeof(got));
	size = trace_get('5', "{server} send handshake record", "complete record", 0, expected, sizeof(expected));
	CHECK(got_size == size);
	CHECK_BYTES(got, expected, size);
	CHECK(peer->retries == 1U && peer->state == TLSPEER_WAIT_HELLO);

	/* The second: the ServerHello record and the encrypted flight. */
	size = trace_get('5', "{client} send handshake record", "complete record", 1, value, sizeof(value));
	CHECK(tlspeer_input(peer, value, size) == 0);
	got_size = take_all(peer, got, sizeof(got));
	size = trace_get('5', "{server} send handshake record", "complete record", 1, expected, sizeof(expected));
	CHECK(got_size > size);
	CHECK_BYTES(got, expected, size);
	got_size -= size;
	memmove(got, got + size, got_size);
	size = trace_get('5', "{server} send handshake record", "complete record", 2, expected, sizeof(expected));
	CHECK(got_size == size);
	CHECK_BYTES(got, expected, size);

	/* The client's Finished, then the alerts. */
	size = trace_get('5', "{client} send handshake record", "complete record", 2, value, sizeof(value));
	CHECK(tlspeer_input(peer, value, size) == 0);
	CHECK(peer->state == TLSPEER_CONNECTED);
	size = trace_get('5', "{client} send alert record", "complete record", 0, value, sizeof(value));
	CHECK(tlspeer_input(peer, value, size) == 0);
	CHECK(peer->close_notify == 1);
	CHECK(tlspeer_send_alert(peer, SSLC_ALERT_WARNING, SSLC_ALERT_CLOSE_NOTIFY) == 0);
	got_size = take_all(peer, got, sizeof(got));
	size = trace_get('5', "{server} send alert record", "complete record", 0, expected, sizeof(expected));
	CHECK(got_size == size);
	CHECK_BYTES(got, expected, size);
	CHECK(peer->error == 0);
	tlspeer_free(peer);

	/* A second ClientHello without the cookie is refused. */
	CHECK(tlspeer_new(&peer, &config) == 0);
	size = trace_get('5', "{client} construct a ClientHello", "ClientHello", 0, value, sizeof(value));
	CHECK(tlspeer_handshake(peer, value, size) == 0);
	CHECK(tlspeer_handshake(peer, value, size) == EPROTO);
	tlspeer_free(peer);
}

/*
 * Own handshakes.
 */

/* Runs TLS 1.3 handshakes with tlspeer's own certificates: x25519, and secp256r1 after a HelloRetryRequest. */
static void
test_own13(
	enum cryc_curve_id curve,
	uint16_t suite,
	size_t record_size,
	int bytewise,
	uint32_t options)
{
	static struct client client;
	static uint8_t hello1[1024];
	static uint8_t hello2[1024];
	static uint8_t cookie[TLSPEER_COOKIE_MAX];
	struct tlspeer_hello hello;
	struct tlspeer_config config;
	struct tlspeer_pki pki;
	struct tlspeer *peer;
	uint8_t private_key[32];
	size_t size1;
	size_t size2;

	/* x25519 with the trace's section 3 ClientHello (its private key is known). */
	feed_bytewise = bytewise;
	CHECK(tlspeer_pki_init(&pki, curve, "server") == 0);
	CHECK(tlspeer_config_init(&config, SSLC_TLS13, &pki) == 0);
	config.suite = suite;
	config.record_size = record_size;
	config.options = options;
	CHECK(tlspeer_new(&peer, &config) == 0);
	client_init(&client, suite);
	size1 = trace_get('3', "{client} construct a ClientHello", "ClientHello", 0, hello1, sizeof(hello1));
	trace_get('3', "{client} create an ephemeral x25519", "private key", 0, private_key, 32);
	CHECK(client_handshake13(&client, peer, hello1, size1, NULL, 0, private_key) == 0);
	if ((options & TLSPEER_OPT_CCS) != 0U)
		CHECK(client.ccs == 1U);
	exchange13(&client, peer);
	tlspeer_free(peer);

	/* secp256r1 after a HelloRetryRequest with the trace's section 5 ClientHellos. */
	size1 = trace_get('5', "{client} construct a ClientHello", "ClientHello", 0, hello1, sizeof(hello1));
	size2 = trace_get('5', "{client} construct a ClientHello", "ClientHello", 1, hello2, sizeof(hello2));
	trace_get('5', "{client} create an ephemeral P-256", "private key", 0, private_key, 32);
	CHECK(tlspeer_parse_client_hello(hello2, size2, &hello) == 0);
	memcpy(cookie, hello.cookie, hello.cookie_size);
	config.cookie = cookie;
	config.cookie_size = hello.cookie_size;
	config.hello_retry = 1;
	config.group = SSLC_GROUP_SECP256R1;
	CHECK(tlspeer_new(&peer, &config) == 0);
	client_init(&client, suite);
	CHECK(client_handshake13(&client, peer, hello1, size1, hello2, size2, private_key) == 0);
	CHECK(peer->retries == 1U);
	if ((options & TLSPEER_OPT_CCS) != 0U)
		CHECK(client.ccs == 1U);
	exchange13(&client, peer);
	tlspeer_free(peer);
	feed_bytewise = 0;
}

/*
 * Writes a TLS 1.2 ClientHello: random, suite (and the SCSV), groups
 * x25519 and secp256r1, ec_point_formats, signature_algorithms,
 * extended_master_secret when ems, server_name "server", and a session id
 * of 32 bytes when sid.
 */
static size_t
hello12(
	uint8_t *out,
	size_t room,
	uint16_t suite,
	int ems,
	int sid)
{
	struct tlspeer_writer writer;
	size_t index;

	/* legacy_version, random, session id, suites, compression. */
	tlspeer_writer_init(&writer, out, room);
	tlspeer_put_int(&writer, SSLC_HS_CLIENT_HELLO, 1);
	tlspeer_begin(&writer, 3);
	tlspeer_put_int(&writer, SSLC_TLS12, 2);
	for (index = 0; index < 32U; index++)
		tlspeer_put_int(&writer, (uint32_t)(0xa0U + index), 1);
	tlspeer_begin(&writer, 1);
	for (index = 0; sid && index < 32U; index++)
		tlspeer_put_int(&writer, (uint32_t)index, 1);
	tlspeer_end(&writer);
	tlspeer_put_int(&writer, 4, 2);
	tlspeer_put_int(&writer, suite, 2);
	tlspeer_put_int(&writer, SSLC_SUITE_EMPTY_RENEGOTIATION_INFO_SCSV, 2);
	tlspeer_put_int(&writer, 1, 1);
	tlspeer_put_int(&writer, 0, 1);

	/* The extensions. */
	tlspeer_begin(&writer, 2);
	tlspeer_put_int(&writer, SSLC_EXT_SERVER_NAME, 2);
	tlspeer_put_int(&writer, 11, 2);
	tlspeer_put_int(&writer, 9, 2);
	tlspeer_put_int(&writer, 0, 1);
	tlspeer_put_int(&writer, 6, 2);
	tlspeer_put(&writer, (const uint8_t *)"server", 6);
	tlspeer_put_int(&writer, SSLC_EXT_SUPPORTED_GROUPS, 2);
	tlspeer_put_int(&writer, 6, 2);
	tlspeer_put_int(&writer, 4, 2);
	tlspeer_put_int(&writer, SSLC_GROUP_X25519, 2);
	tlspeer_put_int(&writer, SSLC_GROUP_SECP256R1, 2);
	tlspeer_put_int(&writer, SSLC_EXT_EC_POINT_FORMATS, 2);
	tlspeer_put_int(&writer, 2, 2);
	tlspeer_put_int(&writer, 1, 1);
	tlspeer_put_int(&writer, 0, 1);
	tlspeer_put_int(&writer, SSLC_EXT_SIGNATURE_ALGORITHMS, 2);
	tlspeer_put_int(&writer, 6, 2);
	tlspeer_put_int(&writer, 4, 2);
	tlspeer_put_int(&writer, SSLC_SCHEME_ECDSA_SECP256R1_SHA256, 2);
	tlspeer_put_int(&writer, SSLC_SCHEME_ECDSA_SECP384R1_SHA384, 2);
	if (ems)
		tlspeer_put_extension(&writer, SSLC_EXT_EXTENDED_MASTER_SECRET, NULL, 0);
	tlspeer_end(&writer);
	tlspeer_end(&writer);
	CHECK(writer.error == 0);
	return writer.used;
}

/* Runs a TLS 1.2 ECDHE handshake with tlspeer's own certificates, then data and close_notify. */
static void
test_own12(
	enum cryc_curve_id curve,
	uint16_t suite,
	uint16_t group,
	int ems,
	uint32_t options,
	int bytewise)
{
	static struct client client;
	static uint8_t message[TLSPEER_MESSAGE_MAX];
	static uint8_t cert_message[TLSPEER_MESSAGE_MAX];
	static uint8_t hello[1024];
	struct tlspeer_config config;
	struct tlspeer_pki pki;
	struct tlspeer *peer;
	struct tlspeer_key client_key;
	struct server_hello sh;
	struct cryc_x509 cert;
	struct sslc_traffic_keys client_keys;
	uint8_t server_random[32];
	uint8_t client_random[32];
	uint8_t point[CRYC_ECP_MAX_POINT];
	uint8_t shared[32];
	uint8_t content[64U + 4U + CRYC_ECP_MAX_POINT];
	uint8_t digest[CRYC_HASH_MAX_SIZE];
	uint8_t context[CRYC_HASH_MAX_SIZE + 64U];
	uint8_t master[SSLC_TLS12_MASTER];
	uint8_t verify[SSLC_TLS12_VERIFY_DATA];
	uint8_t x25519_private[32];
	size_t point_size;
	size_t params_size;
	size_t context_size;
	size_t size;
	size_t hello_size;
	unsigned seen;
	int used_ems;
	int error;

	/* The peer, the client's ClientHello and key; EMS is used when offered and not refused. */
	used_ems = 0;
	if (ems && (options & TLSPEER_OPT_NO_EMS) == 0U)
		used_ems = 1;
	feed_bytewise = bytewise;
	CHECK(tlspeer_pki_init(&pki, curve, "server") == 0);
	CHECK(tlspeer_config_init(&config, SSLC_TLS12, &pki) == 0);
	config.suite = suite;
	config.group = group;
	config.options = options;
	CHECK(tlspeer_new(&peer, &config) == 0);
	client_init(&client, suite);
	hello_size = hello12(hello, sizeof(hello), suite, ems, 0);
	memcpy(client_random, hello + 6, 32);
	memset(x25519_private, 0x5a, 32);
	CHECK(tlspeer_key_from_label(&client_key, CRYC_CURVE_P256, "client") == 0);
	cryc_hash_update(&client.transcript, hello, hello_size);
	CHECK(client_send(&client, peer, SSLC_CT_HANDSHAKE, hello, hello_size, 1) == 0);

	/* ServerHello .. ServerHelloDone. */
	seen = 0;
	point_size = 0;
	params_size = 0;
	memset(&cert, 0, sizeof(cert));
	for (;;) {
		size = client_message(&client, peer, message, sizeof(message));
		CHECK(size >= 4U);
		if (size < 4U)
			break;
		cryc_hash_update(&client.transcript, message, size);
		seen |= 1U << message[0];
		if (message[0] == SSLC_HS_SERVER_HELLO) {
			CHECK(parse_server_hello(message, size, &sh) == 0);
			memcpy(server_random, sh.random, 32);
			CHECK(sh.suite == suite);
			CHECK(sh.session_id_size == 0U);
			CHECK(hello_has(&sh, SSLC_EXT_RENEGOTIATION_INFO) == ((options & TLSPEER_OPT_NO_RI) == 0U));
			CHECK(hello_has(&sh, SSLC_EXT_EXTENDED_MASTER_SECRET) == (ems && (options & TLSPEER_OPT_NO_EMS) == 0U));
			CHECK(hello_has(&sh, SSLC_EXT_EC_POINT_FORMATS) == ((options & TLSPEER_OPT_POINT_FORMATS) != 0U));
			CHECK(hello_has(&sh, SSLC_EXT_SERVER_NAME) == ((options & TLSPEER_OPT_SNI_ACK) != 0U));
			CHECK(memcmp(sh.random + 24, downgrade_mark, 7) != 0);
		} else if (message[0] == SSLC_HS_CERTIFICATE) {
			memcpy(cert_message, message, size);
			CHECK(leaf_key(cert_message, size, 0, &cert) == 0);
		} else if (message[0] == SSLC_HS_SERVER_KEY_EXCHANGE) {
			/* named_curve, the group, the point; the signature over the randoms and the params. */
			CHECK(message[4] == 3U && cryc_load_be16(message + 5) == group);
			point_size = message[7];
			memcpy(point, message + 8, point_size);
			params_size = 4U + point_size;
			memcpy(content, client_random, 32);
			memcpy(content + 32, server_random, 32);
			memcpy(content + 64, message + 4, params_size);
			CHECK(cryc_load_be16(message + 4 + params_size) == config.scheme);
			CHECK(cryc_load_be16(message + 6 + params_size) == size - 8U - params_size);
			error = verify_signature(cert.key.curve, cert.key.point, cert.key.point_size,
			    curve == CRYC_CURVE_P384 ? CRYC_HASH_SHA384 : CRYC_HASH_SHA256,
			    content, 64U + params_size, message + 8 + params_size, size - 8U - params_size);
			CHECK(error == 0);
		} else if (message[0] == SSLC_HS_SERVER_HELLO_DONE) {
			CHECK(size == 4U);
			break;
		}
	}
	CHECK(seen == ((1U << SSLC_HS_SERVER_HELLO) | (1U << SSLC_HS_CERTIFICATE) | (1U << SSLC_HS_SERVER_KEY_EXCHANGE) |
	    (1U << SSLC_HS_SERVER_HELLO_DONE) | ((options & TLSPEER_OPT_CERT_REQUEST) != 0U ? 1U << SSLC_HS_CERTIFICATE_REQUEST : 0U)));

	/* An empty Certificate when asked; ClientKeyExchange with the client's point; the shared secret. */
	if ((options & TLSPEER_OPT_CERT_REQUEST) != 0U) {
		message[0] = SSLC_HS_CERTIFICATE;
		cryc_store_be24(message + 1, 3);
		cryc_store_be24(message + 4, 0);
		cryc_hash_update(&client.transcript, message, 7);
		CHECK(client_send(&client, peer, SSLC_CT_HANDSHAKE, message, 7, 0) == 0);
	}
	message[0] = SSLC_HS_CLIENT_KEY_EXCHANGE;
	if (group == SSLC_GROUP_X25519) {
		cryc_store_be24(message + 1, 33);
		message[4] = 32;
		cryc_x25519_public(message + 5, x25519_private);
		CHECK(cryc_x25519(shared, x25519_private, point) == 0);
		size = 37;
	} else {
		cryc_store_be24(message + 1, 66);
		message[4] = 65;
		memcpy(message + 5, client_key.public_key, 65);
		CHECK(cryc_ecc_ecdh(&client_key.ec, client_key.private_key, point, point_size, shared) == 0);
		size = 70;
	}
	cryc_hash_update(&client.transcript, message, size);
	CHECK(client_send(&client, peer, SSLC_CT_HANDSHAKE, message, size, 0) == 0);

	/* The master secret and the keys; they must be the peer's. */
	if (used_ems) {
		client_hash(&client, context);
		context_size = client.hash_size;
	} else {
		memcpy(context, client_random, 32);
		memcpy(context + 32, server_random, 32);
		context_size = 64;
	}
	CHECK(sslc_tls12_master(client.hash, used_ems, shared, 32, context, context_size, master) == 0);
	CHECK(peer->ems == used_ems);
	CHECK(sslc_tls12_key_block(client.suite, master, client_random, server_random, &client_keys, &client.read12) == 0);
	CHECK_BYTES(client_keys.key, peer->client_application_keys.key, client_keys.key_size);

	/* change_cipher_spec, then Finished under the client's keys. */
	CHECK(client_send(&client, peer, SSLC_CT_CHANGE_CIPHER_SPEC, (const uint8_t *)"\001", 1, 0) == 0);
	CHECK(sslc_record_set_keys(&client.write, 0, &client_keys) == 0);
	client_hash(&client, digest);
	CHECK(sslc_tls12_finished(client.hash, master, 1, digest, verify) == 0);
	message[0] = SSLC_HS_FINISHED;
	cryc_store_be24(message + 1, SSLC_TLS12_VERIFY_DATA);
	memcpy(message + 4, verify, SSLC_TLS12_VERIFY_DATA);
	cryc_hash_update(&client.transcript, message, 4U + SSLC_TLS12_VERIFY_DATA);
	CHECK(client_send(&client, peer, SSLC_CT_HANDSHAKE, message, 4U + SSLC_TLS12_VERIFY_DATA, 0) == 0);
	CHECK(peer->state == TLSPEER_CONNECTED);

	/* The server's CCS and Finished. */
	client_hash(&client, digest);
	size = client_message(&client, peer, message, sizeof(message));
	CHECK(client.ccs == 1U);
	CHECK(size == 4U + SSLC_TLS12_VERIFY_DATA && message[0] == SSLC_HS_FINISHED);
	CHECK(sslc_tls12_finished(client.hash, master, 0, digest, verify) == 0);
	CHECK_BYTES(message + 4, verify, SSLC_TLS12_VERIFY_DATA);

	/* Data both ways, close_notify. */
	CHECK(client_send(&client, peer, SSLC_CT_APPLICATION_DATA, (const uint8_t *)"ping", 4, 0) == 0);
	CHECK(peer->data_size == 4U && memcmp(peer->data, "ping", 4) == 0);
	CHECK(tlspeer_send_data(peer, (const uint8_t *)"pong", 4) == 0);
	CHECK(tlspeer_send_alert(peer, SSLC_ALERT_WARNING, SSLC_ALERT_CLOSE_NOTIFY) == 0);
	client_drain(&client, peer);
	CHECK(client.failed == 0);
	CHECK(client.data_size == 4U && memcmp(client.data, "pong", 4) == 0);
	CHECK(client.alerts == 1U);
	CHECK(client_send(&client, peer, SSLC_CT_ALERT, (const uint8_t *)"\001\000", 2, 0) == 0);
	CHECK(peer->close_notify == 1 && peer->alert_protected == 1);
	CHECK(peer->error == 0);
	tlspeer_free(peer);
	feed_bytewise = 0;
}

/*
 * The breaks and options, on the message interface.
 */

/* Makes a TLS 1.3 peer over pki with breaks and options and feeds it the trace's section 3 ClientHello. */
static struct tlspeer *
peer13(
	const struct tlspeer_pki *pki,
	uint32_t breaks,
	uint32_t options,
	uint16_t group)
{
	static uint8_t hello[1024];
	struct tlspeer_config config;
	struct tlspeer *peer;
	size_t size;

	/* The peer and its answer to the ClientHello. */
	tlspeer_config_init(&config, SSLC_TLS13, pki);
	config.breaks = breaks;
	config.options = options;
	config.group = group;
	config.extra_type = 0x7a7a;
	config.extra_data = (const uint8_t *)"xy";
	config.extra_size = 2;
	if (tlspeer_new(&peer, &config) != 0) {
		CHECK(0);
		return NULL;
	}
	size = trace_get('3', "{client} construct a ClientHello", "ClientHello", 0, hello, sizeof(hello));
	CHECK(tlspeer_handshake(peer, hello, size) == 0);
	return peer;
}

/* Collects the items of a peer into items (at most room); returns the count. */
static size_t
collect(
	struct tlspeer *peer,
	struct tlspeer_item *items,
	size_t room)
{
	size_t count;

	/* All of them. */
	count = 0;
	while (count < room && tlspeer_next(peer, &items[count]))
		count++;
	return count;
}

/* Returns 1 when message (of size bytes) contains the 6 bytes of the extra extension 7a 7a 00 02 'x' 'y'. */
static int
has_extra(
	const uint8_t *message,
	size_t size)
{
	static const uint8_t extra[6] = { 0x7a, 0x7a, 0x00, 0x02, 'x', 'y' };
	size_t index;

	/* A search. */
	for (index = 0; index + 6U <= size; index++) {
		if (memcmp(message + index, extra, 6) == 0)
			return 1;
	}
	return 0;
}

/* Checks each break and option. */
static void
test_breaks(void)
{
	static uint8_t first_flight[8192];
	static uint8_t got[16384];
	static uint8_t reference[16384];
	static uint8_t hello[1024];
	static uint8_t hello2[1024];
	struct tlspeer_item normal[16];
	struct tlspeer_item broken[16];
	struct tlspeer_pki pki;
	struct tlspeer_config config;
	struct tlspeer *peer;
	struct tlspeer *other;
	struct server_hello sh;
	struct tlspeer_hello parsed;
	struct cryc_x509 cert;
	struct sslc_record_dir dir;
	struct sslc_record opened;
	struct cryc_hash transcript;
	uint8_t content[200];
	size_t content_size;
	size_t normal_count;
	size_t broken_count;
	size_t size;
	size_t got_size;
	size_t first_size;
	size_t index;

	/* The reference flight: SH, EE, Certificate, CertificateVerify, Finished. */
	CHECK(tlspeer_pki_init(&pki, CRYC_CURVE_P256, "server") == 0);
	peer = peer13(&pki, 0, 0, SSLC_GROUP_X25519);
	normal_count = collect(peer, normal, 16);
	CHECK(normal_count == 5U);
	first_size = 0;
	for (index = 0; index < normal_count; index++) {
		memcpy(first_flight + first_size, normal[index].data, normal[index].size);
		normal[index].data = first_flight + first_size;
		first_size += normal[index].size;
	}
	CHECK(has_extra(first_flight, first_size) == 0);
	tlspeer_free(peer);

	/* FINISHED: only the Finished's first byte differs. */
	peer = peer13(&pki, TLSPEER_BREAK_FINISHED, 0, SSLC_GROUP_X25519);
	broken_count = collect(peer, broken, 16);
	CHECK(broken_count == 5U);
	for (index = 0; index < 4U; index++)
		CHECK(broken[index].size == normal[index].size && memcmp(broken[index].data, normal[index].data, normal[index].size) == 0);
	CHECK(broken[4].size == normal[4].size && broken[4].data[4] == (normal[4].data[4] ^ 1U));
	CHECK(memcmp(broken[4].data + 5, normal[4].data + 5, normal[4].size - 5U) == 0);
	tlspeer_free(peer);

	/* SIGNATURE: the CertificateVerify is well formed and does not verify. */
	peer = peer13(&pki, TLSPEER_BREAK_SIGNATURE, 0, SSLC_GROUP_X25519);
	broken_count = collect(peer, broken, 16);
	CHECK(broken_count == 5U);
	CHECK(leaf_key(broken[2].data, broken[2].size, 1, &cert) == 0);
	memset(content, 0x20, 64);
	memcpy(content + 64, cv_context, sizeof(cv_context));
	content_size = 64U + sizeof(cv_context);
	cryc_hash_init(&transcript, CRYC_HASH_SHA256);
	size = trace_get('3', "{client} construct a ClientHello", "ClientHello", 0, hello, sizeof(hello));
	cryc_hash_update(&transcript, hello, size);
	for (index = 0; index < 3U; index++)
		cryc_hash_update(&transcript, broken[index].data, broken[index].size);
	cryc_hash_final(&transcript, content + content_size);
	content_size += 32U;
	CHECK(verify_signature(CRYC_CURVE_P256, cert.key.point, cert.key.point_size, CRYC_HASH_SHA256, content, content_size, broken[3].data + 8, broken[3].size - 8U) == EBADMSG);
	content[content_size - 1U] ^= 1U;
	CHECK(verify_signature(CRYC_CURVE_P256, cert.key.point, cert.key.point_size, CRYC_HASH_SHA256, content, content_size, broken[3].data + 8, broken[3].size - 8U) == 0);
	tlspeer_free(peer);

	/* RECORD_TAG: the flight's protected record differs in its last byte and does not open. */
	peer = peer13(&pki, 0, 0, SSLC_GROUP_X25519);
	other = peer13(&pki, TLSPEER_BREAK_RECORD_TAG, 0, SSLC_GROUP_X25519);
	size = take_all(peer, reference, sizeof(reference));
	got_size = take_all(other, got, sizeof(got));
	CHECK(size == got_size && size > 0U);
	CHECK(got[got_size - 1U] == (reference[size - 1U] ^ 1U));
	CHECK(memcmp(got, reference, size - 1U) == 0);
	CHECK((other->breaks & TLSPEER_BREAK_RECORD_TAG) == 0U);
	sslc_record_init(&dir);
	CHECK(sslc_record_set_keys(&dir, 1, &peer->write_keys[1]) == 0);
	index = SSLC_RECORD_HEADER + cryc_load_be16(got + 3);
	CHECK(sslc_record_open(&dir, got + index, got_size - index, &opened) == EBADMSG);
	sslc_record_clear(&dir);
	tlspeer_free(peer);
	tlspeer_free(other);

	/* SAME_RECORD: the first record carries ServerHello and EncryptedExtensions in plaintext. */
	peer = peer13(&pki, TLSPEER_BREAK_SAME_RECORD, 0, SSLC_GROUP_X25519);
	got_size = take_all(peer, got, sizeof(got));
	CHECK(got[0] == SSLC_CT_HANDSHAKE && cryc_load_be16(got + 3) == normal[0].size + normal[1].size);
	CHECK(memcmp(got + 5, normal[0].data, normal[0].size) == 0);
	CHECK(memcmp(got + 5 + normal[0].size, normal[1].data, normal[1].size) == 0);
	CHECK((peer->breaks & TLSPEER_BREAK_SAME_RECORD) == 0U);
	tlspeer_free(peer);

	/* SESSION_ID (the client's is empty): a one-byte id 01. CERT_CONTEXT: a one-byte context. */
	peer = peer13(&pki, TLSPEER_BREAK_SESSION_ID | TLSPEER_BREAK_CERT_CONTEXT, 0, SSLC_GROUP_X25519);
	broken_count = collect(peer, broken, 16);
	CHECK(parse_server_hello(broken[0].data, broken[0].size, &sh) == 0);
	CHECK(sh.session_id_size == 1U && sh.session_id[0] == 1U);
	CHECK(broken[2].data[0] == SSLC_HS_CERTIFICATE && broken[2].data[4] == 1U && broken[2].data[5] == 1U);
	tlspeer_free(peer);

	/* A 32-byte session id is echoed, and with SESSION_ID echoed with its first byte flipped. */
	size = trace_get('3', "{client} construct a ClientHello", "ClientHello", 0, hello, sizeof(hello));
	memcpy(hello2, hello, 38);
	hello2[38] = 32;
	for (index = 0; index < 32U; index++)
		hello2[39U + index] = (uint8_t)(0x40U + index);
	memcpy(hello2 + 71, hello + 39, size - 39U);
	fix_length(hello2, size + 32U);
	tlspeer_config_init(&config, SSLC_TLS13, &pki);
	CHECK(tlspeer_new(&peer, &config) == 0);
	CHECK(tlspeer_handshake(peer, hello2, size + 32U) == 0);
	CHECK(tlspeer_next(peer, &broken[0]) == 1);
	CHECK(parse_server_hello(broken[0].data, broken[0].size, &sh) == 0);
	CHECK(sh.session_id_size == 32U && memcmp(sh.session_id, hello2 + 39, 32) == 0);
	tlspeer_free(peer);
	config.breaks = TLSPEER_BREAK_SESSION_ID;
	CHECK(tlspeer_new(&peer, &config) == 0);
	CHECK(tlspeer_handshake(peer, hello2, size + 32U) == 0);
	CHECK(tlspeer_next(peer, &broken[0]) == 1);
	CHECK(parse_server_hello(broken[0].data, broken[0].size, &sh) == 0);
	CHECK(sh.session_id_size == 32U && sh.session_id[0] == (hello2[39] ^ 1U) && memcmp(sh.session_id + 1, hello2 + 40, 31) == 0);
	tlspeer_free(peer);

	/* UNSHARED: a secp256r1 ServerHello without the client's share, and the script ends. */
	peer = peer13(&pki, TLSPEER_BREAK_UNSHARED, 0, SSLC_GROUP_SECP256R1);
	broken_count = collect(peer, broken, 16);
	CHECK(broken_count == 1U && peer->state == TLSPEER_STOPPED);
	CHECK(parse_server_hello(broken[0].data, broken[0].size, &sh) == 0);
	CHECK(sh.group == SSLC_GROUP_SECP256R1 && sh.key_size == 65U);
	CHECK(tlspeer_handshake(peer, hello, 4) == EPROTO);
	tlspeer_free(peer);

	/* Without the break, the missing share is the peer's failure. */
	tlspeer_config_init(&config, SSLC_TLS13, &pki);
	config.group = SSLC_GROUP_SECP256R1;
	CHECK(tlspeer_new(&peer, &config) == 0);
	size = trace_get('3', "{client} construct a ClientHello", "ClientHello", 0, hello, sizeof(hello));
	CHECK(tlspeer_handshake(peer, hello, size) == ENOENT);
	tlspeer_free(peer);

	/* ZERO_SHARE: u = 0, the script ends. */
	peer = peer13(&pki, TLSPEER_BREAK_ZERO_SHARE, 0, SSLC_GROUP_X25519);
	broken_count = collect(peer, broken, 16);
	CHECK(broken_count == 1U && peer->state == TLSPEER_STOPPED);
	CHECK(parse_server_hello(broken[0].data, broken[0].size, &sh) == 0);
	memset(content, 0, 32);
	CHECK(sh.key_size == 32U && memcmp(sh.key, content, 32) == 0);
	tlspeer_free(peer);

	/* The extra extension where asked, and SNI_ACK, CCS and CERT_REQUEST. */
	tlspeer_config_init(&config, SSLC_TLS13, &pki);
	config.extra_type = 0x7a7a;
	config.extra_data = (const uint8_t *)"xy";
	config.extra_size = 2;
	config.extra_at = TLSPEER_AT_SERVER_HELLO | TLSPEER_AT_ENCRYPTED_EXTENSIONS | TLSPEER_AT_CERT_REQUEST | TLSPEER_AT_CERT_ENTRY;
	config.options = TLSPEER_OPT_SNI_ACK | TLSPEER_OPT_CCS | TLSPEER_OPT_CERT_REQUEST;
	config.request_context = (const uint8_t *)"\007";
	config.request_context_size = 1;
	CHECK(tlspeer_new(&peer, &config) == 0);
	CHECK(tlspeer_handshake(peer, hello, size) == 0);
	broken_count = collect(peer, broken, 16);
	CHECK(broken_count == 7U);
	CHECK(broken[0].data[0] == SSLC_HS_SERVER_HELLO && has_extra(broken[0].data, broken[0].size));
	CHECK(broken[1].kind == TLSPEER_ITEM_CCS && broken[1].epoch == 0U && broken[1].size == 1U);
	CHECK(broken[2].data[0] == SSLC_HS_ENCRYPTED_EXTENSIONS && has_extra(broken[2].data, broken[2].size));
	CHECK(broken[2].size == 4U + 2U + 4U + 6U && cryc_load_be16(broken[2].data + 6) == SSLC_EXT_SERVER_NAME);
	CHECK(broken[3].data[0] == SSLC_HS_CERTIFICATE_REQUEST && broken[3].data[4] == 1U && broken[3].data[5] == 7U);
	CHECK(has_extra(broken[3].data, broken[3].size));
	CHECK(broken[4].data[0] == SSLC_HS_CERTIFICATE && has_extra(broken[4].data, broken[4].size));
	CHECK(peer->state == TLSPEER_WAIT_CERTIFICATE);

	/* A non-empty or wrong-context client Certificate is refused. */
	memcpy(hello2, "\013\000\000\005\001\007\000\000\000", 9);
	CHECK(tlspeer_handshake(peer, hello2, 9) == 0);
	CHECK(peer->state == TLSPEER_WAIT_FINISHED);
	tlspeer_free(peer);
	CHECK(tlspeer_new(&peer, &config) == 0);
	CHECK(tlspeer_handshake(peer, hello, size) == 0);
	memcpy(hello2, "\013\000\000\005\001\010\000\000\000", 9);
	CHECK(tlspeer_handshake(peer, hello2, 9) == EPROTO);
	tlspeer_free(peer);
	CHECK(tlspeer_new(&peer, &config) == 0);
	CHECK(tlspeer_handshake(peer, hello, size) == 0);
	memcpy(hello2, "\013\000\000\011\001\007\000\000\004\000\000\001\060", 13);
	CHECK(tlspeer_handshake(peer, hello2, 13) == EPROTO);
	tlspeer_free(peer);

	/* SECOND_HRR and RETRY_NO_GROUP with the section 5 ClientHellos. */
	tlspeer_config_init(&config, SSLC_TLS13, &pki);
	config.hello_retry = 1;
	config.group = SSLC_GROUP_SECP256R1;
	config.breaks = TLSPEER_BREAK_SECOND_HRR;
	config.options = TLSPEER_OPT_RETRY_NO_GROUP;
	config.cookie = (const uint8_t *)"cookie";
	config.cookie_size = 6;
	config.extra_at = TLSPEER_AT_RETRY;
	config.extra_type = 0x7a7a;
	config.extra_data = (const uint8_t *)"xy";
	config.extra_size = 2;
	CHECK(tlspeer_new(&peer, &config) == 0);
	size = trace_get('5', "{client} construct a ClientHello", "ClientHello", 0, hello, sizeof(hello));
	CHECK(tlspeer_handshake(peer, hello, size) == 0);
	broken_count = collect(peer, broken, 16);
	CHECK(broken_count == 1U);
	CHECK(parse_server_hello(broken[0].data, broken[0].size, &sh) == 0);
	CHECK(memcmp(sh.random, retry_random, 32) == 0);
	CHECK(hello_has(&sh, SSLC_EXT_KEY_SHARE) == 0 && sh.cookie_size == 6U && sh.version == SSLC_TLS13);
	CHECK(has_extra(broken[0].data, broken[0].size));
	size = trace_get('5', "{client} construct a ClientHello", "ClientHello", 1, hello2, sizeof(hello2));
	CHECK(tlspeer_handshake(peer, hello2, size) == 0);
	broken_count = collect(peer, broken, 16);
	CHECK(broken_count == 1U && peer->retries == 2U && peer->state == TLSPEER_STOPPED);
	CHECK(parse_server_hello(broken[0].data, broken[0].size, &sh) == 0);
	CHECK(memcmp(sh.random, retry_random, 32) == 0);
	tlspeer_free(peer);

	/* TLS 1.2 DOWNGRADE12, DOWNGRADE11, SESSION_ID (echoed) on the 1.2 ServerHello. */
	tlspeer_config_init(&config, SSLC_TLS12, &pki);
	config.breaks = TLSPEER_BREAK_DOWNGRADE12 | TLSPEER_BREAK_SESSION_ID;
	CHECK(tlspeer_new(&peer, &config) == 0);
	size = hello12(hello, sizeof(hello), SSLC_SUITE_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, 1, 1);
	CHECK(tlspeer_parse_client_hello(hello, size, &parsed) == 0);
	CHECK(parsed.session_id_size == 32U && parsed.has_scsv == 1 && parsed.has_extended_master_secret == 1);
	CHECK(tlspeer_handshake(peer, hello, size) == 0);
	broken_count = collect(peer, broken, 16);
	CHECK(broken_count == 4U);
	CHECK(parse_server_hello(broken[0].data, broken[0].size, &sh) == 0);
	CHECK(memcmp(sh.random + 24, downgrade_mark, 8) == 0);
	CHECK(sh.session_id_size == 32U && memcmp(sh.session_id, parsed.session_id, 32) == 0);
	tlspeer_free(peer);
	config.breaks = TLSPEER_BREAK_DOWNGRADE11 | TLSPEER_BREAK_SIGNATURE;
	config.options = TLSPEER_OPT_NO_RI | TLSPEER_OPT_NO_EMS;
	CHECK(tlspeer_new(&peer, &config) == 0);
	CHECK(tlspeer_handshake(peer, hello, size) == 0);
	broken_count = collect(peer, broken, 16);
	CHECK(parse_server_hello(broken[0].data, broken[0].size, &sh) == 0);
	CHECK(memcmp(sh.random + 24, downgrade_mark, 7) == 0 && sh.random[31] == 0U);
	CHECK(sh.extension_count == 0U && broken[0].size == 4U + 2U + 32U + 1U + 2U + 1U);
	CHECK(peer->ems == 0);

	/* The broken ServerKeyExchange signature does not verify. */
	CHECK(leaf_key(broken[1].data, broken[1].size, 0, &cert) == 0);
	memcpy(content, parsed.random, 32);
	memcpy(content + 32, sh.random, 32);
	memcpy(content + 64, broken[2].data + 4, 36);
	CHECK(verify_signature(CRYC_CURVE_P256, cert.key.point, cert.key.point_size, CRYC_HASH_SHA256, content, 100, broken[2].data + 44, broken[2].size - 44U) == EBADMSG);
	content[99] ^= 1U;
	CHECK(verify_signature(CRYC_CURVE_P256, cert.key.point, cert.key.point_size, CRYC_HASH_SHA256, content, 100, broken[2].data + 44, broken[2].size - 44U) == 0);

	/* Out of order: a CCS before the ClientKeyExchange. */
	CHECK(tlspeer_change_cipher_spec(peer) == EPROTO);
	tlspeer_free(peer);

	/* ZERO_SHARE in 1.2: the ServerKeyExchange carries u = 0, signed. */
	config.breaks = TLSPEER_BREAK_ZERO_SHARE;
	config.options = 0;
	CHECK(tlspeer_new(&peer, &config) == 0);
	CHECK(tlspeer_handshake(peer, hello, size) == 0);
	broken_count = collect(peer, broken, 16);
	CHECK(broken_count == 4U && broken[2].data[7] == 32U);
	memset(content, 0, 32);
	CHECK(memcmp(broken[2].data + 8, content, 32) == 0);
	tlspeer_free(peer);

	/* Hand-made sends: a raw record, raw bytes, a HelloRequest, after a plaintext ServerHello. */
	tlspeer_config_init(&config, SSLC_TLS12, &pki);
	CHECK(tlspeer_new(&peer, &config) == 0);
	CHECK(tlspeer_send_record(peer, SSLC_CT_APPLICATION_DATA, NULL, 0) == 0);
	CHECK(tlspeer_send_bytes(peer, (const uint8_t *)"\026\003\003\000\000", 5) == 0);
	CHECK(tlspeer_send_handshake(peer, (const uint8_t *)"\000\000\000\000", 4) == 0);
	got_size = take_all(peer, got, sizeof(got));
	CHECK(got_size == 5U + 5U + 9U);
	CHECK(memcmp(got, "\027\003\003\000\000\026\003\003\000\000\026\003\003\000\004\000\000\000\000", 19) == 0);
	CHECK(peer->state == TLSPEER_WAIT_HELLO);

	/* A message after the script fails, and garbage. */
	CHECK(tlspeer_handshake(peer, (const uint8_t *)"\001\000\000\001", 4) == EINVAL);
	CHECK(peer->state == TLSPEER_FAILED);
	CHECK(tlspeer_input(peer, (const uint8_t *)"x", 1) == EINVAL);
	tlspeer_free(peer);

	/* A config out of range. */
	tlspeer_config_init(&config, SSLC_TLS13, &pki);
	config.group = 0x0019;
	CHECK(tlspeer_new(&peer, &config) == EINVAL && peer == NULL);
	config.group = SSLC_GROUP_X25519;
	config.suite = 0x1234;
	CHECK(tlspeer_new(&peer, &config) == EINVAL);
	config.suite = SSLC_SUITE_AES_128_GCM_SHA256;
	config.version = 0x0302;
	CHECK(tlspeer_new(&peer, &config) == EINVAL);
	tlspeer_free(NULL);
}

/* Runs every check. */
int
main(void)
{
	/* The trace first: most checks use it. */
	if (trace_load() != 0) {
		printf("test-tlspeer: FAIL (no rfc8448 vectors)\n");
		return EXIT_FAILURE;
	}

	/* The parts, the replays, the own handshakes, the breaks. */
	test_signer();
	test_pki();
	test_parser();
	test_rfc8448_simple();
	test_rfc8448_retry();
	test_own13(CRYC_CURVE_P256, SSLC_SUITE_AES_128_GCM_SHA256, 0, 0, 0);
	test_own13(CRYC_CURVE_P384, SSLC_SUITE_AES_256_GCM_SHA384, 0, 0, TLSPEER_OPT_CCS);
	test_own13(CRYC_CURVE_P256, SSLC_SUITE_CHACHA20_POLY1305_SHA256, 1, 1, TLSPEER_OPT_SNI_ACK);
	test_own13(CRYC_CURVE_P256, SSLC_SUITE_AES_128_GCM_SHA256, 7, 0, TLSPEER_OPT_CERT_REQUEST);
	test_own12(CRYC_CURVE_P256, SSLC_SUITE_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, SSLC_GROUP_X25519, 1, 0, 0);
	test_own12(CRYC_CURVE_P256, SSLC_SUITE_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256, SSLC_GROUP_SECP256R1, 0, TLSPEER_OPT_POINT_FORMATS | TLSPEER_OPT_SNI_ACK, 1);
	test_own12(CRYC_CURVE_P384, SSLC_SUITE_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384, SSLC_GROUP_X25519, 1, TLSPEER_OPT_CERT_REQUEST | TLSPEER_OPT_NO_EMS, 0);
	test_own12(CRYC_CURVE_P256, SSLC_SUITE_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, SSLC_GROUP_SECP256R1, 1, TLSPEER_OPT_NO_RI, 0);
	test_breaks();
	trace_free();
	return check_finish("test-tlspeer");
}
