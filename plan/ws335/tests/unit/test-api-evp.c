/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the public EVP digest, cipher and MAC functions (api-evp.c):
 * SHA-256 (EVP_sha256, EVP_Digest*, EVP_MD_CTX_copy_ex), ChaCha20
 * (EVP_chacha20, EVP_CipherInit_ex, EVP_CipherUpdate, EVP_Cipher) and
 * Poly1305 (EVP_MAC_*).
 *
 * Expected values: the NIST byte-oriented SHA-256 vectors (short, long and
 * Monte Carlo messages, read from the vector files) and the FIPS 180-4
 * example messages; the examples and test vectors of RFC 8439 (2.3.2,
 * 2.4.2, 2.5.2, 2.6.2, 2.8.2 and Appendix A) through the EVP interface,
 * with the IV of EVP_chacha20 built as the 32-bit block counter followed
 * by the 12-byte nonce (state words 12 to 15); the Wycheproof vectors of
 * chacha20_poly1305_test, sealed and checked with the AEAD construction of
 * RFC 8439 2.8 written here around the EVP functions; a reference
 * keystream made from the block function of cryc-chacha.h at every 64-bit
 * counter, so that a stream split into calls of any size, the carry from
 * the low into the high half of the counter and the end of the counter's
 * range are checked; and the packet construction of SSH's
 * chacha20-poly1305@openssh.com (a 64-bit big-endian sequence number as
 * the nonce, block 0 for the Poly1305 key, block 1 onward for the payload,
 * a second key for the 4-byte length) built from the primitives directly.
 * Every refusal is checked for the one error it queues; every success for
 * an empty queue.
 */

#include <limits.h>

#include "check.h"
#include "vecfile.h"

#include <compat/crypto/err.h>
#include <compat/crypto/evp.h>

#include "userland/base/libcrypto-compat/cryc-api.h"
#include "userland/base/libcrypto-compat/cryc-chacha.h"
#include "userland/base/libcrypto-compat/cryc-err.h"

/* The sizes of the buffers that hold decoded vectors. */
#define VECTOR_BYTES		1024U

/* The longest message the generated tests use. */
#define MESSAGE_BYTES		1100U

/* A byte that fills a buffer to show later that nothing was written to it. */
#define SENTINEL_BYTE		0x5aU

/* The hex text of 32 and of 64 zero bytes (the plaintext that makes a stream cipher show its keystream). */
#define ZERO_32_BYTES	"0000000000000000000000000000000000000000000000000000000000000000"
#define ZERO_64_BYTES	ZERO_32_BYTES ZERO_32_BYTES

/* A vector of the block function: key, nonce, counter and the 64 bytes it produces. */
struct block_vector {
	const char *key;
	const char *nonce;
	uint32_t counter;
	const char *keystream;
};

/* A vector of the cipher: key, nonce, initial counter, plaintext and ciphertext. */
struct xor_vector {
	const char *key;
	const char *nonce;
	uint32_t counter;
	const char *plain;
	const char *cipher;
};

/* A vector of Poly1305: the 32-byte key, the message and the tag. */
struct mac_vector {
	const char *key;
	const char *message;
	const char *tag;
};

/* A vector of the Poly1305 key generation: ChaCha20 key, nonce and the 32-byte one-time key. */
struct keygen_vector {
	const char *key;
	const char *nonce;
	const char *onetime;
};

/* Values referenced: RFC 8439 (June 2018) 2.3.2, the serialized block. */
static const char rfc_block_serialized[] =
	"10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d4"
	"6c4ed2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8"
	"a2503c4e";

/* Values referenced: RFC 8439 (June 2018) 2.4.2, the plaintext "Sunscreen". */
static const char rfc_sunscreen_plain[] =
	"4c616469657320616e642047656e746c656d656e206f662074686520636c"
	"617373206f66202739393a204966204920636f756c64206f666665722079"
	"6f75206f6e6c79206f6e652074697020666f722074686520667574757265"
	"2c2073756e73637265656e20776f756c642062652069742e";

/* Values referenced: RFC 8439 (June 2018) 2.4.2, the ciphertext "Sunscreen". */
static const char rfc_sunscreen_cipher[] =
	"6e2e359a2568f98041ba0728dd0d6981e97e7aec1d4360c20a27afccfd9f"
	"ae0bf91b65c5524733ab8f593dabcd62b3571639d624e65152ab8f530c35"
	"9f0861d807ca0dbf500d6a6156a38e088a22b65e52bc514d16ccf806818c"
	"e91ab77937365af90bbf74a35be6b40b8eedf2785e42874d";

/* Values referenced: RFC 8439 (June 2018) 2.6.2, the key. */
static const char rfc_keygen_key[] =
	"808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d"
	"9e9f";

/* Values referenced: RFC 8439 (June 2018) 2.6.2, the output bytes. */
static const char rfc_keygen_output[] =
	"8ad5a08b905f81cc815040274ab29471a833b637e3fd0da508dbb8e2fdd1"
	"a646";

/* Values referenced: RFC 8439 (June 2018) 2.8.2, the plaintext. */
static const char rfc_aead_plain[] =
	"4c616469657320616e642047656e746c656d656e206f662074686520636c"
	"617373206f66202739393a204966204920636f756c64206f666665722079"
	"6f75206f6e6c79206f6e652074697020666f722074686520667574757265"
	"2c2073756e73637265656e20776f756c642062652069742e";

/* Values referenced: RFC 8439 (June 2018) 2.8.2, the AAD. */
static const char rfc_aead_aad[] =
	"50515253c0c1c2c3c4c5c6c7";

/* Values referenced: RFC 8439 (June 2018) 2.8.2, the key. */
static const char rfc_aead_key[] =
	"808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d"
	"9e9f";

/* Values referenced: RFC 8439 (June 2018) 2.8.2, the IV (the nonce is the 32-bit fixed-common part 07 00 00 00 then the IV). */
static const char rfc_aead_iv[] =
	"4041424344454647";

/* Values referenced: RFC 8439 (June 2018) 2.8.2, the ciphertext. */
static const char rfc_aead_cipher[] =
	"d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee"
	"62d63dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b6"
	"7ecd3b3692ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585"
	"808b4831d7bc3ff4def08e4b7a9de576d26586cec64b6116";

/* Values referenced: RFC 8439 (June 2018) 2.8.2, the Poly1305 key. */
static const char rfc_aead_otk[] =
	"7bac2b252db447af09b67a55a4e955840ae1d6731075d9eb2a9375783ed5"
	"53ff";

/* Values referenced: RFC 8439 (June 2018) 2.8.2, the tag. */
static const char rfc_aead_tag[] =
	"1ae10b594f09e26a7e902ecbd0600691";

/* Values referenced: RFC 8439 (June 2018) 2.5.2, the message to be authenticated. */
static const char rfc_poly_message[] =
	"43727970746f6772617068696320466f72756d2052657365617263682047"
	"726f7570";

/* Values referenced: RFC 8439 (June 2018) 2.5.2, the key material. */
static const char rfc_poly_key[] =
	"85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149"
	"f51b";

/* Values referenced: RFC 8439 (June 2018) 2.5.2, the tag. */
static const char rfc_poly_tag[] =
	"a8061dc1305136c6c22b8baf0c0127a9";

/* Values referenced: RFC 8439 (June 2018) A.5, the ChaCha20 key. */
static const char rfc_open_key[] =
	"1c9240a5eb55d38af333888604f6b5f0473917c1402b80099dca5cbc2070"
	"75c0";

/* Values referenced: RFC 8439 (June 2018) A.5, the ciphertext. */
static const char rfc_open_cipher[] =
	"64a0861575861af460f062c79be643bd5e805cfd345cf389f108670ac76c"
	"8cb24c6cfc18755d43eea09ee94e382d26b0bdb7b73c321b0100d4f03b7f"
	"355894cf332f830e710b97ce98c8a84abd0b948114ad176e008d33bd60f9"
	"82b1ff37c8559797a06ef4f0ef61c186324e2b3506383606907b6a7c02b0"
	"f9f6157b53c867e4b9166c767b804d46a59b5216cde7a4e99040c5a40433"
	"225ee282a1b0a06c523eaf4534d7f83fa1155b0047718cbc546a0d072b04"
	"b3564eea1b422273f548271a0bb2316053fa76991955ebd63159434ecebb"
	"4e466dae5a1073a6727627097a1049e617d91d361094fa68f0ff77987130"
	"305beaba2eda04df997b714d6c6f2c29a6ad5cb4022b02709b";

/* Values referenced: RFC 8439 (June 2018) A.5, the nonce. */
static const char rfc_open_nonce[] =
	"000000000102030405060708";

/* Values referenced: RFC 8439 (June 2018) A.5, the AAD. */
static const char rfc_open_aad[] =
	"f33388860000000000004e91";

/* Values referenced: RFC 8439 (June 2018) A.5, the received tag. */
static const char rfc_open_tag[] =
	"eead9d67890cbb22392336fea1851f38";

/* Values referenced: RFC 8439 (June 2018) A.5, the plaintext. */
static const char rfc_open_plain[] =
	"496e7465726e65742d4472616674732061726520647261667420646f6375"
	"6d656e74732076616c696420666f722061206d6178696d756d206f662073"
	"6978206d6f6e74687320616e64206d617920626520757064617465642c20"
	"7265706c616365642c206f72206f62736f6c65746564206279206f746865"
	"7220646f63756d656e747320617420616e792074696d652e204974206973"
	"20696e617070726f70726961746520746f2075736520496e7465726e6574"
	"2d447261667473206173207265666572656e6365206d6174657269616c20"
	"6f7220746f2063697465207468656d206f74686572207468616e20617320"
	"2fe2809c776f726b20696e2070726f67726573732e2fe2809d";

/* Values referenced: RFC 8439 (June 2018) A.1, Test Vectors 1 to 5 (key, nonce, block counter, keystream). */
static const struct block_vector a1_vectors[] = {
	{
		"000000000000000000000000000000000000000000000000000000000000"
		"0000",
		"000000000000000000000000",
		0U,
		"76b8e0ada0f13d90405d6ae55386bd28bdd219b8a08ded1aa836efcc8b77"
		"0dc7da41597c5157488d7724e03fb8d84a376a43b8f41518a11cc387b669"
		"b2ee6586"
	},
	{
		"000000000000000000000000000000000000000000000000000000000000"
		"0000",
		"000000000000000000000000",
		1U,
		"9f07e7be5551387a98ba977c732d080dcb0f29a048e3656912c6533e32ee"
		"7aed29b721769ce64e43d57133b074d839d531ed1f28510afb45ace10a1f"
		"4b794d6f"
	},
	{
		"000000000000000000000000000000000000000000000000000000000000"
		"0001",
		"000000000000000000000000",
		1U,
		"3aeb5224ecf849929b9d828db1ced4dd832025e8018b8160b82284f3c949"
		"aa5a8eca00bbb4a73bdad192b5c42f73f2fd4e273644c8b36125a64addeb"
		"006c13a0"
	},
	{
		"00ff00000000000000000000000000000000000000000000000000000000"
		"0000",
		"000000000000000000000000",
		2U,
		"72d54dfbf12ec44b362692df94137f328fea8da73990265ec1bbbea1ae9a"
		"f0ca13b25aa26cb4a648cb9b9d1be65b2c0924a66c54d545ec1b7374f487"
		"2e99f096"
	},
	{
		"000000000000000000000000000000000000000000000000000000000000"
		"0000",
		"000000000000000000000002",
		0U,
		"c2c64d378cd536374ae204b9ef933fcd1a8b2288b3dfa49672ab765b54ee"
		"27c78a970e0e955c14f3a88e741b97c286f75f8fc299e8148362fa198a39"
		"531bed6d"
	}
};

