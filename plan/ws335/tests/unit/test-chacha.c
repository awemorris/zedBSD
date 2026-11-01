/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks ChaCha20, Poly1305 and AEAD_CHACHA20_POLY1305 (cryc-chacha.h).
 *
 * Expected values: the examples and test vectors of RFC 8439 (2.3.2,
 * 2.4.2, 2.5.2, 2.6.2, 2.8.2 and Appendix A), the Wycheproof vectors of
 * chacha20_poly1305_test read from the vector file, and two references
 * written from the text of RFC 8439 alone: Poly1305 as the arithmetic
 * on big integers of 2.5 (acc = (acc + block) * r modulo 2^130 - 5, with
 * the clamp of r and the final addition of s), and the AEAD message
 * layout of 2.8 built around it.  The keystream is checked against the
 * block function blockwise, including the counters at the 32-bit and
 * 64-bit limits.
 */

#include "check.h"
#include "vecfile.h"

#include "userland/base/libcrypto-compat/cryc-chacha.h"

/* The sizes of the buffers that hold decoded vectors. */
#define VECTOR_BYTES		1024U

/* The longest message the generated tests use, and the room for it with the padding of the AEAD message. */
#define MESSAGE_BYTES		1100U

/* The length of the numbers of the Poly1305 reference, in bytes (320 bits cover a 131-bit sum times a 128-bit r). */
#define BIG_BYTES		40U

/* A byte that fills a buffer to show later that nothing was written to it. */
#define SENTINEL_BYTE		0x5aU

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
static uint32_t random_state = 0x2c1b3c6dU;

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