/* Values referenced: RFC 8439 (June 2018) A.2, Test Vectors 1 to 3 (key, nonce, initial block counter, plaintext, ciphertext). */
static const struct xor_vector a2_vectors[] = {
	{
		"000000000000000000000000000000000000000000000000000000000000"
		"0000",
		"000000000000000000000000",
		0U,
		"000000000000000000000000000000000000000000000000000000000000"
		"000000000000000000000000000000000000000000000000000000000000"
		"00000000",
		"76b8e0ada0f13d90405d6ae55386bd28bdd219b8a08ded1aa836efcc8b77"
		"0dc7da41597c5157488d7724e03fb8d84a376a43b8f41518a11cc387b669"
		"b2ee6586"
	},
	{
		"000000000000000000000000000000000000000000000000000000000000"
		"0001",
		"000000000000000000000002",
		1U,
		"416e79207375626d697373696f6e20746f20746865204945544620696e74"
		"656e6465642062792074686520436f6e7472696275746f7220666f722070"
		"75626c69636174696f6e20617320616c6c206f722070617274206f662061"
		"6e204945544620496e7465726e65742d4472616674206f72205246432061"
		"6e6420616e792073746174656d656e74206d6164652077697468696e2074"
		"686520636f6e74657874206f6620616e2049455446206163746976697479"
		"20697320636f6e7369646572656420616e20224945544620436f6e747269"
		"627574696f6e222e20537563682073746174656d656e747320696e636c75"
		"6465206f72616c2073746174656d656e747320696e204945544620736573"
		"73696f6e732c2061732077656c6c206173207772697474656e20616e6420"
		"656c656374726f6e696320636f6d6d756e69636174696f6e73206d616465"
		"20617420616e792074696d65206f7220706c6163652c2077686963682061"
		"72652061646472657373656420746f",
		"a3fbf07df3fa2fde4f376ca23e82737041605d9f4f4f57bd8cff2c1d4b79"
		"55ec2a97948bd3722915c8f3d337f7d370050e9e96d647b7c39f56e031ca"
		"5eb6250d4042e02785ececfa4b4bb5e8ead0440e20b6e8db09d881a7c613"
		"2f420e52795042bdfa7773d8a9051447b3291ce1411c680465552aa6c405"
		"b7764d5e87bea85ad00f8449ed8f72d0d662ab052691ca66424bc86d2df8"
		"0ea41f43abf937d3259dc4b2d0dfb48a6c9139ddd7f76966e928e635553b"
		"a76c5c879d7b35d49eb2e62b0871cdac638939e25e8a1e0ef9d5280fa8ca"
		"328b351c3c765989cbcf3daa8b6ccc3aaf9f3979c92b3720fc88dc95ed84"
		"a1be059c6499b9fda236e7e818b04b0bc39c1e876b193bfe5569753f8812"
		"8cc08aaa9b63d1a16f80ef2554d7189c411f5869ca52c5b83fa36ff216b9"
		"c1d30062bebcfd2dc5bce0911934fda79a86f6e698ced759c3ff9b647733"
		"8f3da4f9cd8514ea9982ccafb341b2384dd902f3d1ab7ac61dd29c6f21ba"
		"5b862f3730e37cfdc4fd806c22f221"
	},
	{
		"1c9240a5eb55d38af333888604f6b5f0473917c1402b80099dca5cbc2070"
		"75c0",
		"000000000000000000000002",
		42U,
		"2754776173206272696c6c69672c20616e642074686520736c6974687920"
		"746f7665730a446964206779726520616e642067696d626c6520696e2074"
		"686520776162653a0a416c6c206d696d737920776572652074686520626f"
		"726f676f7665732c0a416e6420746865206d6f6d65207261746873206f75"
		"7467726162652e",
		"62e6347f95ed87a45ffae7426f27a1df5fb69110044c0d73118effa95b01"
		"e5cf166d3df2d721caf9b21e5fb14c616871fd84c54f9d65b283196c7fe4"
		"f60553ebf39c6402c42234e32a356b3e764312a61a5532055716ead69625"
		"68f87d3f3f7704c6a8d1bcd1bf4d50d6154b6da731b187b58dfd728afa36"
		"757a797ac188d1"
	}
};

/* Values referenced: RFC 8439 (June 2018) A.3, Test Vectors 1 to 11 (the 32-byte one-time key, the text, the tag). */
static const struct mac_vector a3_vectors[] = {
	{
		"000000000000000000000000000000000000000000000000000000000000"
		"0000",
		"000000000000000000000000000000000000000000000000000000000000"
		"000000000000000000000000000000000000000000000000000000000000"
		"00000000",
		"00000000000000000000000000000000"
	},
	{
		"0000000000000000000000000000000036e5f6b5c5e06070f0efca96227a"
		"863e",
		"416e79207375626d697373696f6e20746f20746865204945544620696e74"
		"656e6465642062792074686520436f6e7472696275746f7220666f722070"
		"75626c69636174696f6e20617320616c6c206f722070617274206f662061"
		"6e204945544620496e7465726e65742d4472616674206f72205246432061"
		"6e6420616e792073746174656d656e74206d6164652077697468696e2074"
		"686520636f6e74657874206f6620616e2049455446206163746976697479"
		"20697320636f6e7369646572656420616e20224945544620436f6e747269"
		"627574696f6e222e20537563682073746174656d656e747320696e636c75"
		"6465206f72616c2073746174656d656e747320696e204945544620736573"
		"73696f6e732c2061732077656c6c206173207772697474656e20616e6420"
		"656c656374726f6e696320636f6d6d756e69636174696f6e73206d616465"
		"20617420616e792074696d65206f7220706c6163652c2077686963682061"
		"72652061646472657373656420746f",
		"36e5f6b5c5e06070f0efca96227a863e"
	},
	{
		"36e5f6b5c5e06070f0efca96227a863e0000000000000000000000000000"
		"0000",
		"416e79207375626d697373696f6e20746f20746865204945544620696e74"
		"656e6465642062792074686520436f6e7472696275746f7220666f722070"
		"75626c69636174696f6e20617320616c6c206f722070617274206f662061"
		"6e204945544620496e7465726e65742d4472616674206f72205246432061"
		"6e6420616e792073746174656d656e74206d6164652077697468696e2074"
		"686520636f6e74657874206f6620616e2049455446206163746976697479"
		"20697320636f6e7369646572656420616e20224945544620436f6e747269"
		"627574696f6e222e20537563682073746174656d656e747320696e636c75"
		"6465206f72616c2073746174656d656e747320696e204945544620736573"
		"73696f6e732c2061732077656c6c206173207772697474656e20616e6420"
		"656c656374726f6e696320636f6d6d756e69636174696f6e73206d616465"
		"20617420616e792074696d65206f7220706c6163652c2077686963682061"
		"72652061646472657373656420746f",
		"f3477e7cd95417af89a6b8794c310cf0"
	},
	{
		"1c9240a5eb55d38af333888604f6b5f0473917c1402b80099dca5cbc2070"
		"75c0",
		"2754776173206272696c6c69672c20616e642074686520736c6974687920"
		"746f7665730a446964206779726520616e642067696d626c6520696e2074"
		"686520776162653a0a416c6c206d696d737920776572652074686520626f"
		"726f676f7665732c0a416e6420746865206d6f6d65207261746873206f75"
		"7467726162652e",
		"4541669a7eaaee61e708dc7cbcc5eb62"
	},
	{
		"020000000000000000000000000000000000000000000000000000000000"
		"0000",
		"ffffffffffffffffffffffffffffffff",
		"03000000000000000000000000000000"
	},
	{
		"02000000000000000000000000000000ffffffffffffffffffffffffffff"
		"ffff",
		"02000000000000000000000000000000",
		"03000000000000000000000000000000"
	},
	{
		"010000000000000000000000000000000000000000000000000000000000"
		"0000",
		"fffffffffffffffffffffffffffffffff0ffffffffffffffffffffffffff"
		"ffff11000000000000000000000000000000",
		"05000000000000000000000000000000"
	},
	{
		"010000000000000000000000000000000000000000000000000000000000"
		"0000",
		"fffffffffffffffffffffffffffffffffbfefefefefefefefefefefefefe"
		"fefe01010101010101010101010101010101",
		"00000000000000000000000000000000"
	},
	{
		"020000000000000000000000000000000000000000000000000000000000"
		"0000",
		"fdffffffffffffffffffffffffffffff",
		"faffffffffffffffffffffffffffffff"
	},
	{
		"010000000000000004000000000000000000000000000000000000000000"
		"0000",
		"e33594d7505e43b900000000000000003394d7505e4379cd010000000000"
		"000000000000000000000000000000000000010000000000000000000000"
		"00000000",
		"14000000000000005500000000000000"
	},
	{
		"010000000000000004000000000000000000000000000000000000000000"
		"0000",
		"e33594d7505e43b900000000000000003394d7505e4379cd010000000000"
		"000000000000000000000000000000000000",
		"13000000000000000000000000000000"
	}
};

/* Values referenced: RFC 8439 (June 2018) A.4, Test Vectors 1 to 3 (ChaCha20 key, nonce, Poly1305 one-time key). */
static const struct keygen_vector a4_vectors[] = {
	{
		"000000000000000000000000000000000000000000000000000000000000"
		"0000",
		"000000000000000000000000",
		"76b8e0ada0f13d90405d6ae55386bd28bdd219b8a08ded1aa836efcc8b77"
		"0dc7"
	},
	{
		"000000000000000000000000000000000000000000000000000000000000"
		"0001",
		"000000000000000000000002",
		"ecfa254f845f647473d3cb140da9e87606cb33066c447b87bc2666dde3fb"
		"b739"
	},
	{
		"1c9240a5eb55d38af333888604f6b5f0473917c1402b80099dca5cbc2070"
		"75c0",
		"000000000000000000000002",
		"965e3bc6f9ec7ed9560808f4d229f94b137ff275ca9b3fcbdd59deaad233"
		"10ae"
	}
};

/* The state of the deterministic generator of the test inputs. */
static uint32_t random_state = 0x3a1f7c25U;

/* The number of Wycheproof records checked and skipped, for the summary. */
static unsigned long wycheproof_checked;
static unsigned long wycheproof_skipped;

/* Returns the next pseudo-random 32-bit value (xorshift). */
static uint32_t
random_next(void)
{
	/* The three xorshift steps. */
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;
	return random_state;
}

/* Fills size bytes at out with pseudo-random bytes. */
static void
random_fill(
	uint8_t *out,
	size_t size)
{
	size_t index;

	/* One byte at a time. */
	for (index = 0; index < size; index++)
		out[index] = (uint8_t)(random_next() >> 24);
}

/* Returns a pseudo-random number from 0 to limit - 1. */
static size_t
random_below(
	size_t limit)
{
	/* The high bits of the generator are the better ones. */
	return (size_t)((random_next() >> 8) % (uint32_t)limit);
}

/* Returns whether size bytes at buffer all equal value. */
static int
all_equal(
	const uint8_t *buffer,
	size_t size,
	uint8_t value)
{
	size_t index;

	/* Any other byte. */
	for (index = 0; index < size; index++)
		if (buffer[index] != value)
			return 0;
	return 1;
}

/* Writes a 32-bit little-endian word (the test's own, not the library's). */
static void
write_le32(
	uint8_t *bytes,
	uint32_t value)
{
	/* Byte 0 is the least significant. */
	bytes[0] = (uint8_t)(value & 0xffU);
	bytes[1] = (uint8_t)((value >> 8) & 0xffU);
	bytes[2] = (uint8_t)((value >> 16) & 0xffU);
	bytes[3] = (uint8_t)((value >> 24) & 0xffU);
}

/* Writes a 64-bit little-endian integer. */
static void
write_le64(
	uint8_t *bytes,
	uint64_t value)
{
	/* The low half first. */
	write_le32(bytes, (uint32_t)(value & 0xffffffffU));
	write_le32(bytes + 4, (uint32_t)(value >> 32));
}

/* Writes a 64-bit big-endian integer. */
static void
write_be64(
	uint8_t *bytes,
	uint64_t value)
{
	size_t index;

	/* Byte 0 is the most significant. */
	for (index = 0; index < 8U; index++)
		bytes[index] = (uint8_t)((value >> (56U - 8U * index)) & 0xffU);
}

/* Decodes the hex text of a vector into out and checks that it was well formed; returns the size (0 when not). */
static size_t
unhex(
	const char *text,
	uint8_t *out,
	size_t room)
{
	size_t size;

	/* The decoder says (size_t)-1 for odd lengths, bad digits and too little room. */
	size = hex_decode(text, out, room);
	CHECK(size != (size_t)-1);
	if (size == (size_t)-1)
		return 0;
	return size;
}

/*
 * The code of the error the EVP library queues for a reason: OpenSSL's
 * packing of library 6 and the reason number (the common reasons carry
 * their flag bits).
 */
static unsigned long
error_code(
	unsigned int reason)
{
	/* The library number above bit 23, the reason below. */
	return (((unsigned long)6U) << 23) | (unsigned long)reason;
}

/* Checks that the queue holds exactly one error, the one for reason, and empties it. */
static void
check_refusal(
	unsigned int reason)
{
	unsigned long first;
	unsigned long second;

	/* The first and the next code of the queue. */
	first = ERR_get_error();
	second = ERR_get_error();
	CHECK(first == error_code(reason));
	CHECK(second == 0UL);
}

/* Runs a call that must fail with the value failure and queue exactly the error for reason. */
#define REFUSED(call, failure, reason) \
	do { \
		ERR_clear_error(); \
		CHECK((call) == (failure)); \
		check_refusal(reason); \
	} while (0)

/* Runs a call that must succeed (return 1) and queue nothing. */
#define OK(call) \
	do { \
		ERR_clear_error(); \
		CHECK((call) == 1); \
		CHECK(ERR_peek_error() == 0UL); \
	} while (0)

/* The reasons the implementation queues (the header's constants, spelled for each condition). */
#define R_NULL		CRYC_R_PASSED_NULL_PARAMETER
#define R_ARGUMENT	CRYC_R_PASSED_INVALID_ARGUMENT
#define R_ALGORITHM	CRYC_R_EVP_UNSUPPORTED_ALGORITHM
#define R_NOT_SET_UP	CRYC_R_EVP_OPERATION_NOT_INITIALIZED
#define R_KEY_LENGTH	CRYC_R_EVP_INVALID_KEY_LENGTH
#define R_TOO_SMALL	CRYC_R_EVP_BUFFER_TOO_SMALL

/* A made-up digest algorithm, cipher and MAC that the functions must refuse (they are not the constant objects). */
static struct evp_md_st fake_md = { CRYC_HASH_SHA384, 48 };
static struct evp_cipher_st fake_cipher = { 32, 16 };
static struct evp_mac_st fake_mac = { 16 };

/* ------------------------------------------------------------------ */
/* SHA-256                                                              */
/* ------------------------------------------------------------------ */

/* Hashes with EVP_Digest, checking the call, and writes 32 bytes to out. */
static void
digest_one_shot(
	const uint8_t *data,
	size_t size,
	uint8_t *out)
{
	unsigned int length;

	/* One call, with the size output. */
	length = 0;
	OK(EVP_Digest(data, size, out, &length, EVP_sha256(), NULL));
	CHECK(length == 32U);
}

/*
 * Checks a message of the vector files against its digest through every
 * way of using the digest functions: one shot, one update, random chunks
 * with empty updates in between, and a snapshot taken in the middle that
 * both halves continue independently.
 */
static void
check_digest_message(
	EVP_MD_CTX *ctx,
	EVP_MD_CTX *copy,
	const uint8_t *message,
	size_t size,
	const uint8_t *expected)
{
	uint8_t got[EVP_MAX_MD_SIZE + 1U];
	uint8_t *changed;
	uint8_t changed_digest[32];
	unsigned int length;
	size_t offset;
	size_t take;
	size_t half;

	/* One shot, with and without the size output; nothing is written past the digest. */
	memset(got, SENTINEL_BYTE, sizeof(got));
	length = 0;
	OK(EVP_Digest(message, size, got, &length, EVP_sha256(), NULL));
	CHECK(length == 32U);
	CHECK_BYTES(got, expected, 32U);
	CHECK(got[32] == SENTINEL_BYTE);
	memset(got, SENTINEL_BYTE, sizeof(got));
	OK(EVP_Digest(message, size, got, NULL, EVP_sha256(), NULL));
	CHECK_BYTES(got, expected, 32U);
	CHECK(got[32] == SENTINEL_BYTE);

	/* The whole message in one update, on a context that is reused for every message. */
	OK(EVP_DigestInit_ex(ctx, EVP_sha256(), NULL));
	CHECK(ctx->mode == CRYC_MD_DIGEST);
	CHECK(ctx->md == EVP_sha256());
	OK(EVP_DigestUpdate(ctx, message, size));
	memset(got, SENTINEL_BYTE, sizeof(got));
	length = 0;
	OK(EVP_DigestFinal_ex(ctx, got, &length));
	CHECK(length == 32U);
	CHECK_BYTES(got, expected, 32U);
	CHECK(got[32] == SENTINEL_BYTE);

	/* After the final the context is empty: the hash state is wiped and it must be started again. */
	CHECK(ctx->mode == CRYC_MD_NONE);
	CHECK(ctx->md == NULL);
	CHECK(all_equal((const uint8_t *)&ctx->hash, sizeof(ctx->hash), 0));

	/* Random chunks, among them empty ones and one of a NULL pointer with no bytes; the size output is optional. */
	OK(EVP_DigestInit_ex(ctx, EVP_sha256(), NULL));
	offset = 0;
	while (offset < size) {
		take = random_below(130U);
		if (take > size - offset)
			take = size - offset;
		if (take == 0)
			OK(EVP_DigestUpdate(ctx, NULL, 0));
		else
			OK(EVP_DigestUpdate(ctx, message + offset, take));
		offset += take;
	}
	memset(got, SENTINEL_BYTE, sizeof(got));
	OK(EVP_DigestFinal_ex(ctx, got, NULL));
	CHECK_BYTES(got, expected, 32U);
	CHECK(got[32] == SENTINEL_BYTE);

	/*
	 * A snapshot in the middle: the destination may hold another digest
	 * in progress (and then loses it), both contexts go on separately,
	 * and a byte added to the first does not reach the copy.
	 */
	half = size / 2U;
	OK(EVP_DigestInit_ex(ctx, EVP_sha256(), NULL));
	OK(EVP_DigestUpdate(ctx, message, half));
	if ((random_next() & 1U) != 0U) {
		OK(EVP_DigestInit_ex(copy, EVP_sha256(), NULL));
		OK(EVP_DigestUpdate(copy, "junk", 4U));
	}
	OK(EVP_MD_CTX_copy_ex(copy, ctx));
	CHECK(copy->mode == CRYC_MD_DIGEST);
	CHECK(copy->md == EVP_sha256());
	OK(EVP_DigestUpdate(ctx, "x", 1U));
	OK(EVP_DigestUpdate(copy, message + half, size - half));
	memset(got, SENTINEL_BYTE, sizeof(got));
	OK(EVP_DigestFinal_ex(copy, got, NULL));
	CHECK_BYTES(got, expected, 32U);
	changed = malloc(half + 1U);
	CHECK(changed != NULL);
	if (changed != NULL) {
		memcpy(changed, message, half);
		changed[half] = (uint8_t)'x';
		digest_one_shot(changed, half + 1U, changed_digest);
		OK(EVP_DigestFinal_ex(ctx, got, NULL));
		CHECK_BYTES(got, changed_digest, 32U);
		free(changed);
	} else {
		OK(EVP_DigestFinal_ex(ctx, got, NULL));
	}
}

/* Runs the short-message or long-message vector file of SHA-256 (Len in bits, Msg, MD); returns the records read. */
static unsigned long
run_digest_message_file(
	const char *name)
{
	struct vec_file file;
	EVP_MD_CTX *ctx;
	EVP_MD_CTX *copy;
	uint8_t *message;
	uint8_t expected[64];
	const char *text;
	size_t size;
	size_t room;
	unsigned long count;

	/* The two contexts are reused for every message of the file. */
	count = 0;
	ctx = EVP_MD_CTX_new();
	copy = EVP_MD_CTX_new();
	CHECK(ctx != NULL && copy != NULL);
	if (vec_open(&file, name) != 0 || ctx == NULL || copy == NULL) {
		CHECK(0);
		EVP_MD_CTX_free(ctx);
		EVP_MD_CTX_free(copy);
		return 0;
	}
	while (vec_next(&file)) {
		text = vec_get(&file, "Len");
		if (text == NULL)
			continue;
		size = (size_t)strtoul(text, NULL, 10) / 8U;
		room = strlen(vec_get(&file, "Msg")) / 2U + 1U;
		message = malloc(room);
		CHECK(message != NULL);
		if (message == NULL)
			break;
		CHECK(vec_hex(&file, "Msg", message, room) != (size_t)-1);
		CHECK(vec_hex(&file, "MD", expected, sizeof(expected)) == 32U);
		check_digest_message(ctx, copy, message, size, expected);
		free(message);
		count++;
	}
	vec_close(&file);
	EVP_MD_CTX_free(ctx);
	EVP_MD_CTX_free(copy);
	return count;
}

/* Runs the Monte Carlo vector file of SHA-256 through EVP_Digest; returns the checkpoints checked. */
static unsigned long
run_digest_monte_file(void)
{
	struct vec_file file;
	uint8_t seed[32];
	uint8_t expected[64];
	uint8_t chain[3 * 32];
	uint8_t next[32];
	size_t step;
	int have_seed;
	unsigned long count;

	/* The seed record, then the 100 checkpoints in order. */
	count = 0;
	have_seed = 0;
	if (vec_open(&file, "cavp-shabytetestvectors-SHA256Monte") != 0) {
		CHECK(0);
		return 0;
	}
	while (vec_next(&file)) {
		if (vec_get(&file, "Seed") != NULL) {
			CHECK(vec_hex(&file, "Seed", seed, sizeof(seed)) == 32U);
			have_seed = 1;
			continue;
		}
		if (!have_seed || vec_get(&file, "MD") == NULL)
			continue;
		CHECK(vec_hex(&file, "MD", expected, sizeof(expected)) == 32U);

		/* MD(0) = MD(1) = MD(2) = seed; MD(i) = hash(MD(i-3) | MD(i-2) | MD(i-1)). */
		memcpy(chain, seed, 32U);
		memcpy(chain + 32U, seed, 32U);
		memcpy(chain + 64U, seed, 32U);
		for (step = 0; step < 1000U; step++) {
			if (EVP_Digest(chain, sizeof(chain), next, NULL, EVP_sha256(), NULL) != 1)
				CHECK(0);
			memmove(chain, chain + 32U, 64U);
			memcpy(chain + 64U, next, 32U);
		}
		CHECK_BYTES(next, expected, 32U);
		memcpy(seed, next, 32U);
		count++;
	}
	vec_close(&file);
	return count;
}