/* Reads a 32-bit little-endian word (the test's own, not the library's). */
static uint32_t
read_le32(
	const uint8_t *bytes)
{
	/* Byte 0 is the least significant. */
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/* Writes a 32-bit little-endian word. */
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
 * The reference keystream: blocks consecutive 64-byte blocks starting at
 * counter, each from the block function (2.3, 2.4.1).  The 32-bit form
 * takes the 12-byte nonce as it is; the 64-bit form takes the 8-byte nonce
 * and builds the 12-byte nonce of each block from the counter's high half
 * (words 13 and 14 of the state are the counter's high half and the first
 * nonce word, so the high half is the first four nonce bytes read as a
 * little-endian word).
 */
static void
reference_stream(
	const uint8_t *key,
	uint64_t counter,
	int wide,
	const uint8_t *nonce,
	size_t blocks,
	uint8_t *stream)
{
	uint8_t block_nonce[12];
	uint64_t current;
	size_t index;

	/* Each block with its own counter. */
	for (index = 0; index < blocks; index++) {
		current = counter + (uint64_t)index;
		if (wide) {
			write_le32(block_nonce, (uint32_t)(current >> 32));
			memcpy(block_nonce + 4, nonce, 8);
		} else {
			memcpy(block_nonce, nonce, 12);
		}
		cryc_chacha20_block(key, (uint32_t)(current & 0xffffffffU), block_nonce, stream + 64U * index);
	}
}

/* Checks one block of the block function against the expected hex text. */
static void
check_block(
	const char *key_hex,
	const char *nonce_hex,
	uint32_t counter,
	const char *expected_hex)
{
	uint8_t key[32];
	uint8_t key_copy[32];
	uint8_t nonce[12];
	uint8_t nonce_copy[12];
	uint8_t expected[64];
	uint8_t out[65];

	/* The inputs. */
	CHECK(unhex(key_hex, key, sizeof(key)) == 32U);
	CHECK(unhex(nonce_hex, nonce, sizeof(nonce)) == 12U);
	CHECK(unhex(expected_hex, expected, sizeof(expected)) == 64U);
	memcpy(key_copy, key, sizeof(key));
	memcpy(nonce_copy, nonce, sizeof(nonce));

	/* The block, with a guard byte after it. */
	memset(out, SENTINEL_BYTE, sizeof(out));
	cryc_chacha20_block(key, counter, nonce, out);
	CHECK_BYTES(out, expected, 64U);
	CHECK(out[64] == SENTINEL_BYTE);

	/* The inputs are left alone. */
	CHECK_BYTES(key, key_copy, sizeof(key));
	CHECK_BYTES(nonce, nonce_copy, sizeof(nonce));
}

/* Checks the block function: RFC 8439 2.3.2 and A.1. */
static void
test_block(void)
{
	size_t index;

	/* 2.3.2: the key 00 01 ... 1f, the nonce 00 00 00 09 00 00 00 4a 00 00 00 00 and block count 1. */
	check_block("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", "000000090000004a00000000", 1U, rfc_block_serialized);

	/* A.1: the five block vectors. */
	for (index = 0; index < sizeof(a1_vectors) / sizeof(a1_vectors[0]); index++)
		check_block(a1_vectors[index].key, a1_vectors[index].nonce, a1_vectors[index].counter, a1_vectors[index].keystream);
}

/*
 * Checks cryc_chacha20_xor and cryc_chacha20_xor64 against the expected
 * ciphertext: out of place, in place, and back again; the input is left
 * alone and nothing is written past the output.  When wide is set, the
 * 64-bit form is used with the counter and nonce mapped from the 12-byte
 * form of the RFC: counter = (first four nonce bytes) * 2^32 + counter32,
 * nonce8 = the last eight nonce bytes.
 */
static void
check_cipher(
	int wide,
	const uint8_t *key,
	const uint8_t *nonce,
	uint32_t counter,
	const uint8_t *plain,
	size_t size,
	const uint8_t *cipher)
{
	uint8_t input[VECTOR_BYTES];
	uint8_t output[VECTOR_BYTES + 8U];
	uint64_t wide_counter;
	int error;

	/* The inputs are kept for the comparison afterwards. */
	memcpy(input, plain, size);
	wide_counter = CRYC_U64(read_le32(nonce), counter);

	/* Out of place. */
	memset(output, SENTINEL_BYTE, sizeof(output));
	if (wide)
		error = cryc_chacha20_xor64(key, wide_counter, nonce + 4, input, output, size);
	else
		error = cryc_chacha20_xor(key, counter, nonce, input, output, size);
	CHECK(error == 0);
	CHECK_BYTES(output, cipher, size);
	CHECK(all_equal(output + size, 8U, SENTINEL_BYTE));
	CHECK_BYTES(input, plain, size);

	/* In place. */
	memcpy(output, plain, size);
	if (wide)
		error = cryc_chacha20_xor64(key, wide_counter, nonce + 4, output, output, size);
	else
		error = cryc_chacha20_xor(key, counter, nonce, output, output, size);
	CHECK(error == 0);
	CHECK_BYTES(output, cipher, size);

	/* Back again: the same operation decrypts. */
	if (wide)
		error = cryc_chacha20_xor64(key, wide_counter, nonce + 4, output, output, size);
	else
		error = cryc_chacha20_xor(key, counter, nonce, output, output, size);
	CHECK(error == 0);
	CHECK_BYTES(output, plain, size);
}

/* Checks a vector of the cipher with both forms (the 64-bit form through the mapping of the nonce). */
static void
check_xor_vector(
	const char *key_hex,
	const char *nonce_hex,
	uint32_t counter,
	const char *plain_hex,
	const char *cipher_hex)
{
	uint8_t key[32];
	uint8_t nonce[12];
	uint8_t plain[VECTOR_BYTES];
	uint8_t cipher[VECTOR_BYTES];
	size_t size;

	/* The inputs. */
	CHECK(unhex(key_hex, key, sizeof(key)) == 32U);
	CHECK(unhex(nonce_hex, nonce, sizeof(nonce)) == 12U);
	size = unhex(plain_hex, plain, sizeof(plain));
	CHECK(unhex(cipher_hex, cipher, sizeof(cipher)) == size);

	/* The 32-bit form, then the 64-bit form with the same keystream. */
	check_cipher(0, key, nonce, counter, plain, size, cipher);
	check_cipher(1, key, nonce, counter, plain, size, cipher);
}

/*
 * Checks one stream case: size bytes of random input at counter, against the
 * blockwise reference, or the refusal when the counter would pass its limit.
 * wide selects the 64-bit form (nonce of 8 bytes) or the 32-bit form (12).
 */
static void
check_stream_case(
	int wide,
	uint64_t counter,
	size_t size,
	const uint8_t *key,
	const uint8_t *nonce)
{
	uint8_t input[MESSAGE_BYTES];
	uint8_t expected[MESSAGE_BYTES + 64U];
	uint8_t stream[MESSAGE_BYTES + 64U];
	uint8_t output[MESSAGE_BYTES + 8U];
	uint64_t blocks;
	int overflow;
	int error;
	size_t index;

	/* The input, and the number of blocks it needs. */
	random_fill(input, size);
	blocks = ((uint64_t)size + 63U) / 64U;

	/* The last block has counter + blocks - 1, which must not pass the limit. */
	overflow = 0;
	if (blocks > 0U) {
		if (wide)
			overflow = (blocks - 1U) > (~(uint64_t)0 - counter);
		else
			overflow = counter + blocks - 1U > (uint64_t)0xffffffffU;
	}

	/* Out of place. */
	memset(output, SENTINEL_BYTE, sizeof(output));
	if (wide)
		error = cryc_chacha20_xor64(key, counter, nonce, input, output, size);
	else
		error = cryc_chacha20_xor(key, (uint32_t)counter, nonce, input, output, size);
	if (overflow) {
		/* Refused, and nothing is written. */
		CHECK(error == EOVERFLOW);
		CHECK(all_equal(output, sizeof(output), SENTINEL_BYTE));
	} else {
		/* The input exclusive-or the keystream blocks. */
		reference_stream(key, counter, wide, nonce, (size_t)blocks, stream);
		for (index = 0; index < size; index++)
			expected[index] = (uint8_t)(input[index] ^ stream[index]);
		CHECK(error == 0);
		CHECK_BYTES(output, expected, size);
		CHECK(all_equal(output + size, 8U, SENTINEL_BYTE));
	}

	/* In place gives the same, and a refusal leaves the buffer as it was. */
	memcpy(output, input, size);
	if (wide)
		error = cryc_chacha20_xor64(key, counter, nonce, output, output, size);
	else
		error = cryc_chacha20_xor(key, (uint32_t)counter, nonce, output, output, size);
	if (overflow) {
		CHECK(error == EOVERFLOW);
		CHECK_BYTES(output, input, size);
	} else {
		CHECK(error == 0);
		CHECK_BYTES(output, expected, size);
	}
}

/* Checks the cipher: RFC 8439 2.4.2 and A.2, then the blockwise reference over sizes and counters. */
static void
test_cipher(void)
{
	static const size_t sizes[] = {
		0U, 1U, 2U, 15U, 16U, 17U, 31U, 32U, 33U, 63U, 64U, 65U, 66U, 127U, 128U, 129U,
		191U, 192U, 193U, 255U, 256U, 257U, 300U, 1000U
	};
	static const uint64_t counters32[] = {
		0U, 1U, 0x7fffffffU, 0xfffffff0U, 0xfffffffdU, 0xfffffffeU, 0xffffffffU
	};
	uint8_t key[32];
	uint8_t nonce[12];
	uint8_t scratch[8];
	size_t index;
	size_t which;
	uint64_t wide_counters[12];
	int error;

	/* 2.4.2: the key 00 01 ... 1f, the nonce 00 .. 4a 00 00 00 00 and initial counter 1. */
	check_xor_vector("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", "000000000000004a00000000", 1U, rfc_sunscreen_plain, rfc_sunscreen_cipher);

	/* A.2: the three cipher vectors. */
	for (index = 0; index < sizeof(a2_vectors) / sizeof(a2_vectors[0]); index++)
		check_xor_vector(a2_vectors[index].key, a2_vectors[index].nonce, a2_vectors[index].counter, a2_vectors[index].plain, a2_vectors[index].cipher);

	/* A random key and nonce for the blockwise comparisons. */
	random_fill(key, sizeof(key));
	random_fill(nonce, sizeof(nonce));

	/* The 32-bit form over every size and counter, including the cases that pass 2^32 - 1. */
	for (which = 0; which < sizeof(counters32) / sizeof(counters32[0]); which++)
		for (index = 0; index < sizeof(sizes) / sizeof(sizes[0]); index++)
			check_stream_case(0, counters32[which], sizes[index], key, nonce);

	/* The 64-bit form, with counters on both sides of the 32-bit boundary and at the end of the 64-bit range. */
	wide_counters[0] = 0U;
	wide_counters[1] = 1U;
	wide_counters[2] = 0xfffffffeU;
	wide_counters[3] = 0xffffffffU;
	wide_counters[4] = CRYC_U64(1U, 0U);
	wide_counters[5] = CRYC_U64(1U, 0xffffffffU);
	wide_counters[6] = CRYC_U64(0x12345678U, 0xfffffffeU);
	wide_counters[7] = CRYC_U64(0x7fffffffU, 0xffffffffU);
	wide_counters[8] = CRYC_U64(0xffffffffU, 0xfffffffdU);
	wide_counters[9] = CRYC_U64(0xffffffffU, 0xfffffffeU);
	wide_counters[10] = CRYC_U64(0xffffffffU, 0xffffffffU);
	wide_counters[11] = CRYC_U64(0x80000000U, 0U);
	for (which = 0; which < sizeof(wide_counters) / sizeof(wide_counters[0]); which++)
		for (index = 0; index < sizeof(sizes) / sizeof(sizes[0]); index++)
			check_stream_case(1, wide_counters[which], sizes[index], key, nonce);

	/* A zero size succeeds even at the last counter, and the buffers are not touched. */
	memset(scratch, SENTINEL_BYTE, sizeof(scratch));
	error = cryc_chacha20_xor(key, 0xffffffffU, nonce, scratch, scratch, 0U);
	CHECK(error == 0);
	error = cryc_chacha20_xor64(key, CRYC_U64(0xffffffffU, 0xffffffffU), nonce, scratch, scratch, 0U);
	CHECK(error == 0);
	CHECK(all_equal(scratch, sizeof(scratch), SENTINEL_BYTE));

	/* The most a 32-bit counter allows is (2^32 - counter) * 64 bytes; one more is refused (checked before any byte is touched). */
	if (sizeof(size_t) > 4U) {
		error = cryc_chacha20_xor(key, 0U, nonce, scratch, scratch, (size_t)CRYC_U64(0x40U, 1U));
		CHECK(error == EOVERFLOW);
		error = cryc_chacha20_xor(key, 1U, nonce, scratch, scratch, (size_t)CRYC_U64(0x3fU, 0xffffffc1U));
		CHECK(error == EOVERFLOW);
		error = cryc_chacha20_xor(key, 0x80000000U, nonce, scratch, scratch, (size_t)CRYC_U64(0x20U, 1U));
		CHECK(error == EOVERFLOW);
		CHECK(all_equal(scratch, sizeof(scratch), SENTINEL_BYTE));
	}
}

/*
 * Checks the 64-bit form against the 2.3.2 block through the mapping of
 * words: the 2.3.2 state has word 12 = 1 and words 13 to 15 = 00 00 00 09,
 * 00 00 00 4a, 00 00 00 00, so the counter is 0x0900000000000001 and the
 * nonce is the eight bytes 00 00 00 4a 00 00 00 00 (the same keystream block
 * with the state words read the original ChaCha way).
 */
static void
test_wide_block(void)
{
	uint8_t key[32];
	uint8_t nonce8[8];
	uint8_t zero[64];
	uint8_t out[64];
	uint8_t expected[64];
	size_t index;
	int error;

	/* The key and the nonce of 2.3.2. */
	for (index = 0; index < sizeof(key); index++)
		key[index] = (uint8_t)index;
	CHECK(unhex("0000004a00000000", nonce8, sizeof(nonce8)) == 8U);
	CHECK(unhex(rfc_block_serialized, expected, sizeof(expected)) == 64U);

	/* The keystream is the exclusive-or of zeros. */
	memset(zero, 0, sizeof(zero));
	error = cryc_chacha20_xor64(key, CRYC_U64(0x09000000U, 1U), nonce8, zero, out, sizeof(zero));
	CHECK(error == 0);
	CHECK_BYTES(out, expected, 64U);
}
/*
 * The Poly1305 reference, written from RFC 8439 2.5: numbers are
 * little-endian byte arrays of BIG_BYTES bytes, and the accumulator
 * follows "Acc = ((Acc + block) * r) % P" with P = 2^130 - 5, the 0x01 byte
 * appended to each block, r clamped, and "Acc + s" taken modulo 2^128 for
 * the tag.
 */
typedef uint8_t big_number[BIG_BYTES];

/* Sets a number to zero. */
static void
big_zero(
	big_number number)
{
	/* Every byte. */
	memset(number, 0, BIG_BYTES);
}

/* Adds addend to number (the sum must fit). */
static void
big_add(
	big_number number,
	const big_number addend)
{
	unsigned carry;
	unsigned sum;
	size_t index;

	/* Byte by byte with the carry. */
	carry = 0;
	for (index = 0; index < BIG_BYTES; index++) {
		sum = (unsigned)number[index] + (unsigned)addend[index] + carry;
		number[index] = (uint8_t)(sum & 0xffU);
		carry = sum >> 8;
	}
}

/* Multiplies left by right into product (the product must fit). */
static void
big_multiply(
	big_number product,
	const big_number left,
	const big_number right)
{
	uint32_t column[BIG_BYTES];
	uint32_t carry;
	size_t row;
	size_t index;

	/* The columns of the long multiplication; each stays far below 2^32. */
	memset(column, 0, sizeof(column));
	for (row = 0; row < BIG_BYTES; row++)
		for (index = 0; row + index < BIG_BYTES; index++)
			column[row + index] += (uint32_t)left[row] * (uint32_t)right[index];

	/* Carries between the columns. */
	carry = 0;
	for (index = 0; index < BIG_BYTES; index++) {
		column[index] += carry;
		product[index] = (uint8_t)(column[index] & 0xffU);
		carry = column[index] >> 8;
	}
}

/* Returns whether left is at least right. */
static int
big_at_least(
	const big_number left,
	const big_number right)
{
	size_t index;

	/* From the most significant byte down. */
	for (index = BIG_BYTES; index > 0U; index--) {
		if (left[index - 1U] > right[index - 1U])
			return 1;
		if (left[index - 1U] < right[index - 1U])
			return 0;
	}
	return 1;
}

/* Subtracts amount from number (number must be at least amount). */
static void
big_subtract(
	big_number number,
	const big_number amount)
{
	int borrow;
	int difference;
	size_t index;

	/* Byte by byte with the borrow. */
	borrow = 0;
	for (index = 0; index < BIG_BYTES; index++) {
		difference = (int)number[index] - (int)amount[index] - borrow;
		borrow = 0;
		if (difference < 0) {
			difference += 256;
			borrow = 1;
		}
		number[index] = (uint8_t)difference;
	}
}

/*
 * Reduces number modulo P = 2^130 - 5: a number is high * 2^130 + low,
 * which is low + 5 * high modulo P; this is repeated until the part above
 * bit 130 is zero, and P is subtracted once if what is left is at least P.
 */
static void
big_reduce(
	big_number number)
{
	big_number high;
	big_number five_high;
	big_number prime;
	unsigned carry;
	unsigned value;
	int again;
	size_t index;

	/* P in bytes: 2^130 - 5 is fb, then 15 bytes of ff, then 03. */
	big_zero(prime);
	prime[0] = 0xfbU;
	for (index = 1; index < 16U; index++)
		prime[index] = 0xffU;
	prime[16] = 0x03U;

	/* Folds the part above bit 130 back in until nothing is above it. */
	do {
		/* high = number >> 130: bit 130 is bit 2 of byte 16. */
		big_zero(high);
		again = 0;
		for (index = 0; index + 16U < BIG_BYTES; index++) {
			value = (unsigned)number[16U + index] >> 2;
			if (index + 17U < BIG_BYTES)
				value |= ((unsigned)number[17U + index] << 6) & 0xffU;
			high[index] = (uint8_t)value;
			if (value != 0U)
				again = 1;
		}
		if (!again)
			break;

		/* number = number mod 2^130, then plus 5 * high. */
		number[16] &= 0x03U;
		for (index = 17U; index < BIG_BYTES; index++)
			number[index] = 0;
		carry = 0;
		for (index = 0; index < BIG_BYTES; index++) {
			value = (unsigned)high[index] * 5U + carry;
			five_high[index] = (uint8_t)(value & 0xffU);
			carry = value >> 8;
		}
		big_add(number, five_high);
	} while (again);

	/* Less than 2^130 but possibly at least P. */
	if (big_at_least(number, prime))
		big_subtract(number, prime);
}

/* Computes the Poly1305 tag of size bytes at message by the reference. */
static void
reference_poly1305(
	const uint8_t *key,
	const uint8_t *message,
	size_t size,
	uint8_t *tag)
{
	big_number accumulator;
	big_number block;
	big_number multiplier;
	big_number product;
	big_number addend;
	uint8_t clamped[16];
	size_t offset;
	size_t take;

	/* r: the first 16 bytes with the clamp of 2.5. */
	memcpy(clamped, key, 16U);
	clamped[3] &= 15U;
	clamped[7] &= 15U;
	clamped[11] &= 15U;
	clamped[15] &= 15U;
	clamped[4] &= 252U;
	clamped[8] &= 252U;
	clamped[12] &= 252U;
	big_zero(multiplier);
	memcpy(multiplier, clamped, 16U);

	/* Each block of up to 16 bytes with 0x01 appended. */
	big_zero(accumulator);
	for (offset = 0; offset < size; offset += 16U) {
		take = size - offset;
		if (take > 16U)
			take = 16U;
		big_zero(block);
		memcpy(block, message + offset, take);
		block[take] = 1;
		big_add(accumulator, block);
		big_multiply(product, accumulator, multiplier);
		big_reduce(product);
		memcpy(accumulator, product, BIG_BYTES);
	}

	/* The tag is (Acc + s) modulo 2^128: s is the second half of the key. */
	big_zero(addend);
	memcpy(addend, key + 16, 16U);
	big_add(accumulator, addend);
	memcpy(tag, accumulator, 16U);
}

/* Computes the tag with the library in one call. */
static void
poly1305_oneshot(
	const uint8_t *key,
	const uint8_t *message,
	size_t size,
	uint8_t *tag)
{
	struct cryc_poly1305 mac;

	/* The three calls; a NULL message is allowed for size 0. */
	cryc_poly1305_init(&mac, key);
	cryc_poly1305_update(&mac, size == 0U ? NULL : message, size);
	cryc_poly1305_final(&mac, tag);
}

/* Computes the tag with the library in pieces of random sizes from 0 to limit bytes. */
static void
poly1305_pieces(
	const uint8_t *key,
	const uint8_t *message,
	size_t size,
	size_t limit,
	uint8_t *tag)
{
	struct cryc_poly1305 mac;
	size_t offset;
	size_t take;

	/* A piece of size 0 is passed with a NULL pointer. */
	cryc_poly1305_init(&mac, key);
	offset = 0;
	while (offset < size) {
		take = random_below(limit + 1U);
		if (take > size - offset)
			take = size - offset;
		cryc_poly1305_update(&mac, take == 0U ? NULL : message + offset, take);
		offset += take;
	}
	cryc_poly1305_final(&mac, tag);
}

/* Checks one tag vector: the library in one call and in pieces, and the reference, against the expected tag. */
static void
check_mac_vector(
	const char *key_hex,
	const char *message_hex,
	const char *tag_hex)
{
	uint8_t key[32];
	uint8_t message[VECTOR_BYTES];
	uint8_t expected[16];
	uint8_t tag[16];
	struct cryc_poly1305 mac;
	size_t size;
	size_t split;
	size_t second;

	/* The inputs. */
	CHECK(unhex(key_hex, key, sizeof(key)) == 32U);
	size = unhex(message_hex, message, sizeof(message));
	CHECK(unhex(tag_hex, expected, sizeof(expected)) == 16U);

	/* One call. */
	poly1305_oneshot(key, message, size, tag);
	CHECK_BYTES(tag, expected, 16U);

	/* The reference arithmetic gives the same tag. */
	reference_poly1305(key, message, size, tag);
	CHECK_BYTES(tag, expected, 16U);

	/* Two pieces at every split point (for the short messages; every 7th for the longer). */
	for (split = 0; split <= size; split += size > 40U ? 7U : 1U) {
		cryc_poly1305_init(&mac, key);
		cryc_poly1305_update(&mac, split == 0U ? NULL : message, split);
		cryc_poly1305_update(&mac, split == size ? NULL : message + split, size - split);
		cryc_poly1305_final(&mac, tag);
		CHECK_BYTES(tag, expected, 16U);
	}

	/* Three pieces at every pair of split points for the short messages. */
	if (size <= 40U) {
		for (split = 0; split <= size; split++) {
			for (second = split; second <= size; second++) {
				cryc_poly1305_init(&mac, key);
				cryc_poly1305_update(&mac, split == 0U ? NULL : message, split);
				cryc_poly1305_update(&mac, second == split ? NULL : message + split, second - split);
				cryc_poly1305_update(&mac, second == size ? NULL : message + second, size - second);
				cryc_poly1305_final(&mac, tag);
				CHECK_BYTES(tag, expected, 16U);
			}
		}
	}

	/* One byte at a time. */
	cryc_poly1305_init(&mac, key);
	for (split = 0; split < size; split++)
		cryc_poly1305_update(&mac, message + split, 1U);
	cryc_poly1305_final(&mac, tag);
	CHECK_BYTES(tag, expected, 16U);
}

/* Checks Poly1305: RFC 8439 2.5.2, A.3, the wipe of the state, and random keys and messages against the reference. */
static void
test_poly1305(void)
{
	static const uint8_t all_ff_key[32] = {
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
	};
	uint8_t key[32];
	uint8_t message[MESSAGE_BYTES];
	uint8_t tag[16];
	uint8_t expected[16];
	uint8_t other[16];
	struct cryc_poly1305 mac;
	size_t index;
	size_t size;
	size_t round;

	/* 2.5.2. */
	check_mac_vector(rfc_poly_key, rfc_poly_message, rfc_poly_tag);

	/* A.3: the eleven vectors. */
	for (index = 0; index < sizeof(a3_vectors) / sizeof(a3_vectors[0]); index++)
		check_mac_vector(a3_vectors[index].key, a3_vectors[index].message, a3_vectors[index].tag);

	/* The state is wiped by the final call. */
	CHECK(unhex(rfc_poly_key, key, sizeof(key)) == 32U);
	cryc_poly1305_init(&mac, key);
	cryc_poly1305_update(&mac, key, 21U);
	cryc_poly1305_final(&mac, tag);
	CHECK(all_equal((const uint8_t *)&mac, sizeof(mac), 0U));

	/* The key is left alone by init. */
	cryc_poly1305_init(&mac, key);
	CHECK(unhex(rfc_poly_key, message, sizeof(message)) == 32U);
	CHECK_BYTES(key, message, 32U);

	/* Messages of all 0xff with the largest clamped r, around every multiple of 16 bytes, against the reference. */
	memset(message, 0xff, sizeof(message));
	for (size = 0; size <= 130U; size++) {
		poly1305_oneshot(all_ff_key, message, size, tag);
		reference_poly1305(all_ff_key, message, size, expected);
		CHECK_BYTES(tag, expected, 16U);
	}

	/* Random keys and messages: the library in one call and in pieces against the reference. */
	for (round = 0; round < 1500U; round++) {
		random_fill(key, sizeof(key));
		size = random_below(300U);
		random_fill(message, size);
		if (round % 5U == 0U)
			memset(message, 0xff, size);
		if (round % 7U == 0U)
			memset(key, 0xff, 16U);
		reference_poly1305(key, message, size, expected);
		poly1305_oneshot(key, message, size, tag);
		CHECK_BYTES(tag, expected, 16U);
		poly1305_pieces(key, message, size, 40U, other);
		CHECK_BYTES(other, expected, 16U);
	}

	/* A long message in pieces larger than a block. */
	random_fill(key, sizeof(key));
	random_fill(message, sizeof(message));
	reference_poly1305(key, message, sizeof(message), expected);
	poly1305_pieces(key, message, sizeof(message), 200U, tag);
	CHECK_BYTES(tag, expected, 16U);
}

/* Checks the Poly1305 key generation: RFC 8439 2.6.2 and A.4 (the first 32 bytes of block 0). */
static void
test_keygen(void)
{
	uint8_t key[32];
	uint8_t nonce[12];
	uint8_t expected[32];
	uint8_t block[64];
	size_t index;

	/* 2.6.2: nonce 00 00 00 00 00 01 02 03 04 05 06 07. */
	CHECK(unhex(rfc_keygen_key, key, sizeof(key)) == 32U);
	CHECK(unhex("000000000001020304050607", nonce, sizeof(nonce)) == 12U);
	CHECK(unhex(rfc_keygen_output, expected, sizeof(expected)) == 32U);
	cryc_chacha20_block(key, 0U, nonce, block);
	CHECK_BYTES(block, expected, 32U);

	/* A.4: three vectors. */
	for (index = 0; index < sizeof(a4_vectors) / sizeof(a4_vectors[0]); index++) {
		CHECK(unhex(a4_vectors[index].key, key, sizeof(key)) == 32U);
		CHECK(unhex(a4_vectors[index].nonce, nonce, sizeof(nonce)) == 12U);
		CHECK(unhex(a4_vectors[index].onetime, expected, sizeof(expected)) == 32U);
		cryc_chacha20_block(key, 0U, nonce, block);
		CHECK_BYTES(block, expected, 32U);
	}
}
/*
 * Builds the data the AEAD authenticates (2.8): the additional data, zeros
 * to a multiple of 16 bytes, the ciphertext, zeros to a multiple of 16
 * bytes, then the two lengths as 64-bit little-endian numbers.  Returns the
 * size.
 */
static size_t
build_mac_data(
	const uint8_t *aad,
	size_t aad_size,
	const uint8_t *cipher,
	size_t size,
	uint8_t *out)
{
	size_t used;
	size_t index;

	/* The additional data and its padding. */
	used = 0;
	if (aad_size > 0U)
		memcpy(out, aad, aad_size);
	used += aad_size;
	while (used % 16U != 0U)
		out[used++] = 0;

	/* The ciphertext and its padding. */
	if (size > 0U)
		memcpy(out + used, cipher, size);
	used += size;
	while (used % 16U != 0U)
		out[used++] = 0;

	/* The lengths: the low four bytes, then zeros (the sizes here are far below 2^32). */
	write_le32(out + used, (uint32_t)aad_size);
	for (index = 4; index < 8U; index++)
		out[used + index] = 0;
	used += 8U;
	write_le32(out + used, (uint32_t)size);
	for (index = 4; index < 8U; index++)
		out[used + index] = 0;
	used += 8U;
	return used;
}

/*
 * Computes the AEAD result by the reference: the one-time key is the first
 * 32 bytes of block 0, the ciphertext is the plaintext exclusive-or the
 * keystream from block 1, and the tag is the reference Poly1305 over the
 * data of 2.8.
 */
static void
reference_seal(
	const uint8_t *key,
	const uint8_t *nonce,
	const uint8_t *aad,
	size_t aad_size,
	const uint8_t *plain,
	size_t size,
	uint8_t *cipher,
	uint8_t *tag)
{
	uint8_t block[64];
	uint8_t stream[MESSAGE_BYTES + 64U];
	uint8_t data[2U * MESSAGE_BYTES + 64U];
	size_t used;
	size_t index;

	/* The one-time key. */
	cryc_chacha20_block(key, 0U, nonce, block);

	/* The ciphertext, from block 1. */
	reference_stream(key, 1U, 0, nonce, (size + 63U) / 64U, stream);
	for (index = 0; index < size; index++)
		cipher[index] = (uint8_t)(plain[index] ^ stream[index]);

	/* The tag. */
	used = build_mac_data(aad, aad_size, cipher, size, data);
	reference_poly1305(block, data, used, tag);
}

/*
 * Checks seal and open of one message against the expected ciphertext and
 * tag: out of place and in place, and the opening back to the plaintext,
 * with guard bytes after the outputs.  The pointers of an empty plaintext
 * or empty additional data are NULL.
 */
static void
check_aead(
	const uint8_t *key,
	const uint8_t *nonce,
	const uint8_t *aad,
	size_t aad_size,
	const uint8_t *plain,
	size_t size,
	const uint8_t *cipher,
	const uint8_t *tag)
{
	uint8_t output[MESSAGE_BYTES + 8U];
	uint8_t got_tag[16 + 4];
	int error;

	/* Seal out of place. */
	memset(output, SENTINEL_BYTE, sizeof(output));
	memset(got_tag, SENTINEL_BYTE, sizeof(got_tag));
	error = cryc_chacha20_poly1305_seal(key, nonce, aad_size == 0U ? NULL : aad, aad_size, size == 0U ? NULL : plain, size, output, got_tag);
	CHECK(error == 0);
	CHECK_BYTES(output, cipher, size);
	CHECK_BYTES(got_tag, tag, 16U);
	CHECK(all_equal(output + size, 8U, SENTINEL_BYTE));
	CHECK(all_equal(got_tag + 16, 4U, SENTINEL_BYTE));

	/* Seal in place. */
	memset(output, SENTINEL_BYTE, sizeof(output));
	if (size > 0U)
		memcpy(output, plain, size);
	error = cryc_chacha20_poly1305_seal(key, nonce, aad_size == 0U ? NULL : aad, aad_size, output, size, output, got_tag);
	CHECK(error == 0);
	CHECK_BYTES(output, cipher, size);
	CHECK_BYTES(got_tag, tag, 16U);
	CHECK(all_equal(output + size, 8U, SENTINEL_BYTE));

	/* Open out of place: the plaintext back, the ciphertext untouched. */
	memset(output, SENTINEL_BYTE, sizeof(output));
	error = cryc_chacha20_poly1305_open(key, nonce, aad_size == 0U ? NULL : aad, aad_size, size == 0U ? NULL : cipher, size, output, tag);
	CHECK(error == 0);
	CHECK_BYTES(output, plain, size);
	CHECK(all_equal(output + size, 8U, SENTINEL_BYTE));

	/* Open in place. */
	memset(output, SENTINEL_BYTE, sizeof(output));
	if (size > 0U)
		memcpy(output, cipher, size);
	error = cryc_chacha20_poly1305_open(key, nonce, aad_size == 0U ? NULL : aad, aad_size, output, size, output, tag);
	CHECK(error == 0);
	CHECK_BYTES(output, plain, size);
	CHECK(all_equal(output + size, 8U, SENTINEL_BYTE));
}

/*
 * Checks that opening fails with EBADMSG, writes size zero bytes to out (and
 * nothing after them), and leaves the ciphertext alone when it is separate;
 * also in place, where the ciphertext is replaced by zeros.
 */
static void
check_open_fails(
	const uint8_t *key,
	const uint8_t *nonce,
	const uint8_t *aad,
	size_t aad_size,
	const uint8_t *cipher,
	size_t size,
	const uint8_t *tag)
{
	uint8_t input[MESSAGE_BYTES];
	uint8_t output[MESSAGE_BYTES + 8U];
	int error;

	/* Out of place. */
	if (size > 0U)
		memcpy(input, cipher, size);
	memset(output, SENTINEL_BYTE, sizeof(output));
	error = cryc_chacha20_poly1305_open(key, nonce, aad_size == 0U ? NULL : aad, aad_size, size == 0U ? NULL : input, size, output, tag);
	CHECK(error == EBADMSG);
	CHECK(all_equal(output, size, 0U));
	CHECK(all_equal(output + size, 8U, SENTINEL_BYTE));
	CHECK_BYTES(input, cipher, size);

	/* In place. */
	memset(output, SENTINEL_BYTE, sizeof(output));
	if (size > 0U)
		memcpy(output, cipher, size);
	error = cryc_chacha20_poly1305_open(key, nonce, aad_size == 0U ? NULL : aad, aad_size, output, size, output, tag);
	CHECK(error == EBADMSG);
	CHECK(all_equal(output, size, 0U));
	CHECK(all_equal(output + size, 8U, SENTINEL_BYTE));
}

/* Flips bit number bit of the bytes at buffer. */
static void
flip_bit(
	uint8_t *buffer,
	size_t bit)
{
	/* Bit 0 is the least significant bit of byte 0. */
	buffer[bit / 8U] = (uint8_t)(buffer[bit / 8U] ^ (1U << (bit % 8U)));
}

/* Checks the AEAD: RFC 8439 2.8.2 and A.5, every single-bit change, and the refusals. */
static void
test_aead_rfc(void)
{
	uint8_t key[32];
	uint8_t nonce[12];
	uint8_t aad[32];
	uint8_t plain[VECTOR_BYTES];
	uint8_t cipher[VECTOR_BYTES];
	uint8_t tag[16];
	uint8_t block[64];
	uint8_t expected[32];
	uint8_t changed_key[32];
	uint8_t changed_nonce[12];
	uint8_t changed_aad[32];
	uint8_t changed_cipher[VECTOR_BYTES];
	uint8_t changed_tag[16];
	size_t aad_size;
	size_t size;
	size_t bit;

	/* 2.8.2: the nonce is the fixed part 07 00 00 00 then the IV. */
	CHECK(unhex(rfc_aead_key, key, sizeof(key)) == 32U);
	CHECK(unhex("07000000", nonce, 4U) == 4U);
	CHECK(unhex(rfc_aead_iv, nonce + 4, 8U) == 8U);
	aad_size = unhex(rfc_aead_aad, aad, sizeof(aad));
	size = unhex(rfc_aead_plain, plain, sizeof(plain));
	CHECK(unhex(rfc_aead_cipher, cipher, sizeof(cipher)) == size);
	CHECK(unhex(rfc_aead_tag, tag, sizeof(tag)) == 16U);

	/* The one-time key is block 0. */
	CHECK(unhex(rfc_aead_otk, expected, sizeof(expected)) == 32U);
	cryc_chacha20_block(key, 0U, nonce, block);
	CHECK_BYTES(block, expected, 32U);

	/* Seal and open. */
	check_aead(key, nonce, aad, aad_size, plain, size, cipher, tag);

	/* Every single-bit change of the key, nonce, additional data, ciphertext or tag is refused. */
	for (bit = 0; bit < 8U * sizeof(key); bit++) {
		memcpy(changed_key, key, sizeof(key));
		flip_bit(changed_key, bit);
		check_open_fails(changed_key, nonce, aad, aad_size, cipher, size, tag);
	}
	for (bit = 0; bit < 8U * sizeof(nonce); bit++) {
		memcpy(changed_nonce, nonce, sizeof(nonce));
		flip_bit(changed_nonce, bit);
		check_open_fails(key, changed_nonce, aad, aad_size, cipher, size, tag);
	}
	for (bit = 0; bit < 8U * aad_size; bit++) {
		memcpy(changed_aad, aad, aad_size);
		flip_bit(changed_aad, bit);
		check_open_fails(key, nonce, changed_aad, aad_size, cipher, size, tag);
	}
	for (bit = 0; bit < 8U * size; bit++) {
		memcpy(changed_cipher, cipher, size);
		flip_bit(changed_cipher, bit);
		check_open_fails(key, nonce, aad, aad_size, changed_cipher, size, tag);
	}
	for (bit = 0; bit < 8U * sizeof(tag); bit++) {
		memcpy(changed_tag, tag, sizeof(tag));
		flip_bit(changed_tag, bit);
		check_open_fails(key, nonce, aad, aad_size, cipher, size, changed_tag);
	}

	/* A shorter ciphertext (one byte cut, a byte of the additional data cut) is refused as well. */
	check_open_fails(key, nonce, aad, aad_size, cipher, size - 1U, tag);
	check_open_fails(key, nonce, aad, aad_size - 1U, cipher, size, tag);
	check_open_fails(key, nonce, aad, 0U, cipher, size, tag);
	check_open_fails(key, nonce, aad, aad_size, cipher, 0U, tag);
}

/* Checks A.5: the received tag is accepted and the plaintext comes out; and sealing gives the ciphertext back. */
static void
test_aead_appendix(void)
{
	uint8_t key[32];
	uint8_t nonce[12];
	uint8_t aad[32];
	uint8_t cipher[VECTOR_BYTES];
	uint8_t plain[VECTOR_BYTES];
	uint8_t tag[16];
	size_t aad_size;
	size_t size;

	/* The inputs. */
	CHECK(unhex(rfc_open_key, key, sizeof(key)) == 32U);
	CHECK(unhex(rfc_open_nonce, nonce, sizeof(nonce)) == 12U);
	aad_size = unhex(rfc_open_aad, aad, sizeof(aad));
	size = unhex(rfc_open_cipher, cipher, sizeof(cipher));
	CHECK(unhex(rfc_open_plain, plain, sizeof(plain)) == size);
	CHECK(unhex(rfc_open_tag, tag, sizeof(tag)) == 16U);

	/* Seal and open, and a changed tag is refused. */
	check_aead(key, nonce, aad, aad_size, plain, size, cipher, tag);
	flip_bit(tag, 77U);
	check_open_fails(key, nonce, aad, aad_size, cipher, size, tag);
}

/* Checks the Wycheproof vectors (96-bit nonces, 128-bit tags): valid ones seal and open, invalid ones are refused. */
static void
test_wycheproof(void)
{
	struct vec_file file;
	uint8_t key[32];
	uint8_t nonce[12];
	uint8_t aad[VECTOR_BYTES];
	uint8_t message[VECTOR_BYTES];
	uint8_t cipher[VECTOR_BYTES];
	uint8_t tag[16];
	const char *result;
	const char *text;
	size_t key_size;
	size_t nonce_size;
	size_t aad_size;
	size_t size;
	size_t cipher_size;
	size_t tag_size;
	int accepted;

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
		CHECK(text != NULL);
		if (text == NULL || strcmp(text, "96") != 0) {
			wycheproof_skipped++;
			continue;
		}
		text = vec_get(&file, "group.tagSize");
		CHECK(text != NULL);
		if (text == NULL || strcmp(text, "128") != 0) {
			wycheproof_skipped++;
			continue;
		}
		text = vec_get(&file, "group.keySize");
		CHECK(text != NULL);
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

		/* A valid record seals to the ciphertext and tag and opens back. */
		accepted = strcmp(result, "valid") == 0;
		if (accepted)
			check_aead(key, nonce, aad, aad_size, message, size, cipher, tag);
		else
			check_open_fails(key, nonce, aad, aad_size, cipher, size, tag);
		wycheproof_checked++;
	}
	vec_close(&file);
}

/*
 * Checks seal against the reference construction for many sizes and random
 * keys, nonces and additional data, then the open round trip and the
 * refusal of a random bit change.
 */
static void
test_aead_generated(void)
{
	static const size_t sizes[] = {
		0U, 1U, 2U, 15U, 16U, 17U, 31U, 32U, 33U, 63U, 64U, 65U, 66U, 127U, 128U, 129U,
		191U, 192U, 193U, 255U, 256U, 257U, 1000U
	};
	static const size_t aad_sizes[] = {
		0U, 1U, 4U, 15U, 16U, 17U, 31U, 32U, 33U, 63U, 64U, 65U, 100U
	};
	uint8_t key[32];
	uint8_t nonce[12];
	uint8_t aad[128];
	uint8_t plain[MESSAGE_BYTES];
	uint8_t cipher[MESSAGE_BYTES];
	uint8_t tag[16];
	uint8_t changed[MESSAGE_BYTES];
	uint8_t changed_tag[16];
	size_t size;
	size_t aad_size;
	size_t round;
	size_t bit;

	/* Rounds with the boundary sizes first, then random ones. */
	for (round = 0; round < 800U; round++) {
		if (round < sizeof(sizes) / sizeof(sizes[0]) * 2U)
			size = sizes[round % (sizeof(sizes) / sizeof(sizes[0]))];
		else
			size = random_below(MESSAGE_BYTES);
		aad_size = aad_sizes[random_below(sizeof(aad_sizes) / sizeof(aad_sizes[0]))];
		if (round % 3U == 0U)
			aad_size = random_below(sizeof(aad));
		random_fill(key, sizeof(key));
		random_fill(nonce, sizeof(nonce));
		random_fill(aad, aad_size);
		random_fill(plain, size);
		if (round % 11U == 0U) {
			memset(plain, 0xff, size);
			memset(key, 0xff, 16U);
		}

		/* The reference result is the expectation. */
		reference_seal(key, nonce, aad, aad_size, plain, size, cipher, tag);
		check_aead(key, nonce, aad, aad_size, plain, size, cipher, tag);

		/* A random single-bit change of the ciphertext, the tag or the additional data is refused. */
		if (size > 0U) {
			memcpy(changed, cipher, size);
			flip_bit(changed, random_below(8U * size));
			check_open_fails(key, nonce, aad, aad_size, changed, size, tag);
		}
		memcpy(changed_tag, tag, sizeof(tag));
		bit = random_below(8U * sizeof(tag));
		flip_bit(changed_tag, bit);
		check_open_fails(key, nonce, aad, aad_size, cipher, size, changed_tag);
		if (aad_size > 0U) {
			memcpy(changed, aad, aad_size);
			flip_bit(changed, random_below(8U * aad_size));
			check_open_fails(key, nonce, changed, aad_size, cipher, size, tag);
		}
	}
}