/* Checks the example messages published for SHA-256: the empty message, "abc", the two-block message, a million letters a. */
static void
test_digest_examples(void)
{
	static const char abc[] = "abc";
	static const char two_block[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	uint8_t expected[32];
	uint8_t got[32];
	uint8_t *million;
	uint8_t *block;
	EVP_MD_CTX *ctx;
	size_t index;

	/* The empty message. */
	CHECK(unhex("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", expected, sizeof(expected)) == 32U);
	digest_one_shot((const uint8_t *)"", 0U, got);
	CHECK_BYTES(got, expected, 32U);
	OK(EVP_Digest(NULL, 0U, got, NULL, EVP_sha256(), NULL));
	CHECK_BYTES(got, expected, 32U);

	/* "abc". */
	CHECK(unhex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", expected, sizeof(expected)) == 32U);
	digest_one_shot((const uint8_t *)abc, 3U, got);
	CHECK_BYTES(got, expected, 32U);

	/* The two-block message of 448 bits. */
	CHECK(unhex("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", expected, sizeof(expected)) == 32U);
	digest_one_shot((const uint8_t *)two_block, strlen(two_block), got);
	CHECK_BYTES(got, expected, 32U);

	/* A million letters a, one shot and in a thousand updates of a thousand bytes. */
	CHECK(unhex("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", expected, sizeof(expected)) == 32U);
	million = malloc(1000000U);
	CHECK(million != NULL);
	if (million == NULL)
		return;
	memset(million, (int)'a', 1000000U);
	digest_one_shot(million, 1000000U, got);
	CHECK_BYTES(got, expected, 32U);
	ctx = EVP_MD_CTX_new();
	CHECK(ctx != NULL);
	if (ctx != NULL) {
		OK(EVP_DigestInit_ex(ctx, EVP_sha256(), NULL));
		block = million;
		for (index = 0; index < 1000U; index++) {
			OK(EVP_DigestUpdate(ctx, block, 1000U));
			block += 1000U;
		}
		OK(EVP_DigestFinal_ex(ctx, got, NULL));
		CHECK_BYTES(got, expected, 32U);
		EVP_MD_CTX_free(ctx);
	}
	free(million);
}

/* Checks the SHA-256 vector files of the National Institute of Standards and Technology. */
static void
test_digest_vectors(void)
{
	unsigned long count;

	/* Short messages (0 to 64 bytes: every padding case of the first blocks). */
	count = run_digest_message_file("cavp-shabytetestvectors-SHA256ShortMsg");
	CHECK(count == 65UL);

	/* Long messages. */
	count = run_digest_message_file("cavp-shabytetestvectors-SHA256LongMsg");
	CHECK(count == 64UL);

	/* The Monte Carlo test. */
	count = run_digest_monte_file();
	CHECK(count == 100UL);
}

/* Checks the digest functions' arguments, states and refusals. */
static void
test_digest_api(void)
{
	const EVP_MD *md;
	EVP_MD_CTX *ctx;
	EVP_MD_CTX *other;
	uint8_t out[40];
	uint8_t expected[32];
	uint8_t guard[40];
	unsigned int length;
	int dummy_engine;

	/* The algorithm is one constant object with a size of 32. */
	md = EVP_sha256();
	CHECK(md != NULL);
	CHECK(EVP_sha256() == md);
	CHECK(EVP_MD_get_size(md) == 32);
	CHECK(EVP_MD_size(md) == 32);
	CHECK(EVP_MD_get_size(NULL) == -1);
	CHECK(EVP_MAX_MD_SIZE >= 32);
	CHECK(ERR_peek_error() == 0UL);

	/* Freeing nothing is allowed for every kind of context. */
	EVP_MD_CTX_free(NULL);
	EVP_CIPHER_CTX_free(NULL);
	EVP_MAC_CTX_free(NULL);
	EVP_MAC_free(NULL);

	/* A new context is empty and refuses everything but a start. */
	ctx = EVP_MD_CTX_new();
	other = EVP_MD_CTX_new();
	CHECK(ctx != NULL && other != NULL);
	if (ctx == NULL || other == NULL) {
		EVP_MD_CTX_free(ctx);
		EVP_MD_CTX_free(other);
		return;
	}
	CHECK(ctx->mode == CRYC_MD_NONE && ctx->key == NULL);
	memset(out, SENTINEL_BYTE, sizeof(out));
	memcpy(guard, out, sizeof(out));
	length = 77U;
	REFUSED(EVP_DigestUpdate(ctx, "a", 1U), 0, R_NOT_SET_UP);
	REFUSED(EVP_DigestFinal_ex(ctx, out, &length), 0, R_NOT_SET_UP);
	CHECK(length == 77U);
	CHECK_BYTES(out, guard, sizeof(out));
	REFUSED(EVP_MD_CTX_copy_ex(other, ctx), 0, R_NOT_SET_UP);
	CHECK(other->mode == CRYC_MD_NONE);

	/* A start with a missing context, algorithm or an engine, or another algorithm, is refused. */
	REFUSED(EVP_DigestInit_ex(NULL, md, NULL), 0, R_NULL);
	REFUSED(EVP_DigestInit_ex(ctx, NULL, NULL), 0, R_NULL);
	REFUSED(EVP_DigestInit_ex(ctx, md, (ENGINE *)&dummy_engine), 0, R_ARGUMENT);
	REFUSED(EVP_DigestInit_ex(ctx, &fake_md, NULL), 0, R_ALGORITHM);
	CHECK(ctx->mode == CRYC_MD_NONE);

	/* A refused start leaves a digest in progress alone. */
	OK(EVP_DigestInit_ex(ctx, md, NULL));
	OK(EVP_DigestUpdate(ctx, "abc", 3U));
	REFUSED(EVP_DigestInit_ex(ctx, NULL, NULL), 0, R_NULL);
	REFUSED(EVP_DigestInit_ex(ctx, &fake_md, NULL), 0, R_ALGORITHM);
	REFUSED(EVP_DigestInit_ex(ctx, md, (ENGINE *)&dummy_engine), 0, R_ARGUMENT);
	CHECK(ctx->mode == CRYC_MD_DIGEST);

	/* Refused updates change nothing. */
	REFUSED(EVP_DigestUpdate(NULL, "a", 1U), 0, R_NULL);
	REFUSED(EVP_DigestUpdate(ctx, NULL, 5U), 0, R_NULL);
	OK(EVP_DigestUpdate(ctx, NULL, 0U));

	/* A refused final (no context, no output) leaves the digest in progress. */
	length = 77U;
	REFUSED(EVP_DigestFinal_ex(NULL, out, &length), 0, R_NULL);
	REFUSED(EVP_DigestFinal_ex(ctx, NULL, &length), 0, R_NULL);
	CHECK(length == 77U);
	CHECK(ctx->mode == CRYC_MD_DIGEST);

	/* A copy onto itself changes nothing; a copy with a missing side is refused. */
	OK(EVP_MD_CTX_copy_ex(ctx, ctx));
	REFUSED(EVP_MD_CTX_copy_ex(NULL, ctx), 0, R_NULL);
	REFUSED(EVP_MD_CTX_copy_ex(other, NULL), 0, R_NULL);

	/* The digest in progress still is the digest of "abc". */
	CHECK(unhex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", expected, sizeof(expected)) == 32U);
	memset(out, SENTINEL_BYTE, sizeof(out));
	OK(EVP_DigestFinal_ex(ctx, out, &length));
	CHECK(length == 32U);
	CHECK_BYTES(out, expected, 32U);

	/* A finished context refuses update, final and being copied until it is started again. */
	REFUSED(EVP_DigestUpdate(ctx, "a", 1U), 0, R_NOT_SET_UP);
	REFUSED(EVP_DigestFinal_ex(ctx, out, &length), 0, R_NOT_SET_UP);
	REFUSED(EVP_MD_CTX_copy_ex(other, ctx), 0, R_NOT_SET_UP);
	OK(EVP_DigestInit_ex(ctx, md, NULL));
	OK(EVP_DigestUpdate(ctx, "abc", 3U));
	memset(out, SENTINEL_BYTE, sizeof(out));
	OK(EVP_DigestFinal_ex(ctx, out, NULL));
	CHECK_BYTES(out, expected, 32U);

	/* A start discards a digest in progress. */
	OK(EVP_DigestInit_ex(ctx, md, NULL));
	OK(EVP_DigestUpdate(ctx, "discarded", 9U));
	OK(EVP_DigestInit_ex(ctx, md, NULL));
	OK(EVP_DigestUpdate(ctx, "abc", 3U));
	memset(out, SENTINEL_BYTE, sizeof(out));
	OK(EVP_DigestFinal_ex(ctx, out, NULL));
	CHECK_BYTES(out, expected, 32U);

	/* The one-shot function refuses what it cannot do and writes nothing then. */
	memset(out, SENTINEL_BYTE, sizeof(out));
	memcpy(guard, out, sizeof(out));
	length = 77U;
	REFUSED(EVP_Digest("abc", 3U, out, &length, NULL, NULL), 0, R_NULL);
	REFUSED(EVP_Digest("abc", 3U, out, &length, &fake_md, NULL), 0, R_ALGORITHM);
	REFUSED(EVP_Digest("abc", 3U, out, &length, md, (ENGINE *)&dummy_engine), 0, R_ARGUMENT);
	REFUSED(EVP_Digest("abc", 3U, NULL, &length, md, NULL), 0, R_NULL);
	REFUSED(EVP_Digest(NULL, 3U, out, &length, md, NULL), 0, R_NULL);
	CHECK(length == 77U);
	CHECK_BYTES(out, guard, sizeof(out));

	EVP_MD_CTX_free(ctx);
	EVP_MD_CTX_free(other);
	CHECK(ERR_peek_error() == 0UL);
}

/*
 * Checks that a digest context used for a signature or a verification
 * releases the reference it holds to the key: when it is freed, when it
 * is started as a digest and when it is the destination of a copy.  It
 * needs EVP_PKEY_free to count references, which is another module's
 * function: when that is not yet written the checks are skipped.
 */
static void
test_digest_key(void)
{
	struct evp_pkey_st *key;
	EVP_MD_CTX *ctx;
	EVP_MD_CTX *source;
	uint8_t out[32];
	int implemented;

	/* A key made by hand, with two references: one is the test's. */
	key = calloc(1, sizeof(*key));
	CHECK(key != NULL);
	if (key == NULL)
		return;
	key->type = CRYC_PKEY_ED25519;

	/* Probes whether EVP_PKEY_free drops a reference. */
	key->references = 2;
	EVP_PKEY_free(key);
	implemented = key->references == 1;
	if (!implemented) {
		printf("note: EVP_PKEY_free does not count references yet; the key release checks are skipped\n");
		free(key);
		return;
	}

	/* Freeing a signing context drops the key. */
	ctx = EVP_MD_CTX_new();
	CHECK(ctx != NULL);
	if (ctx == NULL) {
		free(key);
		return;
	}
	key->references = 2;
	ctx->mode = CRYC_MD_SIGN;
	ctx->key = (EVP_PKEY *)key;
	EVP_MD_CTX_free(ctx);
	CHECK(key->references == 1);

	/* A signing or verification context refuses the digest functions and keeps the key. */
	ctx = EVP_MD_CTX_new();
	source = EVP_MD_CTX_new();
	CHECK(ctx != NULL && source != NULL);
	if (ctx == NULL || source == NULL) {
		EVP_MD_CTX_free(ctx);
		EVP_MD_CTX_free(source);
		free(key);
		return;
	}
	key->references = 2;
	ctx->mode = CRYC_MD_VERIFY;
	ctx->key = (EVP_PKEY *)key;
	REFUSED(EVP_DigestUpdate(ctx, "a", 1U), 0, R_NOT_SET_UP);
	REFUSED(EVP_DigestFinal_ex(ctx, out, NULL), 0, R_NOT_SET_UP);
	REFUSED(EVP_MD_CTX_copy_ex(source, ctx), 0, R_NOT_SET_UP);
	CHECK(key->references == 2);
	CHECK(ctx->key == (EVP_PKEY *)key);

	/* A refused start keeps the key; a start drops it and makes the context a digest. */
	REFUSED(EVP_DigestInit_ex(ctx, NULL, NULL), 0, R_NULL);
	CHECK(key->references == 2);
	OK(EVP_DigestInit_ex(ctx, EVP_sha256(), NULL));
	CHECK(key->references == 1);
	CHECK(ctx->key == NULL);
	CHECK(ctx->mode == CRYC_MD_DIGEST);

	/* The destination of a copy drops its key too. */
	OK(EVP_DigestInit_ex(source, EVP_sha256(), NULL));
	OK(EVP_DigestUpdate(source, "abc", 3U));
	key->references = 2;
	ctx->mode = CRYC_MD_SIGN;
	ctx->key = (EVP_PKEY *)key;
	OK(EVP_MD_CTX_copy_ex(ctx, source));
	CHECK(key->references == 1);
	CHECK(ctx->key == NULL);
	CHECK(ctx->mode == CRYC_MD_DIGEST);
	OK(EVP_DigestFinal_ex(ctx, out, NULL));
	OK(EVP_DigestFinal_ex(source, out, NULL));

	EVP_MD_CTX_free(ctx);
	EVP_MD_CTX_free(source);
	free(key);
}

/* ------------------------------------------------------------------ */
/* ChaCha20                                                             */
/* ------------------------------------------------------------------ */

/*
 * The reference keystream: size bytes from the block at the 64-bit
 * counter, each block from the block function with the counter's low half
 * as word 12 and the counter's high half followed by the 8-byte nonce as
 * the 12-byte nonce of words 13 to 15.
 */
static void
reference_stream(
	const uint8_t *key,
	uint64_t counter,
	const uint8_t *nonce8,
	uint8_t *stream,
	size_t size)
{
	uint8_t block_nonce[12];
	uint8_t block[64];
	uint64_t current;
	size_t done;
	size_t take;

	/* Each block with its own counter. */
	done = 0;
	current = counter;
	while (done < size) {
		write_le32(block_nonce, (uint32_t)(current >> 32));
		memcpy(block_nonce + 4, nonce8, 8U);
		cryc_chacha20_block(key, (uint32_t)(current & 0xffffffffU), block_nonce, block);
		take = size - done;
		if (take > 64U)
			take = 64U;
		memcpy(stream + done, block, take);
		done += take;
		current++;
	}
}

/* Builds the 16-byte IV of EVP_chacha20: the 64-bit little-endian counter, then the 8-byte nonce. */
static void
make_iv(
	uint8_t *iv,
	uint64_t counter,
	const uint8_t *nonce8)
{
	/* State words 12 and 13 are the counter, 14 and 15 the nonce. */
	write_le64(iv, counter);
	memcpy(iv + 8, nonce8, 8U);
}

/* Makes a cipher context for the key and IV and checks the setup; NULL when it failed. */
static EVP_CIPHER_CTX *
new_cipher(
	const uint8_t *key,
	const uint8_t *iv)
{
	EVP_CIPHER_CTX *ctx;

	/* One call sets the cipher, the key and the IV. */
	ctx = EVP_CIPHER_CTX_new();
	CHECK(ctx != NULL);
	if (ctx == NULL)
		return NULL;
	OK(EVP_CipherInit_ex(ctx, EVP_chacha20(), NULL, key, iv, 1));
	return ctx;
}

/*
 * Transforms size bytes through a context in chunks of random sizes and
 * checks them against the reference keystream.  style 0 is EVP_CipherUpdate
 * out of place, 1 is EVP_CipherUpdate in place, 2 is EVP_Cipher out of
 * place.  The bytes after the output are never written.
 */
static void
check_stream(
	const uint8_t *key,
	uint64_t counter,
	const uint8_t *nonce8,
	const uint8_t *plain,
	size_t size,
	unsigned int style,
	size_t max_chunk)
{
	uint8_t stream[MESSAGE_BYTES * 2U];
	uint8_t expected[MESSAGE_BYTES * 2U];
	uint8_t buffer[MESSAGE_BYTES * 2U + 8U];
	uint8_t iv[16];
	EVP_CIPHER_CTX *ctx;
	size_t offset;
	size_t take;
	size_t index;
	int out_length;
	int returned;
	int ok;

	/* The expected result and the buffer, whose guard bytes mark what is never written. */
	reference_stream(key, counter, nonce8, stream, size);
	for (index = 0; index < size; index++)
		expected[index] = (uint8_t)(plain[index] ^ stream[index]);
	memset(buffer, SENTINEL_BYTE, sizeof(buffer));
	if (style == 1U)
		memcpy(buffer, plain, size);
	make_iv(iv, counter, nonce8);
	ctx = new_cipher(key, iv);
	if (ctx == NULL)
		return;

	/* Chunk after chunk, among them empty ones. */
	offset = 0;
	while (offset < size) {
		take = random_below(max_chunk + 1U);
		if (take > size - offset)
			take = size - offset;
		out_length = -5;
		if (style == 0U) {
			OK(EVP_CipherUpdate(ctx, buffer + offset, &out_length, plain + offset, (int)take));
			CHECK(out_length == (int)take);
		} else if (style == 1U) {
			OK(EVP_CipherUpdate(ctx, buffer + offset, &out_length, buffer + offset, (int)take));
			CHECK(out_length == (int)take);
		} else {
			ERR_clear_error();
			returned = EVP_Cipher(ctx, buffer + offset, plain + offset, (unsigned int)take);
			CHECK(returned == (int)take);
			CHECK(ERR_peek_error() == 0UL);
		}
		offset += take;

		/* Nothing beyond the chunk is written (an in-place buffer holds the rest of the message there). */
		if (style != 1U)
			CHECK(all_equal(buffer + offset, size + 8U - offset, SENTINEL_BYTE));
	}

	/* The whole result, the position kept in the context, and the guard after the output. */
	ok = memcmp(buffer, expected, size) == 0;
	CHECK(ok);
	if (!ok)
		check_print_hex("got     ", buffer, size);
	CHECK(all_equal(buffer + size, 8U, SENTINEL_BYTE));
	CHECK(ctx->available == (64U - size % 64U) % 64U);
	EVP_CIPHER_CTX_free(ctx);
}

/* Checks one vector of the RFC: the 12-byte nonce and 32-bit counter mapped to the IV, encrypting and decrypting out of place and in place. */
static void
check_rfc_vector(
	const char *key_hex,
	const char *nonce_hex,
	uint32_t counter,
	const char *plain_hex,
	const char *cipher_hex)
{
	uint8_t key[32];
	uint8_t nonce[12];
	uint8_t iv[16];
	uint8_t plain[VECTOR_BYTES];
	uint8_t cipher[VECTOR_BYTES];
	uint8_t output[VECTOR_BYTES + 8U];
	EVP_CIPHER_CTX *ctx;
	size_t size;
	size_t cipher_size;
	int out_length;

	/* The inputs; the IV is the 32-bit counter then the 12-byte nonce (state words 12 to 15). */
	CHECK(unhex(key_hex, key, sizeof(key)) == 32U);
	CHECK(unhex(nonce_hex, nonce, sizeof(nonce)) == 12U);
	size = unhex(plain_hex, plain, sizeof(plain));
	cipher_size = unhex(cipher_hex, cipher, sizeof(cipher));
	CHECK(size == cipher_size);
	write_le32(iv, counter);
	memcpy(iv + 4, nonce, 12U);

	/* Out of place with a guard after the output. */
	memset(output, SENTINEL_BYTE, sizeof(output));
	ctx = new_cipher(key, iv);
	if (ctx == NULL)
		return;
	out_length = -1;
	OK(EVP_CipherUpdate(ctx, output, &out_length, plain, (int)size));
	CHECK(out_length == (int)size);
	CHECK_BYTES(output, cipher, size);
	CHECK(all_equal(output + size, 8U, SENTINEL_BYTE));
	EVP_CIPHER_CTX_free(ctx);

	/* Decrypting the ciphertext in place gives the plaintext (the same keystream). */
	memcpy(output, cipher, size);
	ctx = new_cipher(key, iv);
	if (ctx == NULL)
		return;
	OK(EVP_CipherInit_ex(ctx, NULL, NULL, NULL, NULL, 0));
	OK(EVP_CipherUpdate(ctx, output, &out_length, output, (int)size));
	CHECK_BYTES(output, plain, size);
	EVP_CIPHER_CTX_free(ctx);
}

/* Checks ChaCha20 against the examples and vectors of RFC 8439 through the EVP interface. */
static void
test_cipher_rfc(void)
{
	uint8_t key[32];
	uint8_t iv[16];
	uint8_t nonce[12];
	uint8_t expected[64];
	uint8_t got[64];
	uint8_t zeros[64];
	EVP_CIPHER_CTX *ctx;
	size_t index;
	int out_length;

	/* 2.3.2: one block at counter 1 (the key 00 01 ... 1f, the nonce 00 00 00 09 00 00 00 4a 00 00 00 00), as the keystream of 64 zero bytes. */
	CHECK(unhex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", key, sizeof(key)) == 32U);
	CHECK(unhex("000000090000004a00000000", nonce, sizeof(nonce)) == 12U);
	CHECK(unhex(rfc_block_serialized, expected, sizeof(expected)) == 64U);
	write_le32(iv, 1U);
	memcpy(iv + 4, nonce, 12U);
	memset(zeros, 0, sizeof(zeros));
	ctx = new_cipher(key, iv);
	if (ctx != NULL) {
		OK(EVP_CipherUpdate(ctx, got, &out_length, zeros, 64));
		CHECK_BYTES(got, expected, 64U);
		EVP_CIPHER_CTX_free(ctx);
	}

	/* A.1: the five block vectors. */
	for (index = 0; index < sizeof(a1_vectors) / sizeof(a1_vectors[0]); index++) {
		check_rfc_vector(a1_vectors[index].key, a1_vectors[index].nonce, a1_vectors[index].counter, ZERO_64_BYTES, a1_vectors[index].keystream);
	}

	/* 2.4.2: the "Sunscreen" text at counter 1 with the nonce 00 00 00 00 00 00 00 4a 00 00 00 00. */
	check_rfc_vector("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", "000000000000004a00000000", 1U, rfc_sunscreen_plain, rfc_sunscreen_cipher);

	/* A.2: the three encryption vectors. */
	for (index = 0; index < sizeof(a2_vectors) / sizeof(a2_vectors[0]); index++)
		check_rfc_vector(a2_vectors[index].key, a2_vectors[index].nonce, a2_vectors[index].counter, a2_vectors[index].plain, a2_vectors[index].cipher);

	/* 2.6.2 and A.4: the first 32 bytes of block 0 are the Poly1305 key. */
	for (index = 0; index < sizeof(a4_vectors) / sizeof(a4_vectors[0]); index++) {
		check_rfc_vector(a4_vectors[index].key, a4_vectors[index].nonce, 0U, ZERO_32_BYTES, a4_vectors[index].onetime);
	}
	check_rfc_vector(rfc_keygen_key, "000000000001020304050607", 0U, ZERO_32_BYTES, rfc_keygen_output);
}

/* Checks streams of many lengths, chunk sizes and positions against the reference keystream, with every 64-bit counter of interest. */
static void
test_cipher_stream(void)
{
	static const uint64_t counters[] = {
		CRYC_U64(0x00000000U, 0x00000000U),
		CRYC_U64(0x00000000U, 0x00000001U),
		CRYC_U64(0x00000000U, 0xfffffffeU),
		CRYC_U64(0x00000000U, 0xffffffffU),
		CRYC_U64(0x00000001U, 0x00000000U),
		CRYC_U64(0x00000001U, 0xfffffffeU),
		CRYC_U64(0x12345678U, 0xfffffffdU),
		CRYC_U64(0xfffffffeU, 0xffffffffU),
		CRYC_U64(0xffffffffU, 0xfffffff0U),
		CRYC_U64(0xffffffffU, 0xfffffffeU),
		CRYC_U64(0xffffffffU, 0xffffffffU)
	};
	static const size_t sizes[] = {
		0U, 1U, 2U, 15U, 16U, 17U, 31U, 32U, 33U, 63U, 64U, 65U, 66U, 127U, 128U, 129U,
		191U, 192U, 193U, 255U, 256U, 257U, 1000U
	};
	static const size_t chunks[] = { 1U, 3U, 17U, 63U, 64U, 65U, 200U, 1100U };
	uint8_t key[32];
	uint8_t nonce[8];
	uint8_t plain[MESSAGE_BYTES];
	uint64_t remaining;
	uint64_t counter;
	size_t size;
	size_t limit;
	size_t round;
	size_t index;
	unsigned int style;

	/* Every counter and every boundary size, each in the three styles with a chunk size from the list. */
	for (round = 0; round < 3U; round++) {
		for (index = 0; index < sizeof(counters) / sizeof(counters[0]); index++) {
			random_fill(key, sizeof(key));
			random_fill(nonce, sizeof(nonce));
			for (size = 0; size < sizeof(sizes) / sizeof(sizes[0]); size++) {
				limit = sizes[size];
				remaining = (uint64_t)0 - counters[index];
				if (counters[index] != 0 && remaining < (uint64_t)((limit + 63U) / 64U))
					limit = (size_t)(remaining * 64U);
				random_fill(plain, limit);
				style = (unsigned int)((round + size) % 3U);
				check_stream(key, counters[index], nonce, plain, limit, style, chunks[(round + size + index) % (sizeof(chunks) / sizeof(chunks[0]))]);
			}
		}
	}

	/* Random keys, nonces, counters and sizes. */
	for (round = 0; round < 300U; round++) {
		random_fill(key, sizeof(key));
		random_fill(nonce, sizeof(nonce));
		counter = CRYC_U64(random_next(), random_next());
		if ((round % 4U) == 0U)
			counter = (uint64_t)0 - (uint64_t)random_below(40U) - 1U;
		size = random_below(MESSAGE_BYTES);
		if (counter != 0 && (uint64_t)0 - counter < (uint64_t)((size + 63U) / 64U))
			size = (size_t)(((uint64_t)0 - counter) * 64U);
		random_fill(plain, size);
		check_stream(key, counter, nonce, plain, size, (unsigned int)(round % 3U), 1U + random_below(150U));
	}
}

/* Checks the end of the counter's range: the last block can be used, nothing past it, and a refused call changes nothing. */
static void
test_cipher_exhaustion(void)
{
	uint8_t key[32];
	uint8_t key_two[32];
	uint8_t nonce[8];
	uint8_t iv[16];
	uint8_t stream[256];
	uint8_t stream_two[64];
	uint8_t input[300];
	uint8_t output[300];
	EVP_CIPHER_CTX *ctx;
	int out_length;
	size_t index;

	/* One key and nonce; the last blocks of the range as the reference. */
	random_fill(key, sizeof(key));
	random_fill(key_two, sizeof(key_two));
	random_fill(nonce, sizeof(nonce));
	memset(input, 0, sizeof(input));

	/* Counter 2^64 - 1: 10 bytes, then the other 54 of the block, are fine; one byte more is refused. */
	reference_stream(key, CRYC_U64(0xffffffffU, 0xffffffffU), nonce, stream, 64U);
	make_iv(iv, CRYC_U64(0xffffffffU, 0xffffffffU), nonce);
	ctx = new_cipher(key, iv);
	if (ctx == NULL)
		return;
	memset(output, SENTINEL_BYTE, sizeof(output));
	OK(EVP_CipherUpdate(ctx, output, &out_length, input, 10));
	CHECK_BYTES(output, stream, 10U);
	CHECK(ctx->available == 54U);
	OK(EVP_CipherUpdate(ctx, output + 10, &out_length, input + 10, 54));
	CHECK_BYTES(output, stream, 64U);
	CHECK(ctx->available == 0U);
	out_length = 99;
	REFUSED(EVP_CipherUpdate(ctx, output + 64, &out_length, input, 1), 0, R_ARGUMENT);
	CHECK(out_length == 99);
	CHECK(output[64] == SENTINEL_BYTE);
	REFUSED(EVP_Cipher(ctx, output + 64, input, 1U), -1, R_ARGUMENT);
	CHECK(output[64] == SENTINEL_BYTE);

	/* An empty update is still fine, a new IV starts again, and a refusal wrote nothing and kept the position. */
	OK(EVP_CipherUpdate(ctx, NULL, &out_length, NULL, 0));
	CHECK(out_length == 0);
	make_iv(iv, 0, nonce);
	OK(EVP_CipherInit_ex(ctx, NULL, NULL, NULL, iv, -1));
	reference_stream(key, 0, nonce, stream, 64U);
	OK(EVP_CipherUpdate(ctx, output, &out_length, input, 64));
	CHECK_BYTES(output, stream, 64U);
	EVP_CIPHER_CTX_free(ctx);

	/* A request of 65 bytes at counter 2^64 - 1 is refused whole; 64 bytes after it still work (the position was not moved). */
	make_iv(iv, CRYC_U64(0xffffffffU, 0xffffffffU), nonce);
	ctx = new_cipher(key, iv);
	if (ctx == NULL)
		return;
	memset(output, SENTINEL_BYTE, sizeof(output));
	REFUSED(EVP_CipherUpdate(ctx, output, &out_length, input, 65), 0, R_ARGUMENT);
	CHECK(all_equal(output, sizeof(output), SENTINEL_BYTE));
	reference_stream(key, CRYC_U64(0xffffffffU, 0xffffffffU), nonce, stream, 64U);
	OK(EVP_CipherUpdate(ctx, output, &out_length, input, 64));
	CHECK_BYTES(output, stream, 64U);
	EVP_CIPHER_CTX_free(ctx);

	/* Counter 2^64 - 2: 100 bytes run into the last block of the range, 28 more bytes of it remain, 29 are refused. */
	make_iv(iv, CRYC_U64(0xffffffffU, 0xfffffffeU), nonce);
	reference_stream(key, CRYC_U64(0xffffffffU, 0xfffffffeU), nonce, stream, 128U);
	ctx = new_cipher(key, iv);
	if (ctx == NULL)
		return;
	memset(output, SENTINEL_BYTE, sizeof(output));
	OK(EVP_CipherUpdate(ctx, output, &out_length, input, 100));
	CHECK_BYTES(output, stream, 100U);
	REFUSED(EVP_CipherUpdate(ctx, output + 100, &out_length, input, 29), 0, R_ARGUMENT);
	CHECK(all_equal(output + 100, sizeof(output) - 100U, SENTINEL_BYTE));
	OK(EVP_CipherUpdate(ctx, output + 100, &out_length, input, 28));
	CHECK_BYTES(output, stream, 128U);
	REFUSED(EVP_CipherUpdate(ctx, output + 128, &out_length, input, 1), 0, R_ARGUMENT);

	/* After the range is used up a new key alone goes on with counter 0 (the wrapped counter) and the nonce. */
	OK(EVP_CipherInit_ex(ctx, NULL, NULL, key_two, NULL, -1));
	reference_stream(key_two, 0, nonce, stream_two, 64U);
	OK(EVP_CipherUpdate(ctx, output, &out_length, input, 64));
	CHECK_BYTES(output, stream_two, 64U);
	EVP_CIPHER_CTX_free(ctx);

	/* Counter 2^64 - 1 taken by a 64-byte request that starts at a block boundary: exactly the last block. */
	make_iv(iv, CRYC_U64(0xffffffffU, 0xffffffffU), nonce);
	ctx = new_cipher(key, iv);
	if (ctx == NULL)
		return;
	reference_stream(key, CRYC_U64(0xffffffffU, 0xffffffffU), nonce, stream, 64U);
	OK(EVP_CipherUpdate(ctx, output, &out_length, input, 64));
	CHECK_BYTES(output, stream, 64U);
	REFUSED(EVP_CipherUpdate(ctx, output, &out_length, input, 1), 0, R_ARGUMENT);
	EVP_CIPHER_CTX_free(ctx);

	/* Counter 2^64 - 3 in one request of 192 bytes (three whole blocks) is allowed, 193 is not. */
	make_iv(iv, CRYC_U64(0xffffffffU, 0xfffffffdU), nonce);
	reference_stream(key, CRYC_U64(0xffffffffU, 0xfffffffdU), nonce, stream, 192U);
	ctx = new_cipher(key, iv);
	if (ctx == NULL)
		return;
	REFUSED(EVP_CipherUpdate(ctx, output, &out_length, input, 193), 0, R_ARGUMENT);
	OK(EVP_CipherUpdate(ctx, output, &out_length, input, 192));
	CHECK_BYTES(output, stream, 192U);
	REFUSED(EVP_CipherUpdate(ctx, output, &out_length, input, 1), 0, R_ARGUMENT);
	EVP_CIPHER_CTX_free(ctx);

	/* A request that is a multiple of 16 but not of 64 counts a partial block too: at counter 2^64 - 1, 80 bytes need two blocks. */
	make_iv(iv, CRYC_U64(0xffffffffU, 0xffffffffU), nonce);
	ctx = new_cipher(key, iv);
	if (ctx == NULL)
		return;
	memset(output, SENTINEL_BYTE, sizeof(output));
	REFUSED(EVP_CipherUpdate(ctx, output, &out_length, input, 80), 0, R_ARGUMENT);
	CHECK(all_equal(output, sizeof(output), SENTINEL_BYTE));
	OK(EVP_CipherUpdate(ctx, output, &out_length, input, 16));
	EVP_CIPHER_CTX_free(ctx);

	/* After the last block is used up, a request of 16, 32, 48 or 80 bytes (no whole block, or more) is refused too. */
	for (index = 0; index < 4U; index++) {
		make_iv(iv, CRYC_U64(0xffffffffU, 0xffffffffU), nonce);
		ctx = new_cipher(key, iv);
		if (ctx == NULL)
			return;
		OK(EVP_CipherUpdate(ctx, output, &out_length, input, 64));
		memset(output, SENTINEL_BYTE, sizeof(output));
		REFUSED(EVP_CipherUpdate(ctx, output, &out_length, input, 16 + 16 * (int)index + (index == 3U ? 16 : 0)), 0, R_ARGUMENT);
		CHECK(all_equal(output, sizeof(output), SENTINEL_BYTE));
		EVP_CIPHER_CTX_free(ctx);
	}

	/* A counter of 0 with no earlier use is not mistaken for the end: a long request from it works. */
	make_iv(iv, 0, nonce);
	ctx = new_cipher(key, iv);
	if (ctx != NULL) {
		for (index = 0; index < 4U; index++)
			OK(EVP_CipherUpdate(ctx, output, &out_length, input, 300));
		EVP_CIPHER_CTX_free(ctx);
	}
}

/* Checks the set-up function: which arguments are required, kept, restarted, and refused. */
static void
test_cipher_setup(void)
{
	uint8_t key[32];
	uint8_t key_two[32];
	uint8_t nonce[8];
	uint8_t nonce_two[8];
	uint8_t zero_nonce[8];
	uint8_t iv[16];
	uint8_t iv_two[16];
	uint8_t stream[256];
	uint8_t input[256];
	uint8_t output[256];
	uint8_t guard[256];
	EVP_CIPHER_CTX *ctx;
	int out_length;
	int enc;
	int dummy_engine;

	/* The inputs. */
	random_fill(key, sizeof(key));
	random_fill(key_two, sizeof(key_two));
	random_fill(nonce, sizeof(nonce));
	random_fill(nonce_two, sizeof(nonce_two));
	memset(input, 0, sizeof(input));
	make_iv(iv, CRYC_U64(0x00000003U, 0x00000007U), nonce);
	make_iv(iv_two, CRYC_U64(0x00000000U, 0x00000009U), nonce_two);

	/* The algorithm is one constant object. */
	CHECK(EVP_chacha20() != NULL);
	CHECK(EVP_chacha20() == EVP_chacha20());

	/* A new context is not set up: every use is refused until a key is given. */
	ctx = EVP_CIPHER_CTX_new();
	CHECK(ctx != NULL);
	if (ctx == NULL)
		return;
	memset(output, SENTINEL_BYTE, sizeof(output));
	memcpy(guard, output, sizeof(output));
	out_length = 99;
	REFUSED(EVP_CipherUpdate(ctx, output, &out_length, input, 16), 0, R_NOT_SET_UP);
	CHECK(out_length == 99);
	REFUSED(EVP_Cipher(ctx, output, input, 16U), -1, R_NOT_SET_UP);
	CHECK_BYTES(output, guard, sizeof(output));

	/* Refused set-ups: no context, an engine, another cipher, no cipher at all. */
	REFUSED(EVP_CipherInit_ex(NULL, EVP_chacha20(), NULL, key, iv, 1), 0, R_NULL);
	REFUSED(EVP_CipherInit_ex(ctx, EVP_chacha20(), (ENGINE *)&dummy_engine, key, iv, 1), 0, R_ARGUMENT);
	REFUSED(EVP_CipherInit_ex(ctx, &fake_cipher, NULL, key, iv, 1), 0, R_ALGORITHM);
	REFUSED(EVP_CipherInit_ex(ctx, NULL, NULL, key, iv, 1), 0, R_NULL);
	CHECK(ctx->cipher == NULL && ctx->key_set == 0);

	/* The cipher alone, or the cipher and an IV, set up nothing usable; then the key; then a use with no IV runs with the zero IV. */
	OK(EVP_CipherInit_ex(ctx, EVP_chacha20(), NULL, NULL, NULL, 1));
	REFUSED(EVP_CipherUpdate(ctx, output, &out_length, input, 16), 0, R_NOT_SET_UP);
	OK(EVP_CipherInit_ex(ctx, NULL, NULL, NULL, iv, 1));
	CHECK(ctx->key_set == 0);
	REFUSED(EVP_CipherUpdate(ctx, output, &out_length, input, 16), 0, R_NOT_SET_UP);
	REFUSED(EVP_Cipher(ctx, output, input, 16U), -1, R_NOT_SET_UP);
	OK(EVP_CipherInit_ex(ctx, NULL, NULL, key, NULL, 1));
	reference_stream(key, CRYC_U64(0x00000003U, 0x00000007U), nonce, stream, 100U);
	OK(EVP_CipherUpdate(ctx, output, &out_length, input, 100));
	CHECK_BYTES(output, stream, 100U);

	/* A fresh context given a key and no IV at all runs with the zero IV. */
	EVP_CIPHER_CTX_free(ctx);
	ctx = EVP_CIPHER_CTX_new();
	CHECK(ctx != NULL);
	if (ctx == NULL)
		return;
	OK(EVP_CipherInit_ex(ctx, EVP_chacha20(), NULL, key, NULL, 1));
	memset(zero_nonce, 0, sizeof(zero_nonce));
	reference_stream(key, 0, zero_nonce, stream, 100U);
	OK(EVP_CipherUpdate(ctx, output, &out_length, input, 100));
	CHECK_BYTES(output, stream, 100U);

	/* An IV alone restarts the keystream with the same key, dropping the buffered rest of the block. */
	OK(EVP_CipherInit_ex(ctx, NULL, NULL, NULL, iv, -1));
	CHECK(ctx->available == 0U);
	reference_stream(key, CRYC_U64(0x00000003U, 0x00000007U), nonce, stream, 100U);
	OK(EVP_CipherUpdate(ctx, output, &out_length, input, 100));
	CHECK_BYTES(output, stream, 100U);
	OK(EVP_CipherInit_ex(ctx, NULL, NULL, NULL, iv, -1));
	OK(EVP_CipherUpdate(ctx, output, &out_length, input, 100));
	CHECK_BYTES(output, stream, 100U);

	/* A key alone, in the middle of a block, drops the buffered rest and keeps the counter and the nonce: the next block of the new key. */
	OK(EVP_CipherInit_ex(ctx, NULL, NULL, NULL, iv, -1));
	OK(EVP_CipherUpdate(ctx, output, &out_length, input, 10));
	CHECK(ctx->available == 54U);
	OK(EVP_CipherInit_ex(ctx, NULL, NULL, key_two, NULL, -1));
	CHECK(ctx->available == 0U);
	CHECK(all_equal(ctx->keystream, sizeof(ctx->keystream), 0));
	reference_stream(key_two, CRYC_U64(0x00000003U, 0x00000008U), nonce, stream, 64U);
	OK(EVP_CipherUpdate(ctx, output, &out_length, input, 64));
	CHECK_BYTES(output, stream, 64U);

	/* A key and an IV together, with every direction value (the keystream is the same). */
	for (enc = -1; enc <= 2; enc++) {
		OK(EVP_CipherInit_ex(ctx, EVP_chacha20(), NULL, key, iv_two, enc));
		reference_stream(key, CRYC_U64(0x00000000U, 0x00000009U), nonce_two, stream, 50U);
		OK(EVP_CipherUpdate(ctx, output, &out_length, input, 50));
		CHECK_BYTES(output, stream, 50U);
	}

	/* A refused set-up changes nothing: the stream goes on where it was. */
	OK(EVP_CipherInit_ex(ctx, EVP_chacha20(), NULL, key, iv, 1));
	OK(EVP_CipherUpdate(ctx, output, &out_length, input, 20));
	REFUSED(EVP_CipherInit_ex(ctx, &fake_cipher, NULL, key_two, iv_two, 1), 0, R_ALGORITHM);
	REFUSED(EVP_CipherInit_ex(ctx, NULL, (ENGINE *)&dummy_engine, key_two, iv_two, 1), 0, R_ARGUMENT);
	reference_stream(key, CRYC_U64(0x00000003U, 0x00000007U), nonce, stream, 120U);
	OK(EVP_CipherUpdate(ctx, output + 20, &out_length, input, 100));
	CHECK_BYTES(output + 20, stream + 20, 100U);

	/* Refused updates change nothing: a missing length output, a negative length, a missing buffer. */
	OK(EVP_CipherInit_ex(ctx, NULL, NULL, NULL, iv, -1));
	memset(output, SENTINEL_BYTE, sizeof(output));
	memcpy(guard, output, sizeof(output));
	out_length = 99;
	REFUSED(EVP_CipherUpdate(NULL, output, &out_length, input, 16), 0, R_NULL);
	REFUSED(EVP_CipherUpdate(ctx, output, NULL, input, 16), 0, R_NULL);
	REFUSED(EVP_CipherUpdate(ctx, output, &out_length, input, -1), 0, R_ARGUMENT);
	REFUSED(EVP_CipherUpdate(ctx, NULL, &out_length, input, 16), 0, R_NULL);
	REFUSED(EVP_CipherUpdate(ctx, output, &out_length, NULL, 16), 0, R_NULL);
	REFUSED(EVP_Cipher(NULL, output, input, 16U), -1, R_NULL);
	REFUSED(EVP_Cipher(ctx, NULL, input, 16U), -1, R_NULL);
	REFUSED(EVP_Cipher(ctx, output, NULL, 16U), -1, R_NULL);
	REFUSED(EVP_Cipher(ctx, output, input, 0x80000000U), -1, R_ARGUMENT);
	REFUSED(EVP_Cipher(ctx, output, input, 0xffffffffU), -1, R_ARGUMENT);
	REFUSED(EVP_Cipher(ctx, NULL, NULL, (unsigned int)INT_MAX), -1, R_NULL);
	REFUSED(EVP_Cipher(ctx, NULL, NULL, (unsigned int)INT_MAX + 1U), -1, R_ARGUMENT);
	CHECK(out_length == 99);
	CHECK_BYTES(output, guard, sizeof(output));
	reference_stream(key, CRYC_U64(0x00000003U, 0x00000007U), nonce, stream, 100U);
	OK(EVP_CipherUpdate(ctx, output, &out_length, input, 100));
	CHECK_BYTES(output, stream, 100U);

	/* An empty request needs no buffers, reports 0 and keeps the position; EVP_Cipher returns 0. */
	out_length = 99;
	OK(EVP_CipherUpdate(ctx, NULL, &out_length, NULL, 0));
	CHECK(out_length == 0);
	ERR_clear_error();
	CHECK(EVP_Cipher(ctx, NULL, NULL, 0U) == 0);
	CHECK(ERR_peek_error() == 0UL);
	CHECK(ctx->available == 28U);

	/* EVP_Cipher returns the length it was given. */
	ERR_clear_error();
	CHECK(EVP_Cipher(ctx, output, input, 28U) == 28);
	CHECK(EVP_Cipher(ctx, output, input, 200U) == 200);
	CHECK(ERR_peek_error() == 0UL);

	/* The consumed keystream is wiped as soon as a block is used up (here a block taken in two calls). */
	OK(EVP_CipherInit_ex(ctx, NULL, NULL, NULL, iv, -1));
	OK(EVP_CipherUpdate(ctx, output, &out_length, input, 10));
	CHECK(ctx->available == 54U);
	CHECK(!all_equal(ctx->keystream, sizeof(ctx->keystream), 0));
	OK(EVP_CipherUpdate(ctx, output + 10, &out_length, input + 10, 54));
	CHECK(ctx->available == 0U);
	CHECK(all_equal(ctx->keystream, sizeof(ctx->keystream), 0));
	reference_stream(key, CRYC_U64(0x00000003U, 0x00000007U), nonce, stream, 64U);
	CHECK_BYTES(output, stream, 64U);


	EVP_CIPHER_CTX_free(ctx);
	CHECK(ERR_peek_error() == 0UL);
}

/* ------------------------------------------------------------------ */
/* Poly1305                                                             */
/* ------------------------------------------------------------------ */

/*
 * Authenticates a message through a context that already exists: the same
 * result as the vector's tag whether it comes as one update, random chunks
 * with empty updates, or after an abandoned computation on the same
 * context.
 */
static void
check_mac_message(
	EVP_MAC_CTX *ctx,
	const uint8_t *key,
	const uint8_t *message,
	size_t size,
	const uint8_t *expected)
{
	uint8_t tag[24];
	size_t tag_length;
	size_t offset;
	size_t take;

	/* One update; the tag is 16 bytes and nothing is written past them. */
	OK(EVP_MAC_init(ctx, key, 32U, NULL));
	CHECK(ctx->started == 1);
	OK(EVP_MAC_update(ctx, message, size));
	memset(tag, SENTINEL_BYTE, sizeof(tag));
	tag_length = 99U;
	OK(EVP_MAC_final(ctx, tag, &tag_length, sizeof(tag)));
	CHECK(tag_length == 16U);
	CHECK_BYTES(tag, expected, 16U);
	CHECK(all_equal(tag + 16, 8U, SENTINEL_BYTE));

	/* After the final the context is finished and its state, which holds the key, is wiped. */
	CHECK(ctx->started == 0);
	CHECK(all_equal((const uint8_t *)&ctx->poly, sizeof(ctx->poly), 0));

	/* Random chunks with empty updates; the length output and the exact room are fine. */
	OK(EVP_MAC_init(ctx, key, 32U, NULL));
	offset = 0;
	while (offset < size) {
		take = random_below(40U);
		if (take > size - offset)
			take = size - offset;
		if (take == 0)
			OK(EVP_MAC_update(ctx, NULL, 0U));
		else
			OK(EVP_MAC_update(ctx, message + offset, take));
		offset += take;
	}
	memset(tag, SENTINEL_BYTE, sizeof(tag));
	OK(EVP_MAC_final(ctx, tag, NULL, 16U));
	CHECK_BYTES(tag, expected, 16U);
	CHECK(all_equal(tag + 16, 8U, SENTINEL_BYTE));

	/* A new start discards a computation that was in progress on the same context. */
	OK(EVP_MAC_init(ctx, key, 32U, NULL));
	OK(EVP_MAC_update(ctx, (const unsigned char *)"abandoned", 9U));
	OK(EVP_MAC_init(ctx, key, 32U, NULL));
	OK(EVP_MAC_update(ctx, message, size));
	memset(tag, SENTINEL_BYTE, sizeof(tag));
	OK(EVP_MAC_final(ctx, tag, &tag_length, 16U));
	CHECK_BYTES(tag, expected, 16U);
}

/* Checks Poly1305 against the vectors of RFC 8439 (2.5.2 and A.3) through the EVP interface. */
static void
test_mac_rfc(void)
{
	uint8_t key[32];
	uint8_t message[VECTOR_BYTES];
	uint8_t tag[16];
	EVP_MAC *mac;
	EVP_MAC_CTX *ctx;
	size_t size;
	size_t index;

	/* One context reused for every vector. */
	mac = EVP_MAC_fetch(NULL, "POLY1305", NULL);
	CHECK(mac != NULL);
	if (mac == NULL)
		return;
	ctx = EVP_MAC_CTX_new(mac);
	CHECK(ctx != NULL);
	if (ctx == NULL) {
		EVP_MAC_free(mac);
		return;
	}

	/* 2.5.2. */
	CHECK(unhex(rfc_poly_key, key, sizeof(key)) == 32U);
	size = unhex(rfc_poly_message, message, sizeof(message));
	CHECK(unhex(rfc_poly_tag, tag, sizeof(tag)) == 16U);
	check_mac_message(ctx, key, message, size, tag);

	/* A.3: the eleven vectors. */
	for (index = 0; index < sizeof(a3_vectors) / sizeof(a3_vectors[0]); index++) {
		CHECK(unhex(a3_vectors[index].key, key, sizeof(key)) == 32U);
		size = unhex(a3_vectors[index].message, message, sizeof(message));
		CHECK(unhex(a3_vectors[index].tag, tag, sizeof(tag)) == 16U);
		check_mac_message(ctx, key, message, size, tag);
	}

	EVP_MAC_CTX_free(ctx);
	EVP_MAC_free(mac);
}

/* Checks the fetch, the context and the refusals of the MAC functions. */
static void
test_mac_api(void)
{
	static const char *const refused_names[] = {
		"POLY1306", "POLY130", "POLY13055", "", "POLY1305 ", " POLY1305", "HMAC", "CMAC", "POLY-1305", "poly1305x", "XPOLY1305"
	};
	static const char *const accepted_names[] = {
		"POLY1305", "poly1305", "Poly1305", "pOlY1305"
	};
	uint8_t key[32];
	uint8_t other_key[32];
	uint8_t expected[16];
	uint8_t tag[40];
	uint8_t guard[40];
	EVP_MAC *mac;
	EVP_MAC *again;
	EVP_MAC_CTX *ctx;
	OSSL_PARAM params[1];
	struct cryc_poly1305 reference;
	size_t tag_length;
	size_t length;
	size_t index;
	int dummy_context;

	/* The accepted names (not case sensitive) give the one algorithm object, which can be released any number of times. */
	mac = EVP_MAC_fetch(NULL, "POLY1305", NULL);
	CHECK(mac != NULL);
	if (mac == NULL)
		return;
	for (index = 0; index < sizeof(accepted_names) / sizeof(accepted_names[0]); index++) {
		ERR_clear_error();
		again = EVP_MAC_fetch(NULL, accepted_names[index], NULL);
		CHECK(again == mac);
		CHECK(ERR_peek_error() == 0UL);
		EVP_MAC_free(again);
	}

	/* Every other name, a library context, a property query or a missing name is refused. */
	for (index = 0; index < sizeof(refused_names) / sizeof(refused_names[0]); index++)
		REFUSED(EVP_MAC_fetch(NULL, refused_names[index], NULL), NULL, R_ALGORITHM);
	REFUSED(EVP_MAC_fetch(NULL, NULL, NULL), NULL, R_NULL);
	REFUSED(EVP_MAC_fetch((OSSL_LIB_CTX *)&dummy_context, "POLY1305", NULL), NULL, R_ARGUMENT);
	REFUSED(EVP_MAC_fetch(NULL, "POLY1305", "provider=default"), NULL, R_ARGUMENT);

	/* A context needs the fetched algorithm. */
	REFUSED(EVP_MAC_CTX_new(NULL), NULL, R_NULL);
	REFUSED(EVP_MAC_CTX_new(&fake_mac), NULL, R_ALGORITHM);
	ctx = EVP_MAC_CTX_new(mac);
	CHECK(ctx != NULL);
	if (ctx == NULL)
		return;
	CHECK(ctx->started == 0);

	/* Before a start: update and final are refused (the size query is not); the computation is never started by them. */
	memset(tag, SENTINEL_BYTE, sizeof(tag));
	memcpy(guard, tag, sizeof(tag));
	tag_length = 99U;
	REFUSED(EVP_MAC_update(ctx, (const unsigned char *)"a", 1U), 0, R_NOT_SET_UP);
	REFUSED(EVP_MAC_final(ctx, tag, &tag_length, sizeof(tag)), 0, R_NOT_SET_UP);
	CHECK(tag_length == 99U);
	CHECK_BYTES(tag, guard, sizeof(tag));
	tag_length = 99U;
	OK(EVP_MAC_final(ctx, NULL, &tag_length, 0U));
	CHECK(tag_length == 16U);
	CHECK(ctx->started == 0);

	/* Refused starts: no context, a parameter array, no key, a key of the wrong length. */
	random_fill(key, sizeof(key));
	random_fill(other_key, sizeof(other_key));
	memset(&params[0], 0, sizeof(params[0]));
	REFUSED(EVP_MAC_init(NULL, key, 32U, NULL), 0, R_NULL);
	REFUSED(EVP_MAC_init(ctx, key, 32U, params), 0, R_ARGUMENT);
	REFUSED(EVP_MAC_init(ctx, NULL, 32U, NULL), 0, R_NULL);
	REFUSED(EVP_MAC_init(ctx, NULL, 0U, NULL), 0, R_NULL);
	for (length = 0; length <= 64U; length++) {
		if (length == 32U)
			continue;
		REFUSED(EVP_MAC_init(ctx, key, length, NULL), 0, R_KEY_LENGTH);
	}
	REFUSED(EVP_MAC_init(ctx, key, (size_t)-1, NULL), 0, R_KEY_LENGTH);
	CHECK(ctx->started == 0);

	/* A refused start keeps a computation in progress: its tag is the tag of the first key and all the bytes. */
	OK(EVP_MAC_init(ctx, key, 32U, NULL));
	OK(EVP_MAC_update(ctx, (const unsigned char *)"hello ", 6U));
	REFUSED(EVP_MAC_init(ctx, other_key, 31U, NULL), 0, R_KEY_LENGTH);
	REFUSED(EVP_MAC_init(ctx, other_key, 32U, params), 0, R_ARGUMENT);
	REFUSED(EVP_MAC_update(NULL, (const unsigned char *)"x", 1U), 0, R_NULL);
	REFUSED(EVP_MAC_update(ctx, NULL, 3U), 0, R_NULL);
	OK(EVP_MAC_update(ctx, NULL, 0U));
	OK(EVP_MAC_update(ctx, (const unsigned char *)"world", 5U));
	cryc_poly1305_init(&reference, key);
	cryc_poly1305_update(&reference, (const uint8_t *)"hello world", 11U);
	cryc_poly1305_final(&reference, expected);

	/* Refused finals: no context, no room (too small), no output and no length; the computation stays. */
	memset(tag, SENTINEL_BYTE, sizeof(tag));
	memcpy(guard, tag, sizeof(tag));
	tag_length = 99U;
	REFUSED(EVP_MAC_final(NULL, tag, &tag_length, sizeof(tag)), 0, R_NULL);
	for (length = 0; length < 16U; length++)
		REFUSED(EVP_MAC_final(ctx, tag, &tag_length, length), 0, R_TOO_SMALL);
	REFUSED(EVP_MAC_final(ctx, NULL, NULL, 16U), 0, R_NULL);
	CHECK(tag_length == 99U);
	CHECK_BYTES(tag, guard, sizeof(tag));
	CHECK(ctx->started == 1);

	/* The size query leaves the computation in progress; then the tag, with room to spare. */
	tag_length = 99U;
	OK(EVP_MAC_final(ctx, NULL, &tag_length, 0U));
	CHECK(tag_length == 16U);
	CHECK(ctx->started == 1);
	tag_length = 99U;
	OK(EVP_MAC_final(ctx, tag, &tag_length, sizeof(tag)));
	CHECK(tag_length == 16U);
	CHECK_BYTES(tag, expected, 16U);
	CHECK(all_equal(tag + 16, sizeof(tag) - 16U, SENTINEL_BYTE));

	/* A finished context refuses update and final until it is started again; the tag was not changed. */
	REFUSED(EVP_MAC_update(ctx, (const unsigned char *)"x", 1U), 0, R_NOT_SET_UP);
	REFUSED(EVP_MAC_final(ctx, tag, &tag_length, sizeof(tag)), 0, R_NOT_SET_UP);

	/* A context freed in the middle of a computation is fine (no leak, no use after free). */
	OK(EVP_MAC_init(ctx, key, 32U, NULL));
	OK(EVP_MAC_update(ctx, (const unsigned char *)"half", 4U));
	EVP_MAC_CTX_free(ctx);
	EVP_MAC_free(mac);
	CHECK(ERR_peek_error() == 0UL);
}

/* ------------------------------------------------------------------ */
/* The constructions built on the EVP functions                         */
/* ------------------------------------------------------------------ */

/*
 * Seals with AEAD_CHACHA20_POLY1305 of RFC 8439 2.8 built from the EVP
 * functions only: block 0 of the keystream is the Poly1305 key, blocks
 * from 1 encrypt, and the tag covers the padded additional data and
 * ciphertext and their lengths.
 */
static void
aead_seal(
	const uint8_t *key,
	const uint8_t *nonce12,
	const uint8_t *aad,
	size_t aad_size,
	const uint8_t *plain,
	size_t size,
	uint8_t *cipher,
	uint8_t *tag)
{
	uint8_t iv[16];
	uint8_t one_time_key[32];
	uint8_t zeros[32];
	uint8_t padding[16];
	uint8_t lengths[16];
	EVP_CIPHER_CTX *cipher_ctx;
	EVP_MAC *mac;
	EVP_MAC_CTX *mac_ctx;
	size_t tag_length;
	size_t pad;
	int out_length;

	/* Block 0 gives the one-time key: the first 32 bytes of the keystream of 32 zero bytes. */
	write_le32(iv, 0U);
	memcpy(iv + 4, nonce12, 12U);
	memset(zeros, 0, sizeof(zeros));
	cipher_ctx = new_cipher(key, iv);
	if (cipher_ctx == NULL)
		return;
	OK(EVP_CipherUpdate(cipher_ctx, one_time_key, &out_length, zeros, 32));
	CHECK(out_length == 32);

	/* The same context, with a new IV, encrypts from block 1. */
	write_le32(iv, 1U);
	OK(EVP_CipherInit_ex(cipher_ctx, NULL, NULL, NULL, iv, -1));
	OK(EVP_CipherUpdate(cipher_ctx, cipher, &out_length, plain, (int)size));
	CHECK(out_length == (int)size);
	EVP_CIPHER_CTX_free(cipher_ctx);

	/* The tag: aad, padding, ciphertext, padding, and the two lengths as 64-bit little-endian integers. */
	mac = EVP_MAC_fetch(NULL, "POLY1305", NULL);
	mac_ctx = EVP_MAC_CTX_new(mac);
	CHECK(mac != NULL && mac_ctx != NULL);
	if (mac == NULL || mac_ctx == NULL) {
		EVP_MAC_CTX_free(mac_ctx);
		EVP_MAC_free(mac);
		return;
	}
	memset(padding, 0, sizeof(padding));
	OK(EVP_MAC_init(mac_ctx, one_time_key, 32U, NULL));
	OK(EVP_MAC_update(mac_ctx, aad, aad_size));
	pad = (16U - aad_size % 16U) % 16U;
	OK(EVP_MAC_update(mac_ctx, padding, pad));
	OK(EVP_MAC_update(mac_ctx, cipher, size));
	pad = (16U - size % 16U) % 16U;
	OK(EVP_MAC_update(mac_ctx, padding, pad));
	write_le64(lengths, (uint64_t)aad_size);
	write_le64(lengths + 8, (uint64_t)size);
	OK(EVP_MAC_update(mac_ctx, lengths, sizeof(lengths)));
	tag_length = 0;
	OK(EVP_MAC_final(mac_ctx, tag, &tag_length, 16U));
	CHECK(tag_length == 16U);
	EVP_MAC_CTX_free(mac_ctx);
	EVP_MAC_free(mac);
}

/* Checks the AEAD example of RFC 8439 2.8.2 sealed with the EVP functions. */
static void
test_aead_rfc(void)
{
	uint8_t key[32];
	uint8_t nonce[12];
	uint8_t aad[VECTOR_BYTES];
	uint8_t plain[VECTOR_BYTES];
	uint8_t expected[VECTOR_BYTES];
	uint8_t expected_tag[16];
	uint8_t cipher[VECTOR_BYTES];
	uint8_t tag[16];
	uint8_t iv[16];
	uint8_t one_time_key[32];
	uint8_t expected_key[32];
	uint8_t zeros[32];
	EVP_CIPHER_CTX *ctx;
	size_t size;
	size_t aad_size;
	int out_length;

	/* The key; the nonce is the fixed part 07 00 00 00 then the IV. */
	CHECK(unhex(rfc_aead_key, key, sizeof(key)) == 32U);
	CHECK(unhex("07000000", nonce, 4U) == 4U);
	CHECK(unhex(rfc_aead_iv, nonce + 4, 8U) == 8U);
	size = unhex(rfc_aead_plain, plain, sizeof(plain));
	aad_size = unhex(rfc_aead_aad, aad, sizeof(aad));
	CHECK(unhex(rfc_aead_cipher, expected, sizeof(expected)) == size);
	CHECK(unhex(rfc_aead_tag, expected_tag, sizeof(expected_tag)) == 16U);

	/* The ciphertext and the tag. */
	aead_seal(key, nonce, aad, aad_size, plain, size, cipher, tag);
	CHECK_BYTES(cipher, expected, size);
	CHECK_BYTES(tag, expected_tag, 16U);

	/* The one-time key of 2.8.2: the first 32 bytes of block 0. */
	CHECK(unhex(rfc_aead_otk, expected_key, sizeof(expected_key)) == 32U);
	write_le32(iv, 0U);
	memcpy(iv + 4, nonce, 12U);
	memset(zeros, 0, sizeof(zeros));
	ctx = new_cipher(key, iv);
	if (ctx != NULL) {
		OK(EVP_CipherUpdate(ctx, one_time_key, &out_length, zeros, 32));
		CHECK_BYTES(one_time_key, expected_key, 32U);
		EVP_CIPHER_CTX_free(ctx);
	}

	/* Appendix A.5: the message of the open example sealed again gives its ciphertext and tag. */
	CHECK(unhex(rfc_open_key, key, sizeof(key)) == 32U);
	CHECK(unhex(rfc_open_nonce, nonce, sizeof(nonce)) == 12U);
	aad_size = unhex(rfc_open_aad, aad, sizeof(aad));
	size = unhex(rfc_open_plain, plain, sizeof(plain));
	CHECK(unhex(rfc_open_cipher, expected, sizeof(expected)) == size);
	CHECK(unhex(rfc_open_tag, expected_tag, sizeof(expected_tag)) == 16U);
	aead_seal(key, nonce, aad, aad_size, plain, size, cipher, tag);
	CHECK_BYTES(cipher, expected, size);
	CHECK_BYTES(tag, expected_tag, 16U);
}

/*
 * Checks the Wycheproof vectors of ChaCha20-Poly1305 sealed with the EVP
 * functions: a valid record gives the ciphertext and tag of the file, an
 * invalid record's tag is not the tag computed over its ciphertext.
 */
static void
test_wycheproof(void)
{
	struct vec_file file;
	uint8_t key[64];
	uint8_t nonce[64];
	uint8_t aad[VECTOR_BYTES];
	uint8_t message[VECTOR_BYTES];
	uint8_t cipher[VECTOR_BYTES];
	uint8_t tag[32];
	uint8_t computed_cipher[VECTOR_BYTES];
	uint8_t computed_tag[16];
	const char *result;
	const char *text;
	size_t key_size;
	size_t nonce_size;
	size_t aad_size;
	size_t size;
	size_t cipher_size;
	size_t tag_size;

	/* Every record. */
	if (vec_open(&file, "wycheproof-chacha20_poly1305_test") != 0) {
		CHECK(0);
		return;
	}
	while (vec_next(&file)) {
		result = vec_get(&file, "result");
		CHECK(result != NULL);
		if (result == NULL)
			continue;

		/* Only 256-bit keys, 96-bit nonces and 128-bit tags are in the interface. */
		text = vec_get(&file, "group.ivSize");
		if (text == NULL || strcmp(text, "96") != 0) {
			wycheproof_skipped++;
			continue;
		}
		text = vec_get(&file, "group.tagSize");
		if (text == NULL || strcmp(text, "128") != 0) {
			wycheproof_skipped++;
			continue;
		}
		text = vec_get(&file, "group.keySize");
		if (text == NULL || strcmp(text, "256") != 0) {
			wycheproof_skipped++;
			continue;
		}

		/* The fields. */
		key_size = vec_hex(&file, "key", key, sizeof(key));
		nonce_size = vec_hex(&file, "iv", nonce, sizeof(nonce));
		aad_size = vec_hex(&file, "aad", aad, sizeof(aad));
		size = vec_hex(&file, "msg", message, sizeof(message));
		cipher_size = vec_hex(&file, "ct", cipher, sizeof(cipher));
		tag_size = vec_hex(&file, "tag", tag, sizeof(tag));
		CHECK(key_size == 32U && nonce_size == 12U && tag_size == 16U);
		CHECK(aad_size != (size_t)-1 && size != (size_t)-1 && cipher_size == size);
		if (key_size != 32U || nonce_size != 12U || tag_size != 16U || aad_size == (size_t)-1 || size == (size_t)-1 || cipher_size != size)
			continue;

		/* Seals the message; a valid record's ciphertext and tag are the file's. */
		aead_seal(key, nonce, aad, aad_size, message, size, computed_cipher, computed_tag);
		if (strcmp(result, "valid") == 0) {
			CHECK_BYTES(computed_cipher, cipher, size);
			CHECK_BYTES(computed_tag, tag, 16U);
		} else {
			/* The tag of an invalid record is wrong for its message (the ciphertext of such a record is the message's own). */
			CHECK(memcmp(computed_tag, tag, 16U) != 0 || memcmp(computed_cipher, cipher, size) != 0);
		}
		wycheproof_checked++;
	}
	vec_close(&file);
}

/*
 * Checks the packet construction of SSH's chacha20-poly1305@openssh.com
 * through the EVP functions against the primitives called directly: a
 * 4-byte length encrypted with a header key at counter 0, the one-time
 * key from block 0 of the main key, the payload encrypted from block 1,
 * and a tag over the encrypted length and payload; the nonce is the
 * 64-bit big-endian packet sequence number.
 */
static void
check_ssh_packet(
	const uint8_t *main_key,
	const uint8_t *header_key,
	uint64_t sequence,
	const uint8_t *payload,
	size_t payload_size)
{
	uint8_t nonce[8];
	uint8_t iv[16];
	uint8_t length_field[4];
	uint8_t expected_length[4];
	uint8_t sealed_length[4];
	uint8_t zeros[32];
	uint8_t expected_key[32];
	uint8_t got_key[32];
	uint8_t expected_payload[MESSAGE_BYTES];
	uint8_t sealed_payload[MESSAGE_BYTES];
	uint8_t opened_payload[MESSAGE_BYTES];
	uint8_t expected_tag[16];
	uint8_t got_tag[16];
	struct cryc_poly1305 reference;
	EVP_CIPHER_CTX *header_ctx;
	EVP_CIPHER_CTX *main_ctx;
	EVP_MAC *mac;
	EVP_MAC_CTX *mac_ctx;
	size_t tag_length;
	int out_length;
	int error;

	/* The nonce and the plain length field. */
	write_be64(nonce, sequence);
	length_field[0] = (uint8_t)((payload_size >> 24) & 0xffU);
	length_field[1] = (uint8_t)((payload_size >> 16) & 0xffU);
	length_field[2] = (uint8_t)((payload_size >> 8) & 0xffU);
	length_field[3] = (uint8_t)(payload_size & 0xffU);
	memset(zeros, 0, sizeof(zeros));

	/* The expected packet, from the block functions and Poly1305 called directly. */
	error = cryc_chacha20_xor64(header_key, 0, nonce, length_field, expected_length, 4U);
	CHECK(error == 0);
	error = cryc_chacha20_xor64(main_key, 0, nonce, zeros, expected_key, 32U);
	CHECK(error == 0);
	error = cryc_chacha20_xor64(main_key, 1, nonce, payload, expected_payload, payload_size);
	CHECK(error == 0);
	cryc_poly1305_init(&reference, expected_key);
	cryc_poly1305_update(&reference, expected_length, 4U);
	cryc_poly1305_update(&reference, expected_payload, payload_size);
	cryc_poly1305_final(&reference, expected_tag);

	/* Through the EVP functions: the length field with the header key. */
	make_iv(iv, 0, nonce);
	header_ctx = new_cipher(header_key, iv);
	main_ctx = new_cipher(main_key, iv);
	mac = EVP_MAC_fetch(NULL, "POLY1305", NULL);
	mac_ctx = EVP_MAC_CTX_new(mac);
	CHECK(header_ctx != NULL && main_ctx != NULL && mac != NULL && mac_ctx != NULL);
	if (header_ctx == NULL || main_ctx == NULL || mac == NULL || mac_ctx == NULL) {
		EVP_CIPHER_CTX_free(header_ctx);
		EVP_CIPHER_CTX_free(main_ctx);
		EVP_MAC_CTX_free(mac_ctx);
		EVP_MAC_free(mac);
		return;
	}
	OK(EVP_CipherUpdate(header_ctx, sealed_length, &out_length, length_field, 4));
	CHECK_BYTES(sealed_length, expected_length, 4U);

	/* The one-time key from block 0 of the main key, then the payload from block 1 on the same context. */
	OK(EVP_CipherUpdate(main_ctx, got_key, &out_length, zeros, 32));
	CHECK_BYTES(got_key, expected_key, 32U);
	make_iv(iv, 1, nonce);
	OK(EVP_CipherInit_ex(main_ctx, NULL, NULL, NULL, iv, -1));
	OK(EVP_CipherUpdate(main_ctx, sealed_payload, &out_length, payload, (int)payload_size));
	CHECK_BYTES(sealed_payload, expected_payload, payload_size);

	/* The tag over the encrypted length and payload. */
	OK(EVP_MAC_init(mac_ctx, got_key, 32U, NULL));
	OK(EVP_MAC_update(mac_ctx, sealed_length, 4U));
	OK(EVP_MAC_update(mac_ctx, sealed_payload, payload_size));
	tag_length = 0;
	OK(EVP_MAC_final(mac_ctx, got_tag, &tag_length, sizeof(got_tag)));
	CHECK(tag_length == 16U);
	CHECK_BYTES(got_tag, expected_tag, 16U);
	EVP_CIPHER_CTX_free(header_ctx);
	EVP_CIPHER_CTX_free(main_ctx);

	/* Decrypting with fresh contexts gives back the length field and the payload. */
	make_iv(iv, 0, nonce);
	header_ctx = new_cipher(header_key, iv);
	make_iv(iv, 1, nonce);
	main_ctx = new_cipher(main_key, iv);
	if (header_ctx != NULL && main_ctx != NULL) {
		OK(EVP_CipherUpdate(header_ctx, sealed_length, &out_length, sealed_length, 4));
		CHECK_BYTES(sealed_length, length_field, 4U);
		OK(EVP_CipherUpdate(main_ctx, opened_payload, &out_length, sealed_payload, (int)payload_size));
		CHECK_BYTES(opened_payload, payload, payload_size);
	}
	EVP_CIPHER_CTX_free(header_ctx);
	EVP_CIPHER_CTX_free(main_ctx);
	EVP_MAC_CTX_free(mac_ctx);
	EVP_MAC_free(mac);
}

/* Runs the packet check for several sequence numbers and payload sizes. */
static void
test_ssh_packets(void)
{
	static const uint64_t sequences[] = {
		CRYC_U64(0x00000000U, 0x00000000U),
		CRYC_U64(0x00000000U, 0x00000001U),
		CRYC_U64(0x00000000U, 0x000000ffU),
		CRYC_U64(0x00000000U, 0xffffffffU),
		CRYC_U64(0x00000001U, 0x00000000U),
		CRYC_U64(0x01234567U, 0x89abcdefU)
	};
	static const size_t sizes[] = { 0U, 1U, 15U, 16U, 17U, 31U, 32U, 63U, 64U, 65U, 100U, 500U, 1000U };
	uint8_t main_key[32];
	uint8_t header_key[32];
	uint8_t payload[MESSAGE_BYTES];
	size_t sequence;
	size_t size;

	/* Every sequence number with every size, fresh random keys and payloads. */
	for (sequence = 0; sequence < sizeof(sequences) / sizeof(sequences[0]); sequence++) {
		for (size = 0; size < sizeof(sizes) / sizeof(sizes[0]); size++) {
			random_fill(main_key, sizeof(main_key));
			random_fill(header_key, sizeof(header_key));
			random_fill(payload, sizeof(payload));
			check_ssh_packet(main_key, header_key, sequences[sequence], payload, sizes[size]);
		}
	}
}

int
main(void)
{
	/* SHA-256. */
	test_digest_examples();
	test_digest_vectors();
	test_digest_api();
	test_digest_key();

	/* ChaCha20. */
	test_cipher_rfc();
	test_cipher_stream();
	test_cipher_exhaustion();
	test_cipher_setup();

	/* Poly1305. */
	test_mac_rfc();
	test_mac_api();

	/* The constructions built on them. */
	test_aead_rfc();
	test_wycheproof();
	test_ssh_packets();

	/* The Wycheproof coverage, then the verdict. */
	CHECK(wycheproof_checked >= 250UL);
	printf("wycheproof: %lu records checked, %lu skipped\n", wycheproof_checked, wycheproof_skipped);
	return check_finish("test-api-evp");
}