/* Checks the size limits of seal and open: a message of more than 2^32 - 1 blocks is refused before anything is written. */
static void
test_aead_limits(void)
{
	uint8_t key[32];
	uint8_t nonce[12];
	uint8_t buffer[16 + 4];
	uint8_t tag[16];
	int error;
	size_t huge;

	/* A key and nonce. */
	random_fill(key, sizeof(key));
	random_fill(nonce, sizeof(nonce));

	/* A size_t of 32 bits cannot hold a size that large, so there is nothing to check. */
	if (sizeof(size_t) <= 4U)
		return;

	/* One byte more than (2^32 - 1) blocks of 64 bytes. */
	huge = (size_t)CRYC_U64(0x3fU, 0xffffffc1U);
	memset(buffer, SENTINEL_BYTE, sizeof(buffer));
	memset(tag, SENTINEL_BYTE, sizeof(tag));
	error = cryc_chacha20_poly1305_seal(key, nonce, NULL, 0U, buffer, huge, buffer, tag);
	CHECK(error == EOVERFLOW);
	CHECK(all_equal(buffer, sizeof(buffer), SENTINEL_BYTE));
	CHECK(all_equal(tag, sizeof(tag), SENTINEL_BYTE));
	error = cryc_chacha20_poly1305_open(key, nonce, NULL, 0U, buffer, huge, buffer, tag);
	CHECK(error == EOVERFLOW);
	CHECK(all_equal(buffer, sizeof(buffer), SENTINEL_BYTE));

	/* A far larger size too. */
	huge = (size_t)CRYC_U64(0xffffffffU, 0xffffffffU);
	error = cryc_chacha20_poly1305_seal(key, nonce, NULL, 0U, buffer, huge, buffer, tag);
	CHECK(error == EOVERFLOW);
	error = cryc_chacha20_poly1305_open(key, nonce, NULL, 0U, buffer, huge, buffer, tag);
	CHECK(error == EOVERFLOW);
	CHECK(all_equal(buffer, sizeof(buffer), SENTINEL_BYTE));
	CHECK(all_equal(tag, sizeof(tag), SENTINEL_BYTE));
}

/* Runs the checks. */
int
main(void)
{
	/* The block function and the cipher. */
	test_block();
	test_cipher();
	test_wide_block();

	/* Poly1305 and the key generation. */
	test_poly1305();
	test_keygen();

	/* The AEAD. */
	test_aead_rfc();
	test_aead_appendix();
	test_wycheproof();
	test_aead_generated();
	test_aead_limits();

	/* The Wycheproof counts must be what the file holds: 316 records with 96-bit nonces, the rest skipped. */
	CHECK(wycheproof_checked == 316UL);
	CHECK(wycheproof_skipped == 9UL);
	printf("wycheproof: %lu checked, %lu skipped\n", wycheproof_checked, wycheproof_skipped);

	/* The result. */
	return check_finish("test-chacha");
}
