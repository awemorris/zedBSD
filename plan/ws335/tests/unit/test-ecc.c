/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks key generation, ECDH and ECDSA verification on P-256 and P-384
 * (cryc-ecc.h).
 *
 * Key generation is driven by a scripted random source: the candidates are
 * chosen at the edges of FIPS 186-5 A.2.2 (0, n - 2, n - 1, n, n + 1, all
 * ones, a carry across every limb), the number of draws, the size of each
 * request and the wiping on failure are observed, and the CAVP key pairs
 * are reproduced by feeding d - 1.
 *
 * ECDH is checked against the CAVP ECC CDH primitive cases (all 25 of P-256
 * and of P-384, typed in below from the published file), the Wycheproof
 * P-256 cases, the identities x(d * Q) = x((n - d) * Q) and
 * x(d * G) = the public key's x, shared secrets whose first byte is zero,
 * and every error of the header.
 *
 * ECDSA verification is checked against the CAVP SigVer cases (P-256 and
 * P-384 with SHA-256, SHA-384 and SHA-512), the Wycheproof cases (the DER
 * signature is decoded here, strictly), and signatures made here by a
 * signer written from FIPS 186-5 6.4.1: round trips, the effect of every
 * field changed, the leftmost-bits rule for long and short hashes, E
 * at or above n, (r, n - s), a result at infinity, and a result whose x
 * coordinate is at or above n (a key is built so that the verifier's R is
 * a chosen such point).
 *
 * Expected values come from the standards' vectors and from arithmetic
 * done here (the group, the order, the inverse), not from another
 * implementation.
 */

#include "check.h"
#include "vecfile.h"

#include "userland/base/libcrypto-compat/cryc-ecc.h"
#include "userland/base/libcrypto-compat/cryc-hash.h"

/* The value of a fill that shows a byte was not written. */
#define SENTINEL_BYTE	0x5aU

/* The most candidates a script holds (one more than the tries). */
#define SCRIPT_MAX	70U

/* The largest hash used by the tests, in bytes. */
#define HASH_MAX	64U

/* The most draws of random points looked for with a zero first byte. */
#define ZERO_BYTE_TRIES	6000U

/* The number of CAVP ECC CDH cases of each curve typed in below. */
#define CAVP_ECDH_COUNT	25U

/* The state of the pseudo-random generator of the tests (xorshift32, Marsaglia 2003). */
static uint32_t rng_state = 0x3c6ef372U;

/* Returns the next pseudo-random 32-bit value. */
static uint32_t
random32(void)
{
	/* Marsaglia's xorshift32 with the shifts 13, 17, 5. */
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

/* The parameters of one curve as hex text: the order and the base point. */
struct params {
	enum cryc_curve_id id;
	const char *name;
	const char *cavp_key;
	size_t size;
	const char *n;
	const char *gx;
	const char *gy;
};

/*
 * The parameters typed again from the pages.
 *
 * Values referenced: NIST SP 800-186 (February 2023) 3.2.1.3 and 3.2.1.4.
 */
static const struct params curves[2] = {
	{
		CRYC_CURVE_P256, "P-256", "P-256", 32U,
		"ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551",
		"6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296",
		"4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5"
	},
	{
		CRYC_CURVE_P384, "P-384", "P-384", 48U,
		"ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973",
		"aa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a385502f25dbf55296c3a545e3872760ab7",
		"3617de4a96262c6f5d9e98bf9292dc29f8f41dbd289a147ce9da3113b5f0b8c00a60b1ce1d7e819d7a431d7c90ea0e5f"
	}
};

/* One case of the CAVP ECC CDH primitive file: the peer's point, the private key, the public key made from it, and the shared x. */
struct cavp_ecdh {
	const char *peer_x;
	const char *peer_y;
	const char *private_key;
	const char *public_x;
	const char *public_y;
	const char *shared;
};

/*
 * The CAVP ECC CDH primitive cases (SP 800-56A Section 5.7.1.2) of P-256
 * and P-384: QCAVS is the peer's point, dIUT the private key, QIUT its
 * public key, ZIUT the shared secret.
 *
 * Values referenced: NIST CAVP KAS_ECC_CDH_PrimitiveTest.txt (CAVS 14.1,
 * ecccdhtestvectors.zip), the [P-256] and [P-384] sections, COUNT 0 to 24.
 */
static const struct cavp_ecdh cavp_ecdh_p256[CAVP_ECDH_COUNT] = {
	{
		"700c48f77f56584c5cc632ca65640db91b6bacce3a4df6b42ce7cc838833d287",
		"db71e509e3fd9b060ddb20ba5c51dcc5948d46fbf640dfe0441782cab85fa4ac",
		"7d7dc5f71eb29ddaf80d6214632eeae03d9058af1fb6d22ed80badb62bc1a534",
		"ead218590119e8876b29146ff89ca61770c4edbbf97d38ce385ed281d8a6b230",
		"28af61281fd35e2fa7002523acc85a429cb06ee6648325389f59edfce1405141",
		"46fc62106420ff012e54a434fbdd2d25ccc5852060561e68040dd7778997bd7b"
	},
	{
		"809f04289c64348c01515eb03d5ce7ac1a8cb9498f5caa50197e58d43a86a7ae",
		"b29d84e811197f25eba8f5194092cb6ff440e26d4421011372461f579271cda3",
		"38f65d6dce47676044d58ce5139582d568f64bb16098d179dbab07741dd5caf5",
		"119f2f047902782ab0c9e27a54aff5eb9b964829ca99c06b02ddba95b0a3f6d0",
		"8f52b726664cac366fc98ac7a012b2682cbd962e5acb544671d41b9445704d1d",
		"057d636096cb80b67a8c038c890e887d1adfa4195e9b3ce241c8a778c59cda67"
	},
	{
		"a2339c12d4a03c33546de533268b4ad667debf458b464d77443636440ee7fec3",
		"ef48a3ab26e20220bcda2c1851076839dae88eae962869a497bf73cb66faf536",
		"1accfaf1b97712b85a6f54b148985a1bdc4c9bec0bd258cad4b3d603f49f32c8",
		"d9f2b79c172845bfdb560bbb01447ca5ecc0470a09513b6126902c6b4f8d1051",
		"f815ef5ec32128d3487834764678702e64e164ff7315185e23aff5facd96d7bc",
		"2d457b78b4614132477618a5b077965ec90730a8c81a1c75d6d4ec68005d67ec"
	},
	{
		"df3989b9fa55495719b3cf46dccd28b5153f7808191dd518eff0c3cff2b705ed",
		"422294ff46003429d739a33206c8752552c8ba54a270defc06e221e0feaf6ac4",
		"207c43a79bfee03db6f4b944f53d2fb76cc49ef1c9c4d34d51b6c65c4db6932d",
		"24277c33f450462dcb3d4801d57b9ced05188f16c28eda873258048cd1607e0d",
		"c4789753e2b1f63b32ff014ec42cd6a69fac81dfe6d0d6fd4af372ae27c46f88",
		"96441259534b80f6aee3d287a6bb17b5094dd4277d9e294f8fe73e48bf2a0024"
	},
	{
		"41192d2813e79561e6a1d6f53c8bc1a433a199c835e141b05a74a97b0faeb922",
		"1af98cc45e98a7e041b01cf35f462b7562281351c8ebf3ffa02e33a0722a1328",
		"59137e38152350b195c9718d39673d519838055ad908dd4757152fd8255c09bf",
		"a8c5fdce8b62c5ada598f141adb3b26cf254c280b2857a63d2ad783a73115f6b",
		"806e1aafec4af80a0d786b3de45375b517a7e5b51ffb2c356537c9e6ef227d4a",
		"19d44c8d63e8e8dd12c22a87b8cd4ece27acdde04dbf47f7f27537a6999a8e62"
	},
	{
		"33e82092a0f1fb38f5649d5867fba28b503172b7035574bf8e5b7100a3052792",
		"f2cf6b601e0a05945e335550bf648d782f46186c772c0f20d3cd0d6b8ca14b2f",
		"f5f8e0174610a661277979b58ce5c90fee6c9b3bb346a90a7196255e40b132ef",
		"7b861dcd2844a5a8363f6b8ef8d493640f55879217189d80326aad9480dfc149",
		"c4675b45eeb306405f6c33c38bc69eb2bdec9b75ad5af4706aab84543b9cc63a",
		"664e45d5bba4ac931cd65d52017e4be9b19a515f669bea4703542a2c525cd3d3"
	},
	{
		"6a9e0c3f916e4e315c91147be571686d90464e8bf981d34a90b6353bca6eeba7",
		"40f9bead39c2f2bcc2602f75b8a73ec7bdffcbcead159d0174c6c4d3c5357f05",
		"3b589af7db03459c23068b64f63f28d3c3c6bc25b5bf76ac05f35482888b5190",
		"9fb38e2d58ea1baf7622e96720101cae3cde4ba6c1e9fa26d9b1de0899102863",
		"d5561b900406edf50802dd7d73e89395f8aed72fba0e1d1b61fe1d22302260f0",
		"ca342daa50dc09d61be7c196c85e60a80c5cb04931746820be548cdde055679d"
	},
	{
		"a9c0acade55c2a73ead1a86fb0a9713223c82475791cd0e210b046412ce224bb",
		"f6de0afa20e93e078467c053d241903edad734c6b403ba758c2b5ff04c9d4229",
		"d8bf929a20ea7436b2461b541a11c80e61d826c0a4c9d322b31dd54e7f58b9c8",
		"20f07631e4a6512a89ad487c4e9d63039e579cb0d7a556cb9e661cd59c1e7fa4",
		"6de91846b3eee8a5ec09c2ab1f41e21bd83620ccdd1bdce3ab7ea6e02dd274f5",
		"35aa9b52536a461bfde4e85fc756be928c7de97923f0416c7a3ac8f88b3d4489"
	},
	{
		"94e94f16a98255fff2b9ac0c9598aac35487b3232d3231bd93b7db7df36f9eb9",
		"d8049a43579cfa90b8093a94416cbefbf93386f15b3f6e190b6e3455fedfe69a",
		"0f9883ba0ef32ee75ded0d8bda39a5146a29f1f2507b3bd458dbea0b2bb05b4d",
		"abb61b423be5d6c26e21c605832c9142dc1dfe5a5fff28726737936e6fbf516d",
		"733d2513ef58beab202090586fac91bf0fee31e80ab33473ab23a2d89e58fad6",
		"605c16178a9bc875dcbff54d63fe00df699c03e8a888e9e94dfbab90b25f39b4"
	},
	{
		"e099bf2a4d557460b5544430bbf6da11004d127cb5d67f64ab07c94fcdf5274f",
		"d9c50dbe70d714edb5e221f4e020610eeb6270517e688ca64fb0e98c7ef8c1c5",
		"2beedb04b05c6988f6a67500bb813faf2cae0d580c9253b6339e4a3337bb6c08",
		"3d63e429cb5fa895a9247129bf4e48e89f35d7b11de8158efeb3e106a2a87395",
		"0cae9e477ef41e7c8c1064379bb7b554ddcbcae79f9814281f1e50f0403c61f3",
		"f96e40a1b72840854bb62bc13c40cc2795e373d4e715980b261476835a092e0b"
	},
	{
		"f75a5fe56bda34f3c1396296626ef012dc07e4825838778a645c8248cff01658",
		"33bbdf1b1772d8059df568b061f3f1122f28a8d819167c97be448e3dc3fb0c3c",
		"77c15dcf44610e41696bab758943eff1409333e4d5a11bbe72c8f6c395e9f848",
		"ad5d13c3db508ddcd38457e5991434a251bed49cf5ddcb59cdee73865f138c9f",
		"62cec1e70588aa4fdfc7b9a09daa678081c04e1208b9d662b8a2214bf8e81a21",
		"8388fa79c4babdca02a8e8a34f9e43554976e420a4ad273c81b26e4228e9d3a3"
	},
	{
		"2db4540d50230756158abf61d9835712b6486c74312183ccefcaef2797b7674d",
		"62f57f314e3f3495dc4e099012f5e0ba71770f9660a1eada54104cdfde77243e",
		"42a83b985011d12303db1a800f2610f74aa71cdf19c67d54ce6c9ed951e9093e",
		"ab48caa61ea35f13f8ed07ffa6a13e8db224dfecfae1a7df8b1bb6ebaf0cb97d",
		"1274530ca2c385a3218bddfbcbf0b4024c9badd5243bff834ebff24a8618dccb",
		"72877cea33ccc4715038d4bcbdfe0e43f42a9e2c0c3b017fc2370f4b9acbda4a"
	},
	{
		"cd94fc9497e8990750309e9a8534fd114b0a6e54da89c4796101897041d14ecb",
		"c3def4b5fe04faee0a11932229fff563637bfdee0e79c6deeaf449f85401c5c4",
		"ceed35507b5c93ead5989119b9ba342cfe38e6e638ba6eea343a55475de2800b",
		"9a8cd9bd72e71752df91440f77c547509a84df98114e7de4f26cdb39234a625d",
		"d07cfc84c8e144fab2839f5189bb1d7c88631d579bbc58012ed9a2327da52f62",
		"e4e7408d85ff0e0e9c838003f28cdbd5247cdce31f32f62494b70e5f1bc36307"
	},
	{
		"15b9e467af4d290c417402e040426fe4cf236bae72baa392ed89780dfccdb471",
		"cdf4e9170fb904302b8fd93a820ba8cc7ed4efd3a6f2d6b05b80b2ff2aee4e77",
		"43e0e9d95af4dc36483cdd1968d2b7eeb8611fcce77f3a4e7d059ae43e509604",
		"f989cf8ee956a82e7ebd9881cdbfb2fd946189b08db53559bc8cfdd48071eb14",
		"5eff28f1a18a616b04b7d337868679f6dd84f9a7b3d7b6f8af276c19611a541d",
		"ed56bcf695b734142c24ecb1fc1bb64d08f175eb243a31f37b3d9bb4407f3b96"
	},
	{
		"49c503ba6c4fa605182e186b5e81113f075bc11dcfd51c932fb21e951eee2fa1",
		"8af706ff0922d87b3f0c5e4e31d8b259aeb260a9269643ed520a13bb25da5924",
		"b2f3600df3368ef8a0bb85ab22f41fc0e5f4fdd54be8167a5c3cd4b08db04903",
		"69c627625b36a429c398b45c38677cb35d8beb1cf78a571e40e99fe4eac1cd4e",
		"81690112b0a88f20f7136b28d7d47e5fbc2ada3c8edd87589bc19ec9590637bd",
		"bc5c7055089fc9d6c89f83c1ea1ada879d9934b2ea28fcf4e4a7e984b28ad2cf"
	},
	{
		"19b38de39fdd2f70f7091631a4f75d1993740ba9429162c2a45312401636b29c",
		"09aed7232b28e060941741b6828bcdfa2bc49cc844f3773611504f82a390a5ae",
		"4002534307f8b62a9bf67ff641ddc60fef593b17c3341239e95bdb3e579bfdc8",
		"5fe964671315a18aa68a2a6e3dd1fde7e23b8ce7181471cfac43c99e1ae80262",
		"d5827be282e62c84de531b963884ba832db5d6b2c3a256f0e604fe7e6b8a7f72",
		"9a4e8e657f6b0e097f47954a63c75d74fcba71a30d83651e3e5a91aa7ccd8343"
	},
	{
		"2c91c61f33adfe9311c942fdbff6ba47020feff416b7bb63cec13faf9b099954",
		"6cab31b06419e5221fca014fb84ec870622a1b12bab5ae43682aa7ea73ea08d0",
		"4dfa12defc60319021b681b3ff84a10a511958c850939ed45635934ba4979147",
		"c9b2b8496f1440bd4a2d1e52752fd372835b364885e154a7dac49295f281ec7c",
		"fbe6b926a8a4de26ccc83b802b1212400754be25d9f3eeaf008b09870ae76321",
		"3ca1fc7ad858fb1a6aba232542f3e2a749ffc7203a2374a3f3d3267f1fc97b78"
	},
	{
		"a28a2edf58025668f724aaf83a50956b7ac1cfbbff79b08c3bf87dfd2828d767",
		"dfa7bfffd4c766b86abeaf5c99b6e50cb9ccc9d9d00b7ffc7804b0491b67bc03",
		"1331f6d874a4ed3bc4a2c6e9c74331d3039796314beee3b7152fcdba5556304e",
		"59e1e101521046ad9cf1d082e9d2ec7dd22530cce064991f1e55c5bcf5fcb591",
		"482f4f673176c8fdaa0bb6e59b15a3e47454e3a04297d3863c9338d98add1f37",
		"1aaabe7ee6e4a6fa732291202433a237df1b49bc53866bfbe00db96a0f58224f"
	},
	{
		"a2ef857a081f9d6eb206a81c4cf78a802bdf598ae380c8886ecd85fdc1ed7644",
		"563c4c20419f07bc17d0539fade1855e34839515b892c0f5d26561f97fa04d1a",
		"dd5e9f70ae740073ca0204df60763fb6036c45709bf4a7bb4e671412fad65da3",
		"30b9db2e2e977bcdc98cb87dd736cbd8e78552121925cf16e1933657c2fb2314",
		"6a45028800b81291bce5c2e1fed7ded650620ebbe6050c6f3a7f0dfb4673ab5c",
		"430e6a4fba4449d700d2733e557f66a3bf3d50517c1271b1ddae1161b7ac798c"
	},
	{
		"ccd8a2d86bc92f2e01bce4d6922cf7fe1626aed044685e95e2eebd464505f01f",
		"e9ddd583a9635a667777d5b8a8f31b0f79eba12c75023410b54b8567dddc0f38",
		"5ae026cfc060d55600717e55b8a12e116d1d0df34af831979057607c2d9c2f76",
		"46c9ebd1a4a3c8c0b6d572b5dcfba12467603208a9cb5d2acfbb733c40cf6391",
		"46c913a27d044185d38b467ace011e04d4d9bbbb8cb9ae25fa92aaf15a595e86",
		"1ce9e6740529499f98d1f1d71329147a33df1d05e4765b539b11cf615d6974d3"
	},
	{
		"c188ffc8947f7301fb7b53e36746097c2134bf9cc981ba74b4e9c4361f595e4e",
		"bf7d2f2056e72421ef393f0c0f2b0e00130e3cac4abbcc00286168e85ec55051",
		"b601ac425d5dbf9e1735c5e2d5bdb79ca98b3d5be4a2cfd6f2273f150e064d9d",
		"7c9e950841d26c8dde8994398b8f5d475a022bc63de7773fcf8d552e01f1ba0a",
		"cc42b9885c9b3bee0f8d8c57d3a8f6355016c019c4062fa22cff2f209b5cc2e1",
		"4690e3743c07d643f1bc183636ab2a9cb936a60a802113c49bb1b3f2d0661660"
	},
	{
		"317e1020ff53fccef18bf47bb7f2dd7707fb7b7a7578e04f35b3beed222a0eb6",
		"09420ce5a19d77c6fe1ee587e6a49fbaf8f280e8df033d75403302e5a27db2ae",
		"fefb1dda1845312b5fce6b81b2be205af2f3a274f5a212f66c0d9fc33d7ae535",
		"38b54db85500cb20c61056edd3d88b6a9dc26780a047f213a6e1b900f76596eb",
		"6387e4e5781571e4eb8ae62991a33b5dc33301c5bc7e125d53794a39160d8fd0",
		"30c2261bd0004e61feda2c16aa5e21ffa8d7e7f7dbf6ec379a43b48e4b36aeb0"
	},
	{
		"45fb02b2ceb9d7c79d9c2fa93e9c7967c2fa4df5789f9640b24264b1e524fcb1",
		"5c6e8ecf1f7d3023893b7b1ca1e4d178972ee2a230757ddc564ffe37f5c5a321",
		"334ae0c4693d23935a7e8e043ebbde21e168a7cba3fa507c9be41d7681e049ce",
		"3f2bf1589abf3047bf3e54ac9a95379bff95f8f55405f64eca36a7eebe8ffca7",
		"5212a94e66c5ae9a8991872f66a72723d80ec5b2e925745c456f5371943b3a06",
		"2adae4a138a239dcd93c243a3803c3e4cf96e37fe14e6a9b717be9599959b11c"
	},
	{
		"a19ef7bff98ada781842fbfc51a47aff39b5935a1c7d9625c8d323d511c92de6",
		"e9c184df75c955e02e02e400ffe45f78f339e1afe6d056fb3245f4700ce606ef",
		"2c4bde40214fcc3bfc47d4cf434b629acbe9157f8fd0282540331de7942cf09d",
		"29c0807f10cbc42fb45c9989da50681eead716daa7b9e91fd32e062f5eb92ca0",
		"ff1d6d1955d7376b2da24fe1163a271659136341bc2eb1195fc706dc62e7f34d",
		"2e277ec30f5ea07d6ce513149b9479b96e07f4b6913b1b5c11305c1444a1bc0b"
	},
	{
		"356c5a444c049a52fee0adeb7e5d82ae5aa83030bfff31bbf8ce2096cf161c4b",
		"57d128de8b2a57a094d1a001e572173f96e8866ae352bf29cddaf92fc85b2f92",
		"85a268f9d7772f990c36b42b0a331adc92b5941de0b862d5d89a347cbf8faab0",
		"9cf4b98581ca1779453cc816ff28b4100af56cf1bf2e5bc312d83b6b1b21d333",
		"7a5504fcac5231a0d12d658218284868229c844a04a3450d6c7381abe080bf3b",
		"1e51373bd2c6044c129c436e742a55be2a668a85ae08441b6756445df5493857"
	},
};

static const struct cavp_ecdh cavp_ecdh_p384[CAVP_ECDH_COUNT] = {
	{
		"a7c76b970c3b5fe8b05d2838ae04ab47697b9eaf52e764592efda27fe7513272734466b400091adbf2d68c58e0c50066",
		"ac68f19f2e1cb879aed43a9969b91a0839c4c38a49749b661efedf243451915ed0905a32b060992b468c64766fc8437a",
		"3cc3122a68f0d95027ad38c067916ba0eb8c38894d22e1b15618b6818a661774ad463b205da88cf699ab4d43c9cf98a1",
		"9803807f2f6d2fd966cdd0290bd410c0190352fbec7ff6247de1302df86f25d34fe4a97bef60cff548355c015dbb3e5f",
		"ba26ca69ec2f5b5d9dad20cc9da711383a9dbe34ea3fa5a2af75b46502629ad54dd8b7d73a8abb06a3a3be47d650cc99",
		"5f9d29dc5e31a163060356213669c8ce132e22f57c9a04f40ba7fcead493b457e5621e766c40a2e3d4d6a04b25e533f1"
	},
	{
		"30f43fcf2b6b00de53f624f1543090681839717d53c7c955d1d69efaf0349b7363acb447240101cbb3af6641ce4b88e0",
		"25e46c0c54f0162a77efcc27b6ea792002ae2ba82714299c860857a68153ab62e525ec0530d81b5aa15897981e858757",
		"92860c21bde06165f8e900c687f8ef0a05d14f290b3f07d8b3a8cc6404366e5d5119cd6d03fb12dc58e89f13df9cd783",
		"ea4018f5a307c379180bf6a62fd2ceceebeeb7d4df063a66fb838aa35243419791f7e2c9d4803c9319aa0eb03c416b66",
		"68835a91484f05ef028284df6436fb88ffebabcdd69ab0133e6735a1bcfb37203d10d340a8328a7b68770ca75878a1a6",
		"a23742a2c267d7425fda94b93f93bbcc24791ac51cd8fd501a238d40812f4cbfc59aac9520d758cf789c76300c69d2ff"
	},
	{
		"1aefbfa2c6c8c855a1a216774550b79a24cda37607bb1f7cc906650ee4b3816d68f6a9c75da6e4242cebfb6652f65180",
		"419d28b723ebadb7658fcebb9ad9b7adea674f1da3dc6b6397b55da0f61a3eddacb4acdb14441cb214b04a0844c02fa3",
		"12cf6a223a72352543830f3f18530d5cb37f26880a0b294482c8a8ef8afad09aa78b7dc2f2789a78c66af5d1cc553853",
		"fcfcea085e8cf74d0dced1620ba8423694f903a219bbf901b0b59d6ac81baad316a242ba32bde85cb248119b852fab66",
		"972e3c68c7ab402c5836f2a16ed451a33120a7750a6039f3ff15388ee622b7065f7122bf6d51aefbc29b37b03404581b",
		"3d2e640f350805eed1ff43b40a72b2abed0a518bcebe8f2d15b111b6773223da3c3489121db173d414b5bd5ad7153435"
	},
	{
		"8bc089326ec55b9cf59b34f0eb754d93596ca290fcb3444c83d4de3a5607037ec397683f8cef07eab2fe357eae36c449",
		"d9d16ce8ac85b3f1e94568521aae534e67139e310ec72693526aa2e927b5b322c95a1a033c229cb6770c957cd3148dd7",
		"8dd48063a3a058c334b5cc7a4ce07d02e5ee6d8f1f3c51a1600962cbab462690ae3cd974fb39e40b0e843daa0fd32de1",
		"e38c9846248123c3421861ea4d32669a7b5c3c08376ad28104399494c84ff5efa3894adb2c6cbe8c3c913ef2eec5bd3c",
		"9fa84024a1028796df84021f7b6c9d02f0f4bd1a612a03cbf75a0beea43fef8ae84b48c60172aadf09c1ad016d0bf3ce",
		"6a42cfc392aba0bfd3d17b7ccf062b91fc09bbf3417612d02a90bdde62ae40c54bb2e56e167d6b70db670097eb8db854"
	},
	{
		"eb952e2d9ac0c20c6cc48fb225c2ad154f53c8750b003fd3b4ed8ed1dc0defac61bcdde02a2bcfee7067d75d342ed2b0",
		"f1828205baece82d1b267d0d7ff2f9c9e15b69a72df47058a97f3891005d1fb38858f5603de840e591dfa4f6e7d489e1",
		"84ece6cc3429309bd5b23e959793ed2b111ec5cb43b6c18085fcaea9efa0685d98a6262ee0d330ee250bc8a67d0e733f",
		"3222063a2997b302ee60ee1961108ff4c7acf1c0ef1d5fb0d164b84bce71c431705cb9aea9a45f5d73806655a058bee3",
		"e61fa9e7fbe7cd43abf99596a3d3a039e99fa9dc93b0bdd9cad81966d17eeaf557068afa7c78466bb5b22032d1100fa6",
		"ce7ba454d4412729a32bb833a2d1fd2ae612d4667c3a900e069214818613447df8c611de66da200db7c375cf913e4405"
	},
	{
		"441d029e244eb7168d647d4df50db5f4e4974ab3fdaf022aff058b3695d0b8c814cc88da6285dc6df1ac55c553885003",
		"e8025ac23a41d4b1ea2aa46c50c6e479946b59b6d76497cd9249977e0bfe4a6262622f13d42a3c43d66bdbb30403c345",
		"68fce2121dc3a1e37b10f1dde309f9e2e18fac47cd1770951451c3484cdb77cb136d00e731260597cc2859601c01a25b",
		"868be0e694841830e424d913d8e7d86b84ee1021d82b0ecf523f09fe89a76c0c95c49f2dfbcf829c1e39709d55efbb3b",
		"9195eb183675b40fd92f51f37713317e4a9b4f715c8ab22e0773b1bc71d3a219f05b8116074658ee86b52e36f3897116",
		"ba69f0acdf3e1ca95caaac4ecaf475bbe51b54777efce01ca381f45370e486fe87f9f419b150c61e329a286d1aa265ec"
	},
	{
		"3d4e6bf08a73404accc1629873468e4269e82d90d832e58ad72142639b5a056ad8d35c66c60e8149fac0c797bceb7c2f",
		"9b0308dc7f0e6d29f8c277acbc65a21e5adb83d11e6873bc0a07fda0997f482504602f59e10bc5cb476b83d0a4f75e71",
		"b1764c54897e7aae6de9e7751f2f37de849291f88f0f91093155b858d1cc32a3a87980f706b86cc83f927bdfdbeae0bd",
		"c371222feaa6770c6f3ea3e0dac9740def4fcf821378b7f91ff937c21e0470f70f3a31d5c6b2912195f10926942b48ae",
		"047d6b4d765123563f81116bc665b7b8cc6207830d805fd84da7cb805a65baa7c12fd592d1b5b5e3e65d9672a9ef7662",
		"1a6688ee1d6e59865d8e3ada37781d36bb0c2717eef92e61964d3927cb765c2965ea80f7f63e58c322ba0397faeaf62b"
	},
	{
		"f5f6bef1d110da03be0017eac760cc34b24d092f736f237bc7054b3865312a813bcb62d297fb10a4f7abf54708fe2d3d",
		"06fdf8d7dc032f4e10010bf19cbf6159321252ff415fb91920d438f24e67e60c2eb0463204679fa356af44cea9c9ebf5",
		"f0f7a96e70d98fd5a30ad6406cf56eb5b72a510e9f192f50e1f84524dbf3d2439f7287bb36f5aa912a79deaab4adea82",
		"99c8c41cb1ab5e0854a346e4b08a537c1706a61553387c8d94943ab15196d40dbaa55b8210a77a5d00915f2c4ea69eab",
		"5531065bdcf17bfb3cb55a02e41a57c7f694c383ad289f900fbd656c2233a93c92e933e7a26f54cbb56f0ad875c51bb0",
		"d06a568bf2336b90cbac325161be7695eacb2295f599500d787f072612aca313ee5d874f807ddef6c1f023fe2b6e7cd0"
	},
	{
		"7cdec77e0737ea37c67b89b7137fe38818010f4464438ee4d1d35a0c488cad3fde2f37d00885d36d3b795b9f93d23a67",
		"28c42ee8d6027c56cf979ba4c229fdb01d234944f8ac433650112c3cf0f02844e888a3569dfef7828a8a884589aa055e",
		"9efb87ddc61d43c482ba66e1b143aef678fbd0d1bebc2000941fabe677fe5b706bf78fce36d100b17cc787ead74bbca2",
		"4c34efee8f0c95565d2065d1bbac2a2dd25ae964320eb6bccedc5f3a9b42a881a1afca1bb6b880584fa27b01c193cd92",
		"d8fb01dbf7cd0a3868c26b951f393c3c56c2858cee901f7793ff5d271925d13a41f8e52409f4eba1990f33acb0bac669",
		"bb3b1eda9c6560d82ff5bee403339f1e80342338a991344853b56b24f109a4d94b92f654f0425edd4c205903d7586104"
	},
	{
		"8eeea3a319c8df99fbc29cb55f243a720d95509515ee5cc587a5c5ae22fbbd009e626db3e911def0b99a4f7ae304b1ba",
		"73877dc94db9adddc0d9a4b24e8976c22d73c844370e1ee857f8d1b129a3bd5f63f40caf3bd0533e38a5f5777074ff9e",
		"d787a57fde22ec656a0a525cf3c738b30d73af61e743ea90893ecb2d7b622add2f94ee25c2171467afb093f3f84d0018",
		"171546923b87b2cbbad664f01ce932bf09d6a6118168678446bfa9f0938608cb4667a98f4ec8ac1462285c2508f74862",
		"fa41cb4db68ae71f1f8a3e8939dc52c2dec61a83c983beb2a02baf29ec49278088882ed0cf56c74b5c173b552ccf63cf",
		"1e97b60add7cb35c7403dd884c0a75795b7683fff8b49f9d8672a8206bfdcf0a106b8768f983258c74167422e44e4d14"
	},
	{
		"a721f6a2d4527411834b13d4d3a33c29beb83ab7682465c6cbaf6624aca6ea58c30eb0f29dd842886695400d7254f20f",
		"14ba6e26355109ad35129366d5e3a640ae798505a7fa55a96a36b5dad33de00474f6670f522214dd7952140ab0a7eb68",
		"83d70f7b164d9f4c227c767046b20eb34dfc778f5387e32e834b1e6daec20edb8ca5bb4192093f543b68e6aeb7ce788b",
		"57cd770f3bbcbe0c78c770eab0b169bc45e139f86378ffae1c2b16966727c2f2eb724572b8f3eb228d130db4ff862c63",
		"7ec5c8813b685558d83e924f14bc719f6eb7ae0cbb2c474227c5bda88637a4f26c64817929af999592da6f787490332f",
		"1023478840e54775bfc69293a3cf97f5bc914726455c66538eb5623e218feef7df4befa23e09d77145ad577db32b41f9"
	},
	{
		"d882a8505c2d5cb9b8851fc676677bb0087681ad53faceba1738286b45827561e7da37b880276c656cfc38b32ade847e",
		"34b314bdc134575654573cffaf40445da2e6aaf987f7e913cd4c3091523058984a25d8f21da8326192456c6a0fa5f60c",
		"8f558e05818b88ed383d5fca962e53413db1a0e4637eda194f761944cbea114ab9d5da175a7d57882550b0e432f395a9",
		"9a2f57f4867ce753d72b0d95195df6f96c1fae934f602efd7b6a54582f556cfa539d89005ca2edac08ad9b72dd1f60ba",
		"d9b94ee82da9cc601f346044998ba387aee56404dc6ecc8ab2b590443319d0b2b6176f9d0eac2d44678ed561607d09a9",
		"6ad6b9dc8a6cf0d3691c501cbb967867f6e4bbb764b60dbff8fcff3ed42dbba39d63cf325b4b4078858495ddee75f954"
	},
	{
		"815c9d773dbf5fb6a1b86799966247f4006a23c92e68c55e9eaa998b17d8832dd4d84d927d831d4f68dac67c6488219f",
		"e79269948b2611484560fd490feec887cb55ef99a4b524880fa7499d6a07283aae2afa33feab97deca40bc606c4d8764",
		"0f5dee0affa7bbf239d5dff32987ebb7cf84fcceed643e1d3c62d0b3352aec23b6e5ac7fa4105c8cb26126ad2d1892cb",
		"23346bdfbc9d7c7c736e02bdf607671ff6082fdd27334a8bc75f3b23681ebe614d0597dd614fae58677c835a9f0b273b",
		"82ba36290d2f94db41479eb45ab4eaf67928a2315138d59eecc9b5285dfddd6714f77557216ea44cc6fc119d8243efaf",
		"cc9e063566d46b357b3fcae21827377331e5e290a36e60cd7c39102b828ae0b918dc5a02216b07fe6f1958d834e42437"
	},
	{
		"1c0eeda7a2be000c5bdcda0478aed4db733d2a9e341224379123ad847030f29e3b168fa18e89a3c0fba2a6ce1c28fc3b",
		"ec8c1c83c118c4dbea94271869f2d868eb65e8b44e21e6f14b0f4d9b38c068daefa27114255b9a41d084cc4a1ad85456",
		"037b633b5b8ba857c0fc85656868232e2febf59578718391b81da8541a00bfe53c30ae04151847f27499f8d7abad8cf4",
		"8878ac8a947f7d5cb2b47aad24fbb8210d86126585399a2871f84aa9c5fde3074ae540c6bf82275ca822d0feb862bc74",
		"632f5cd2f900c2711c32f8930728eb647d31edd8d650f9654e7d33e5ed1b475489d08daa30d8cbcba6bfc3b60d9b5a37",
		"deff7f03bd09865baf945e73edff6d5122c03fb561db87dec8662e09bed4340b28a9efe118337bb7d3d4f7f568635ff9"
	},
	{
		"c95c185e256bf997f30b311548ae7f768a38dee43eeeef43083f3077be70e2bf39ac1d4daf360c514c8c6be623443d1a",
		"3e63a663eaf75d8a765ab2b9a35513d7933fa5e26420a5244550ec6c3b6f033b96db2aca3d6ac6aab052ce929595aea5",
		"e3d07106bedcc096e7d91630ffd3094df2c7859db8d7edbb2e37b4ac47f429a637d06a67d2fba33838764ef203464991",
		"e74a1a2b85f1cbf8dbbdf050cf1aff8acb02fda2fb6591f9d3cfe4e79d0ae938a9c1483e7b75f8db24505d65065cdb18",
		"1773ee591822f7abaa856a1a60bc0a5203548dbd1cb5025466eff8481bd07614eaa04a16c3db76905913e972a5b6b59d",
		"c8b1038f735ad3bb3e4637c3e47eab487637911a6b7950a4e461948329d3923b969e5db663675623611a457fcda35a71"
	},
	{
		"3497238a7e6ad166df2dac039aa4dac8d17aa925e7c7631eb3b56e3aaa1c545fcd54d2e5985807910fb202b1fc191d2a",
		"a49e5c487dcc7aa40a8f234c979446040d9174e3ad357d404d7765183195aed3f913641b90c81a306ebf0d8913861316",
		"f3f9b0c65a49a506632c8a45b10f66b5316f9eeb06fae218f2da62333f99905117b141c760e8974efc4af10570635791",
		"a4ad77aa7d86e5361118a6b921710c820721210712f4c347985fdee58aa4effa1e28be80a17b120b139f96300f89b49b",
		"1ddf22e07e03f1560d8f45a480094560dba9fae7f9531130c1b57ebb95982496524f31d3797793396fa823f22bdb4328",
		"d337eaa32b9f716b8747b005b97a553c59dab0c51df41a2d49039cdae705aa75c7b9e7bc0b6a0e8c578c902bc4fff23e"
	},
	{
		"90a34737d45b1aa65f74e0bd0659bc118f8e4b774b761944ffa6573c6df4f41dec0d11b697abd934d390871d4b453240",
		"9b590719bb3307c149a7817be355d684893a307764b512eeffe07cb699edb5a6ffbf8d6032e6c79d5e93e94212c2aa4e",
		"59fce7fad7de28bac0230690c95710c720e528f9a4e54d3a6a8cd5fc5c5f21637031ce1c5b4e3d39647d8dcb9b794664",
		"9c43bf971edf09402876ee742095381f78b1bd3aa39b5132af75dbfe7e98bd78bde10fe2e903c2b6379e1deee175a1b0",
		"a6c58ecea5a477bb01bd543b339f1cc49f1371a2cda4d46eb4e53e250597942351a99665a122ffea9bde0636c375daf2",
		"32d292b695a4488e42a7b7922e1ae537d76a3d21a0b2e36875f60e9f6d3e8779c2afb3a413b9dd79ae18e70b47d337c1"
	},
	{
		"dda546acfc8f903d11e2e3920669636d44b2068aeb66ff07aa266f0030e1535b0ed0203cb8a460ac990f1394faf22f1d",
		"15bbb2597913035faadf413476f4c70f7279769a40c986f470c427b4ee4962abdf8173bbad81874772925fd32f0b159f",
		"3e49fbf950a424c5d80228dc4bc35e9f6c6c0c1d04440998da0a609a877575dbe437d6a5cedaa2ddd2a1a17fd112aded",
		"5a949594228b1a3d6f599eb3db0d06070fbc551c657b58234ba164ce3fe415fa5f3eb823c08dc29b8c341219c77b6b3d",
		"2baad447c8c290cfed25edd9031c41d0b76921457327f42db31122b81f337bbf0b1039ec830ce9061a3761953c75e4a8",
		"1220e7e6cad7b25df98e5bbdcc6c0b65ca6c2a50c5ff6c41dca71e475646fd489615979ca92fb4389aeadefde79a24f1"
	},
	{
		"788be2336c52f4454d63ee944b1e49bfb619a08371048e6da92e584eae70bde1f171c4df378bd1f3c0ab03048a237802",
		"4673ebd8db604eaf41711748bab2968a23ca4476ce144e728247f08af752929157b5830f1e26067466bdfa8b65145a33",
		"50ccc1f7076e92f4638e85f2db98e0b483e6e2204c92bdd440a6deea04e37a07c6e72791c190ad4e4e86e01efba84269",
		"756c07df0ce32c839dac9fb4733c9c28b70113a676a7057c38d223f22a3a9095a8d564653af528e04c7e1824be4a6512",
		"17c2ce6962cbd2a2e066297b39d57dd9bb4680f0191d390f70b4e461419b2972ce68ad46127fdda6c39195774ea86df3",
		"793bb9cd22a93cf468faf804a38d12b78cb12189ec679ddd2e9aa21fa9a5a0b049ab16a23574fe04c1c3c02343b91beb"
	},
	{
		"d09bb822eb99e38060954747c82bb3278cf96bbf36fece3400f4c873838a40c135eb3babb9293bd1001bf3ecdee7bf26",
		"d416db6e1b87bbb7427788a3b6c7a7ab2c165b1e366f9608df512037584f213a648d47f16ac326e19aae972f63fd76c9",
		"06f132b71f74d87bf99857e1e4350a594e5fe35533b888552ceccbc0d8923c902e36141d7691e28631b8bc9bafe5e064",
		"2a3cc6b8ff5cde926e7e3a189a1bd029c9b586351af8838f4f201cb8f4b70ef3b0da06d352c80fc26baf8f42b784459e",
		"bf9985960176da6d23c7452a2954ffcbbcb24249b43019a2a023e0b3dabd461f19ad3e775c364f3f11ad49f3099400d3",
		"012d191cf7404a523678c6fc075de8285b243720a903047708bb33e501e0dbee5bcc40d7c3ef6c6da39ea24d830da1e8"
	},
	{
		"13741262ede5861dad71063dfd204b91ea1d3b7c631df68eb949969527d79a1dc59295ef7d2bca6743e8cd77b04d1b58",
		"0baaeadc7e19d74a8a04451a135f1be1b02fe299f9dc00bfdf201e83d995c6950bcc1cb89d6f7b30bf54656b9a4da586",
		"12048ebb4331ec19a1e23f1a2c773b664ccfe90a28bfb846fc12f81dff44b7443c77647164bf1e9e67fd2c07a6766241",
		"bc18836bc7a9fdf54b5352f37d7528ab8fa8ec544a8c6180511cbfdd49cce377c39e34c031b5240dc9980503ed2f262c",
		"8086cbe338191080f0b7a16c7afc4c7b0326f9ac66f58552ef4bb9d24de3429ed5d3277ed58fcf48f2b5f61326bec6c6",
		"ad0fd3ddffe8884b9263f3c15fe1f07f2a5a22ffdc7e967085eea45f0cd959f20f18f522763e28bcc925e496a52dda98"
	},
	{
		"9e22cbc18657f516a864b37b783348b66f1aa9626cd631f4fa1bd32ad88cf11db52057c660860d39d11fbf024fabd444",
		"6b0d53c79681c28116df71e9cee74fd56c8b7f04b39f1198cc72284e98be9562e35926fb4f48a9fbecafe729309e8b6f",
		"34d61a699ca576169fcdc0cc7e44e4e1221db0fe63d16850c8104029f7d48449714b9884328cae189978754ab460b486",
		"867f81104ccd6b163a7902b670ef406042cb0cce7dcdc63d1dfc91b2c40e3cdf7595834bf9eceb79849f1636fc8462fc",
		"9d4bde8e875ec49697d258d1d59465f8431c6f5531e1c59e9f9ebe3cf164a8d9ce10a12f1979283a959bad244dd83863",
		"dc4ca392dc15e20185f2c6a8ea5ec31dfc96f56153a47394b3072b13d0015f5d4ae13beb3bed54d65848f9b8383e6c95"
	},
	{
		"2db5da5f940eaa884f4db5ec2139b0469f38e4e6fbbcc52df15c0f7cf7fcb1808c749764b6be85d2fdc5b16f58ad5dc0",
		"22e8b02dcf33e1b5a083849545f84ad5e43f77cb71546dbbac0d11bdb2ee202e9d3872e8d028c08990746c5e1dde9989",
		"dc60fa8736d702135ff16aab992bb88eac397f5972456c72ec447374d0d8ce61153831bfc86ad5a6eb5b60bfb96a862c",
		"b69beede85d0f829fec1b893ccb9c3e052ff692e13b974537bc5b0f9feaf7b22e84f03231629b24866bdb4b8cf908914",
		"66f85e2bfcaba2843285b0e14ebc07ef7dafff8b424416fee647b59897b619f20eed95a632e6a4206bf7da429c04c560",
		"d765b208112d2b9ed5ad10c4046e2e3b0dbf57c469329519e239ac28b25c7d852bf757d5de0ee271cadd021d86cfd347"
	},
	{
		"329647baa354224eb4414829c5368c82d7893b39804e08cbb2180f459befc4b347a389a70c91a23bd9d30c83be5295d3",
		"cc8f61923fad2aa8e505d6cfa126b9fabd5af9dce290b75660ef06d1caa73681d06089c33bc4246b3aa30dbcd2435b12",
		"6fa6a1c704730987aa634b0516a826aba8c6d6411d3a4c89772d7a62610256a2e2f289f5c3440b0ec1e70fa339e251ce",
		"53de1fc1328e8de14aecab29ad8a40d6b13768f86f7d298433d20fec791f86f8bc73f358098b256a298bb488de257bf4",
		"ac28944fd27f17b82946c04c66c41f0053d3692f275da55cd8739a95bd8cd3af2f96e4de959ea8344d8945375905858b",
		"d3778850aeb58804fbe9dfe6f38b9fa8e20c2ca4e0dec335aafceca0333e3f2490b53c0c1a14a831ba37c4b9d74be0f2"
	},
	{
		"29d8a36d22200a75b7aea1bb47cdfcb1b7fd66de967041434728ab5d533a060df732130600fe6f75852a871fb2938e39",
		"e19b53db528395de897a45108967715eb8cb55c3fcbf23379372c0873a058d57544b102ecce722b2ccabb1a603774fd5",
		"74ad8386c1cb2ca0fcdeb31e0869bb3f48c036afe2ef110ca302bc8b910f621c9fcc54cec32bb89ec7caa84c7b8e54a8",
		"27a3e83cfb9d5122e73129d801615857da7cc089cccc9c54ab3032a19e0a0a9f677346e37f08a0b3ed8da6e5dd691063",
		"8d60e44aa5e0fd30c918456796af37f0e41957901645e5c596c6d989f5859b03a0bd7d1f4e77936fff3c74d204e5388e",
		"81e1e71575bb4505498de097350186430a6242fa6c57b85a5f984a23371123d2d1424eefbf804258392bc723e4ef1e35"
	},
};

/* Fills out with size pseudo-random bytes. */
static void
random_bytes(
	uint8_t *out,
	size_t size)
{
	size_t index;

	/* One byte of each word. */
	for (index = 0; index < size; index++)
		out[index] = (uint8_t)(random32() >> 11);
}

/* A random source for the library: pseudo-random bytes from random_bytes. */
static int
prng_random(
	void *context,
	uint8_t *out,
	size_t size)
{
	/* The context is not used. */
	(void)context;
	random_bytes(out, size);
	return 0;
}

/* Reads hex text into exactly size bytes, left-padded with zeros; returns 0, or -1 when the value is wider. */
static int
hex_to_size(
	const char *text,
	uint8_t *out,
	size_t size)
{
	uint8_t raw[512];
	size_t length;
	size_t start;
	char padded[1100];

	/* Decodes the text (an odd length is padded with one zero digit) and drops the leading zero bytes. */
	length = hex_decode(text, raw, sizeof(raw));
	if (length == (size_t)-1) {
		if (strlen(text) + 2U > sizeof(padded))
			return -1;
		padded[0] = '0';
		strcpy(padded + 1, text);
		length = hex_decode(padded, raw, sizeof(raw));
		if (length == (size_t)-1)
			return -1;
	}
	start = 0;
	while (start < length && raw[start] == 0)
		start++;
	if (length - start > size)
		return -1;

	/* Pads on the left. */
	memset(out, 0, size);
	memcpy(out + (size - (length - start)), raw + start, length - start);
	return 0;
}

/* Adds one to the size big-endian bytes (wrapping at the top). */
static void
bytes_inc(
	uint8_t *bytes,
	size_t size)
{
	size_t index;

	/* From the low end while the byte wraps. */
	for (index = size; index > 0U; index--) {
		bytes[index - 1U] = (uint8_t)(bytes[index - 1U] + 1U);
		if (bytes[index - 1U] != 0U)
			break;
	}
}

/* Subtracts one from the size big-endian bytes (wrapping at zero). */
static void
bytes_dec(
	uint8_t *bytes,
	size_t size)
{
	size_t index;

	/* From the low end while the byte wraps. */
	for (index = size; index > 0U; index--) {
		bytes[index - 1U] = (uint8_t)(bytes[index - 1U] - 1U);
		if (bytes[index - 1U] != 0xffU)
			break;
	}
}

/* Sets out = left - right for size big-endian bytes, left not below right. */
static void
bytes_sub(
	uint8_t *out,
	const uint8_t *left,
	const uint8_t *right,
	size_t size)
{
	uint32_t borrow;
	uint32_t difference;
	size_t index;

	/* Byte by byte from the low end. */
	borrow = 0;
	for (index = size; index > 0U; index--) {
		difference = (uint32_t)left[index - 1U] - (uint32_t)right[index - 1U] - borrow;
		out[index - 1U] = (uint8_t)(difference & 0xffU);
		borrow = (difference >> 8) & 1U;
	}
}

/* Returns 1 when the size bytes are all zero. */
static int
bytes_zero(
	const uint8_t *bytes,
	size_t size)
{
	size_t index;

	/* Every byte. */
	for (index = 0; index < size; index++)
		if (bytes[index] != 0U)
			return 0;
	return 1;
}

/* Reads big-endian bytes (the curve's size) into a scalar of the curve's limbs. */
static void
scalar_from_bytes(
	uint32_t *scalar,
	const struct cryc_ecp *ec,
	const uint8_t *bytes)
{
	int error;

	/* The bytes fit: they are the curve's size. */
	memset(scalar, 0, CRYC_ECP_MAX_LIMBS * sizeof(scalar[0]));
	error = cryc_bn_decode(scalar, ec->limbs, bytes, ec->size);
	CHECK(error == 0);
}

/* Sets scalar to a random value in [1, n - 1]. */
static void
random_scalar(
	uint32_t *scalar,
	const struct cryc_ecp *ec)
{
	uint8_t bytes[CRYC_ECP_MAX_BYTES];

	/* Draws until the value is in range (nearly always the first draw). */
	do {
		random_bytes(bytes, ec->size);
		scalar_from_bytes(scalar, ec, bytes);
	} while (cryc_bn_lt(scalar, ec->order.modulus, ec->limbs) == 0U || cryc_bn_is_zero(scalar, ec->limbs) != 0U);
}

/* out = a * b mod n for plain values below n. */
static void
order_mul(
	const struct cryc_ecp *ec,
	uint32_t *out,
	const uint32_t *a,
	const uint32_t *b)
{
	uint32_t a_mont[CRYC_ECP_MAX_LIMBS];

	/* A product with one Montgomery form is a plain product. */
	memset(a_mont, 0, sizeof(a_mont));
	cryc_bn_to_mont(a_mont, a, &ec->order);
	cryc_bn_mont_mul(out, a_mont, b, &ec->order);
}

/* out = a^-1 mod n for a plain value in [1, n - 1] (Fermat: a^(n - 2)). */
static void
order_inv(
	const struct cryc_ecp *ec,
	uint32_t *out,
	const uint32_t *a)
{
	uint8_t exponent[CRYC_ECP_MAX_BYTES];
	uint32_t a_mont[CRYC_ECP_MAX_LIMBS];
	uint32_t inverse[CRYC_ECP_MAX_LIMBS];

	/* n - 2 as bytes. */
	cryc_bn_encode(ec->order.modulus, ec->limbs, exponent, ec->size);
	bytes_dec(exponent, ec->size);
	bytes_dec(exponent, ec->size);

	/* The power, out of Montgomery form. */
	memset(a_mont, 0, sizeof(a_mont));
	memset(inverse, 0, sizeof(inverse));
	cryc_bn_to_mont(a_mont, a, &ec->order);
	cryc_bn_mont_exp(inverse, a_mont, exponent, ec->size, &ec->order);
	memset(out, 0, CRYC_ECP_MAX_LIMBS * sizeof(out[0]));
	cryc_bn_from_mont(out, inverse, &ec->order);
}

/* out = (0 - a) mod n for a plain value below n. */
static void
order_neg(
	const struct cryc_ecp *ec,
	uint32_t *out,
	const uint32_t *a)
{
	uint32_t zero[CRYC_ECP_MAX_LIMBS];

	/* 0 - a. */
	memset(zero, 0, sizeof(zero));
	memset(out, 0, CRYC_ECP_MAX_LIMBS * sizeof(out[0]));
	cryc_bn_mod_sub(out, zero, a, &ec->order);
}

/* Writes the uncompressed encoding of scalar * G into out (1 + 2 * size bytes). */
static void
base_encode(
	const struct cryc_ecp *ec,
	uint8_t *out,
	const uint32_t *scalar)
{
	struct cryc_ecp_point point;
	int error;

	/* The multiple, then its encoding. */
	cryc_ecp_mul_base(ec, &point, scalar);
	error = cryc_ecp_encode(ec, &point, out, 1U + 2U * ec->size);
	CHECK(error == 0);
}

/* The candidates a scripted random source hands out, and what it saw. */
struct script {
	const uint8_t *data;
	unsigned count;
	size_t size;
	unsigned calls;
	unsigned fail_at;
	unsigned wrong_size;
	unsigned overdrawn;
};

/* A random source that hands out the scripted candidates in turn, and fails at fail_at or past the end. */
static int
script_random(
	void *context,
	uint8_t *out,
	size_t size)
{
	struct script *script;
	unsigned index;

	/* Counts the call and notes a request of the wrong size. */
	script = (struct script *)context;
	index = script->calls;
	script->calls++;
	if (size != script->size)
		script->wrong_size++;

	/* A scripted failure leaves garbage, as a broken source might. */
	if (index == script->fail_at) {
		memset(out, SENTINEL_BYTE, size);
		return EIO;
	}

	/* Drawing past the script is a failure of the module under test (it should have stopped). */
	if (index >= script->count) {
		script->overdrawn++;
		memset(out, SENTINEL_BYTE, size);
		return EIO;
	}

	/* The next candidate. */
	memcpy(out, script->data + (size_t)index * script->size, size);
	return 0;
}

/* Sets up a script over data (count candidates of the curve's size). */
static void
script_start(
	struct script *script,
	const uint8_t *data,
	unsigned count,
	size_t size)
{
	/* No failure scheduled. */
	script->data = data;
	script->count = count;
	script->size = size;
	script->calls = 0;
	script->fail_at = (unsigned)-1;
	script->wrong_size = 0;
	script->overdrawn = 0;
}

/* Runs cryc_ecc_keygen on the script with both outputs filled with the sentinel; returns its result. */
static int
keygen_run(
	const struct cryc_ecp *ec,
	struct script *script,
	uint8_t *private_key,
	uint8_t *public_key)
{
	/* The sentinel shows a byte that was not written or not wiped. */
	memset(private_key, SENTINEL_BYTE, CRYC_ECP_MAX_BYTES);
	memset(public_key, SENTINEL_BYTE, 1U + 2U * CRYC_ECP_MAX_BYTES);
	return cryc_ecc_keygen(ec, private_key, public_key, script_random, script);
}

/* Checks that cryc_ecc_keygen on the candidates gave the private key and public key, after draws draws. */
static void
keygen_expect(
	const struct cryc_ecp *ec,
	const uint8_t *candidates,
	unsigned count,
	unsigned draws,
	const uint8_t *want_private,
	const uint8_t *want_public)
{
	struct script script;
	uint8_t private_key[CRYC_ECP_MAX_BYTES];
	uint8_t public_key[1U + 2U * CRYC_ECP_MAX_BYTES];
	uint8_t decoded_check[1U + 2U * CRYC_ECP_MAX_BYTES];
	uint32_t scalar[CRYC_ECP_MAX_LIMBS];
	int error;

	/* Runs the generation. */
	script_start(&script, candidates, count, ec->size);
	error = keygen_run(ec, &script, private_key, public_key);
	CHECK(error == 0);
	CHECK(script.calls == draws);
	CHECK(script.wrong_size == 0U);
	CHECK(script.overdrawn == 0U);
	if (error != 0)
		return;

	/* The private key as scripted. */
	CHECK_BYTES(private_key, want_private, ec->size);

	/* The public key as given, or, when not given, the encoding of d * G made here. */
	if (want_public != NULL) {
		CHECK_BYTES(public_key, want_public, 1U + 2U * ec->size);
	} else {
		scalar_from_bytes(scalar, ec, private_key);
		base_encode(ec, decoded_check, scalar);
		CHECK_BYTES(public_key, decoded_check, 1U + 2U * ec->size);
	}
}

/* Checks the key generation against scripted candidates at the edges of the range. */
static void
check_keygen_script(
	const struct params *params,
	const struct cryc_ecp *ec)
{
	static uint8_t data[SCRIPT_MAX * CRYC_ECP_MAX_BYTES];
	struct script script;
	uint8_t n_minus_1[CRYC_ECP_MAX_BYTES];
	uint8_t n_minus_2[CRYC_ECP_MAX_BYTES];
	uint8_t n_minus_3[CRYC_ECP_MAX_BYTES];
	uint8_t n_plus_1[CRYC_ECP_MAX_BYTES];
	uint8_t n_bytes[CRYC_ECP_MAX_BYTES];
	uint8_t want_private[CRYC_ECP_MAX_BYTES];
	uint8_t want_public[1U + 2U * CRYC_ECP_MAX_BYTES];
	uint8_t private_key[CRYC_ECP_MAX_BYTES + 4U];
	uint8_t public_key[1U + 2U * CRYC_ECP_MAX_BYTES + 4U];
	uint8_t prime[CRYC_ECP_MAX_BYTES];
	uint8_t gx[CRYC_ECP_MAX_BYTES];
	uint8_t gy[CRYC_ECP_MAX_BYTES];
	uint8_t minus_gy[CRYC_ECP_MAX_BYTES];
	struct cryc_ecp bad;
	size_t size;
	size_t limb;
	size_t index;
	unsigned draw;
	int error;

	/* The order and its neighbours as bytes. */
	size = ec->size;
	CHECK(hex_to_size(params->n, n_bytes, size) == 0);
	memcpy(n_minus_1, n_bytes, size);
	bytes_dec(n_minus_1, size);
	memcpy(n_minus_2, n_minus_1, size);
	bytes_dec(n_minus_2, size);
	memcpy(n_minus_3, n_minus_2, size);
	bytes_dec(n_minus_3, size);
	memcpy(n_plus_1, n_bytes, size);
	bytes_inc(n_plus_1, size);
	CHECK(hex_to_size(params->gx, gx, size) == 0);
	CHECK(hex_to_size(params->gy, gy, size) == 0);
	cryc_bn_encode(ec->field.modulus, ec->limbs, prime, size);
	bytes_sub(minus_gy, prime, gy, size);

	/* c = 0 is accepted: d = 1 and Q = G. */
	memset(data, 0, size);
	memset(want_private, 0, size);
	want_private[size - 1U] = 1U;
	want_public[0] = 0x04U;
	memcpy(want_public + 1U, gx, size);
	memcpy(want_public + 1U + size, gy, size);
	keygen_expect(ec, data, 1U, 1U, want_private, want_public);

	/* c = n - 2 is the largest accepted candidate: d = n - 1 and Q = -G. */
	memcpy(data, n_minus_2, size);
	memcpy(want_private, n_minus_1, size);
	memcpy(want_public + 1U, gx, size);
	memcpy(want_public + 1U + size, minus_gy, size);
	keygen_expect(ec, data, 1U, 1U, want_private, want_public);

	/* c = n - 3: d = n - 2, with the public key checked against d * G made here. */
	memcpy(data, n_minus_3, size);
	keygen_expect(ec, data, 1U, 1U, n_minus_2, NULL);

	/* c = n - 1 is rejected, then n - 2 accepted. */
	memcpy(data, n_minus_1, size);
	memcpy(data + size, n_minus_2, size);
	keygen_expect(ec, data, 2U, 2U, n_minus_1, NULL);

	/* c = n is rejected, then 1: d = 2. */
	memcpy(data, n_bytes, size);
	memset(data + size, 0, size);
	data[2U * size - 1U] = 1U;
	memset(want_private, 0, size);
	want_private[size - 1U] = 2U;
	keygen_expect(ec, data, 2U, 2U, want_private, NULL);

	/* c = n + 1, all ones, n - 1 and n are rejected; then 5: d = 6. */
	memcpy(data, n_plus_1, size);
	memset(data + size, 0xff, size);
	memcpy(data + 2U * size, n_minus_1, size);
	memcpy(data + 3U * size, n_bytes, size);
	memset(data + 4U * size, 0, size);
	data[5U * size - 1U] = 5U;
	memset(want_private, 0, size);
	want_private[size - 1U] = 6U;
	keygen_expect(ec, data, 5U, 5U, want_private, NULL);

	/* A candidate of j all-one limbs makes d = 2^(32 j): the carry runs through every limb. */
	for (limb = 1; limb < ec->limbs; limb++) {
		memset(data, 0, size);
		for (index = 0; index < limb * 4U; index++)
			data[size - 1U - index] = 0xffU;
		memset(want_private, 0, size);
		want_private[size - 1U - limb * 4U] = 1U;
		keygen_expect(ec, data, 1U, 1U, want_private, NULL);
	}

	/* A candidate with a byte of ones in the middle: the carry crosses one byte. */
	memset(data, 0, size);
	data[size / 2U] = 0xffU;
	memset(want_private, 0, size);
	want_private[size / 2U] = 0xffU;
	want_private[size - 1U] = 1U;
	keygen_expect(ec, data, 1U, 1U, want_private, NULL);

	/* 63 rejected candidates then an accepted one: the 64th draw is still used. */
	for (draw = 0; draw < 63U; draw++)
		memcpy(data + (size_t)draw * size, n_minus_1, size);
	memset(data + 63U * size, 0, size);
	memset(want_private, 0, size);
	want_private[size - 1U] = 1U;
	keygen_expect(ec, data, 64U, 64U, want_private, NULL);

	/* 64 rejected candidates give up with EIO after exactly 64 draws, and both outputs are wiped. */
	for (draw = 0; draw < 70U; draw++)
		memcpy(data + (size_t)draw * size, n_minus_1, size);
	script_start(&script, data, 70U, size);
	memset(private_key, SENTINEL_BYTE, sizeof(private_key));
	memset(public_key, SENTINEL_BYTE, sizeof(public_key));
	error = cryc_ecc_keygen(ec, private_key, public_key, script_random, &script);
	CHECK(error == EIO);
	CHECK(script.calls == CRYC_ECC_KEYGEN_TRIES);
	CHECK(script.wrong_size == 0U);
	CHECK(script.overdrawn == 0U);
	CHECK(bytes_zero(private_key, size));
	CHECK(bytes_zero(public_key, 1U + 2U * size));
	CHECK(private_key[size] == SENTINEL_BYTE);
	CHECK(public_key[1U + 2U * size] == SENTINEL_BYTE);

	/* A source that fails at the first draw: EIO, wiped, one call. */
	memset(data, 0, size);
	script_start(&script, data, 1U, size);
	script.fail_at = 0;
	error = keygen_run(ec, &script, private_key, public_key);
	CHECK(error == EIO);
	CHECK(script.calls == 1U);
	CHECK(bytes_zero(private_key, size));
	CHECK(bytes_zero(public_key, 1U + 2U * size));

	/* A source that fails at the fourth draw, after three rejections: EIO, wiped, four calls. */
	for (draw = 0; draw < 5U; draw++)
		memcpy(data + (size_t)draw * size, n_bytes, size);
	script_start(&script, data, 5U, size);
	script.fail_at = 3;
	error = keygen_run(ec, &script, private_key, public_key);
	CHECK(error == EIO);
	CHECK(script.calls == 4U);
	CHECK(bytes_zero(private_key, size));
	CHECK(bytes_zero(public_key, 1U + 2U * size));

	/* A missing random source and a curve never prepared are refused. */
	error = cryc_ecc_keygen(ec, private_key, public_key, NULL, NULL);
	CHECK(error == EINVAL);
	memset(&bad, 0, sizeof(bad));
	script_start(&script, data, 1U, size);
	error = cryc_ecc_keygen(&bad, private_key, public_key, script_random, &script);
	CHECK(error == EINVAL);
	CHECK(script.calls == 0U);
}

/* Checks the CAVP key pairs: feeding d - 1 as the candidate must give d and the published Q. */
static void
check_keygen_cavp(
	const struct params *params,
	const struct cryc_ecp *ec)
{
	struct vec_file file;
	struct script script;
	uint8_t candidate[CRYC_ECP_MAX_BYTES];
	uint8_t want_private[CRYC_ECP_MAX_BYTES];
	uint8_t want_public[1U + 2U * CRYC_ECP_MAX_BYTES];
	uint8_t private_key[CRYC_ECP_MAX_BYTES];
	uint8_t public_key[1U + 2U * CRYC_ECP_MAX_BYTES];
	const char *section;
	const char *d_text;
	unsigned long count;
	size_t index;
	int error;

	/* Each record of this curve (the section keys are cumulative: the curve is the last curve key). */
	count = 0;
	error = vec_open(&file, "cavp-186-4ecdsatestvectors-KeyPair");
	CHECK(error == 0);
	if (error != 0)
		return;
	while (vec_next(&file)) {
		d_text = vec_get(&file, "d");
		section = NULL;
		for (index = file.count; index > 0U && section == NULL; index--) {
			const char *key = file.keys[index - 1U];

			if (strncmp(key, "section.", 8) == 0 && key[9] == '-')
				section = key + 8;
		}
		if (d_text == NULL || section == NULL || strcmp(section, params->cavp_key) != 0)
			continue;

		/* The candidate is d - 1 (d is at least 1). */
		CHECK(hex_to_size(d_text, want_private, ec->size) == 0);
		memcpy(candidate, want_private, ec->size);
		bytes_dec(candidate, ec->size);
		want_public[0] = 0x04U;
		CHECK(hex_to_size(vec_get(&file, "Qx"), want_public + 1U, ec->size) == 0);
		CHECK(hex_to_size(vec_get(&file, "Qy"), want_public + 1U + ec->size, ec->size) == 0);

		/* The generation gives d and Q. */
		script_start(&script, candidate, 1U, ec->size);
		error = keygen_run(ec, &script, private_key, public_key);
		CHECK(error == 0);
		CHECK_BYTES(private_key, want_private, ec->size);
		CHECK_BYTES(public_key, want_public, 1U + 2U * ec->size);
		count++;
	}
	vec_close(&file);
	printf("  %s CAVP key pairs: %lu\n", params->name, count);
	CHECK(count >= 10U);
}

/* Checks the CAVP ECC CDH cases: Z = x(dIUT * QCAVS), and dIUT gives QIUT. */
static void
check_ecdh_cavp(
	const struct params *params,
	const struct cryc_ecp *ec,
	const struct cavp_ecdh *table)
{
	struct script script;
	uint8_t peer[1U + 2U * CRYC_ECP_MAX_BYTES];
	uint8_t private_key[CRYC_ECP_MAX_BYTES];
	uint8_t candidate[CRYC_ECP_MAX_BYTES];
	uint8_t want_shared[CRYC_ECP_MAX_BYTES];
	uint8_t want_public[1U + 2U * CRYC_ECP_MAX_BYTES];
	uint8_t shared[CRYC_ECP_MAX_BYTES];
	uint8_t generated_private[CRYC_ECP_MAX_BYTES];
	uint8_t generated_public[1U + 2U * CRYC_ECP_MAX_BYTES];
	unsigned index;
	int error;

	/* Each case. */
	for (index = 0; index < CAVP_ECDH_COUNT; index++) {
		peer[0] = 0x04U;
		CHECK(hex_to_size(table[index].peer_x, peer + 1U, ec->size) == 0);
		CHECK(hex_to_size(table[index].peer_y, peer + 1U + ec->size, ec->size) == 0);
		CHECK(hex_to_size(table[index].private_key, private_key, ec->size) == 0);
		CHECK(hex_to_size(table[index].shared, want_shared, ec->size) == 0);
		want_public[0] = 0x04U;
		CHECK(hex_to_size(table[index].public_x, want_public + 1U, ec->size) == 0);
		CHECK(hex_to_size(table[index].public_y, want_public + 1U + ec->size, ec->size) == 0);

		/* The shared secret. */
		memset(shared, SENTINEL_BYTE, sizeof(shared));
		error = cryc_ecc_ecdh(ec, private_key, peer, 1U + 2U * ec->size, shared);
		CHECK(error == 0);
		CHECK_BYTES(shared, want_shared, ec->size);

		/* The same private key as a generated one (the candidate is d - 1) gives the published public key. */
		memcpy(candidate, private_key, ec->size);
		bytes_dec(candidate, ec->size);
		script_start(&script, candidate, 1U, ec->size);
		error = keygen_run(ec, &script, generated_private, generated_public);
		CHECK(error == 0);
		CHECK_BYTES(generated_private, private_key, ec->size);
		CHECK_BYTES(generated_public, want_public, 1U + 2U * ec->size);
	}
	printf("  %s CAVP ECDH cases: %u\n", params->name, (unsigned)CAVP_ECDH_COUNT);
}

/* Returns the leading-zero-stripped private key of a Wycheproof case in out; returns 0, or -1 when it is wider than the curve. */
static int
wide_private(
	const uint8_t *priv,
	size_t priv_size,
	uint8_t *out,
	size_t size)
{
	size_t start;

	/* Strips leading zero bytes and pads on the left. */
	start = 0;
	while (start < priv_size && priv[start] == 0U)
		start++;
	if (priv_size - start > size)
		return -1;
	memset(out, 0, size);
	memcpy(out + (size - (priv_size - start)), priv + start, priv_size - start);
	return 0;
}

/* Checks the Wycheproof ECDH cases for P-256. */
static void
check_ecdh_wycheproof(
	const struct params *params,
	const struct cryc_ecp *ec)
{
	struct vec_file file;
	uint8_t pub[256];
	uint8_t priv[256];
	uint8_t want[64];
	uint8_t private_key[CRYC_ECP_MAX_BYTES];
	uint8_t shared[CRYC_ECP_MAX_BYTES];
	const char *result;
	size_t pub_size;
	size_t priv_size;
	size_t want_size;
	uint32_t scalar[CRYC_ECP_MAX_LIMBS];
	unsigned long valid;
	unsigned long refused;
	unsigned long wide;
	int error;

	valid = 0;
	refused = 0;
	wide = 0;
	error = vec_open(&file, "wycheproof-ecdh_secp256r1_ecpoint_test");
	CHECK(error == 0);
	if (error != 0)
		return;
	while (vec_next(&file)) {
		result = vec_get(&file, "result");
		pub_size = vec_hex(&file, "public", pub, sizeof(pub));
		priv_size = vec_hex(&file, "private", priv, sizeof(priv));
		want_size = vec_hex(&file, "shared", want, sizeof(want));
		CHECK(result != NULL);
		CHECK(pub_size != (size_t)-1 && priv_size != (size_t)-1 && want_size != (size_t)-1);
		if (result == NULL || pub_size == (size_t)-1 || priv_size == (size_t)-1 || want_size == (size_t)-1)
			continue;

		/* A private key wider than the curve cannot be passed (the header takes ec->size bytes). */
		if (wide_private(priv, priv_size, private_key, ec->size) != 0) {
			wide++;
			continue;
		}
		scalar_from_bytes(scalar, ec, private_key);
		memset(shared, SENTINEL_BYTE, sizeof(shared));
		error = cryc_ecc_ecdh(ec, private_key, pub, pub_size, shared);

		/* An out-of-range private key is refused whatever the point. */
		if (cryc_bn_is_zero(scalar, ec->limbs) != 0U || cryc_bn_lt(scalar, ec->order.modulus, ec->limbs) == 0U) {
			CHECK(error != 0);
			CHECK(bytes_zero(shared, ec->size));
			refused++;
			continue;
		}

		/* An invalid public point is refused and nothing is left in the output. */
		if (strcmp(result, "invalid") == 0) {
			CHECK(error != 0);
			CHECK(bytes_zero(shared, ec->size));
			refused++;
			continue;
		}

		/* A compressed point (acceptable in the file) is a form this module does not support. */
		if (error != 0) {
			CHECK(strcmp(result, "acceptable") == 0);
			CHECK(error == ENOTSUP);
			CHECK(pub[0] == 0x02U || pub[0] == 0x03U);
			CHECK(bytes_zero(shared, ec->size));
			refused++;
			continue;
		}

		/* A valid case gives the published shared secret. */
		CHECK(want_size == ec->size);
		CHECK_BYTES(shared, want, ec->size);
		valid++;
	}
	vec_close(&file);
	printf("  %s Wycheproof ECDH: %lu computed, %lu refused, %lu wider than the curve\n", params->name, valid, refused, wide);
	CHECK(valid > 100U);
	CHECK(refused > 0U);
}

/* Checks the errors of ECDH: the private key's range, every refusal of the peer's point, and the wiping of the output. */
static void
check_ecdh_errors(
	const struct params *params,
	const struct cryc_ecp *ec)
{
	uint8_t private_key[CRYC_ECP_MAX_BYTES];
	uint8_t peer[1U + 2U * CRYC_ECP_MAX_BYTES];
	uint8_t bad_peer[1U + 2U * CRYC_ECP_MAX_BYTES + 4U];
	uint8_t shared[CRYC_ECP_MAX_BYTES + 4U];
	uint8_t prime[CRYC_ECP_MAX_BYTES];
	uint8_t n_bytes[CRYC_ECP_MAX_BYTES];
	uint8_t one[CRYC_ECP_MAX_BYTES];
	struct cryc_ecp bad;
	size_t size;
	size_t total;
	size_t length;
	int error;

	/* The base point as the peer, and a valid private key of 1. */
	size = ec->size;
	total = 1U + 2U * size;
	peer[0] = 0x04U;
	CHECK(hex_to_size(params->gx, peer + 1U, size) == 0);
	CHECK(hex_to_size(params->gy, peer + 1U + size, size) == 0);
	memset(one, 0, size);
	one[size - 1U] = 1U;
	CHECK(hex_to_size(params->n, n_bytes, size) == 0);
	cryc_bn_encode(ec->field.modulus, ec->limbs, prime, size);

	/* A private key of 0 is refused; the output is wiped (it was filled before the call). */
	memset(private_key, 0, size);
	memset(shared, SENTINEL_BYTE, sizeof(shared));
	error = cryc_ecc_ecdh(ec, private_key, peer, total, shared);
	CHECK(error == EINVAL);
	CHECK(bytes_zero(shared, size));
	CHECK(shared[size] == SENTINEL_BYTE);

	/* The order, order + 1 and all ones are refused. */
	memcpy(private_key, n_bytes, size);
	memset(shared, SENTINEL_BYTE, sizeof(shared));
	error = cryc_ecc_ecdh(ec, private_key, peer, total, shared);
	CHECK(error == EINVAL);
	CHECK(bytes_zero(shared, size));
	bytes_inc(private_key, size);
	memset(shared, SENTINEL_BYTE, sizeof(shared));
	error = cryc_ecc_ecdh(ec, private_key, peer, total, shared);
	CHECK(error == EINVAL);
	CHECK(bytes_zero(shared, size));
	memset(private_key, 0xff, size);
	memset(shared, SENTINEL_BYTE, sizeof(shared));
	error = cryc_ecc_ecdh(ec, private_key, peer, total, shared);
	CHECK(error == EINVAL);
	CHECK(bytes_zero(shared, size));

	/* n - 1 and 1 are accepted, and both give x(G) (x(-G) = x(G)). */
	memcpy(private_key, n_bytes, size);
	bytes_dec(private_key, size);
	memset(shared, SENTINEL_BYTE, sizeof(shared));
	error = cryc_ecc_ecdh(ec, private_key, peer, total, shared);
	CHECK(error == 0);
	CHECK_BYTES(shared, peer + 1U, size);
	CHECK(shared[size] == SENTINEL_BYTE);
	memset(shared, SENTINEL_BYTE, sizeof(shared));
	error = cryc_ecc_ecdh(ec, one, peer, total, shared);
	CHECK(error == 0);
	CHECK_BYTES(shared, peer + 1U, size);

	/* The peer's point: a length of 0 is EINVAL. */
	memset(shared, SENTINEL_BYTE, sizeof(shared));
	error = cryc_ecc_ecdh(ec, one, peer, 0U, shared);
	CHECK(error == EINVAL);
	CHECK(bytes_zero(shared, size));

	/* Compressed forms (0x02, 0x03) of any length are ENOTSUP, and the output is wiped. */
	for (length = 1; length <= total + 1U; length++) {
		memcpy(bad_peer, peer, total);
		bad_peer[total] = 0;
		bad_peer[0] = 0x02U;
		memset(shared, SENTINEL_BYTE, sizeof(shared));
		error = cryc_ecc_ecdh(ec, one, bad_peer, length, shared);
		CHECK(error == ENOTSUP);
		CHECK(bytes_zero(shared, size));
		bad_peer[0] = 0x03U;
		memset(shared, SENTINEL_BYTE, sizeof(shared));
		error = cryc_ecc_ecdh(ec, one, bad_peer, length, shared);
		CHECK(error == ENOTSUP);
		CHECK(bytes_zero(shared, size));
	}

	/* Another first byte (including the point at infinity, 0x00) is EINVAL. */
	memcpy(bad_peer, peer, total);
	bad_peer[0] = 0x00U;
	memset(shared, SENTINEL_BYTE, sizeof(shared));
	error = cryc_ecc_ecdh(ec, one, bad_peer, total, shared);
	CHECK(error == EINVAL);
	CHECK(bytes_zero(shared, size));
	bad_peer[0] = 0x05U;
	memset(shared, SENTINEL_BYTE, sizeof(shared));
	error = cryc_ecc_ecdh(ec, one, bad_peer, total, shared);
	CHECK(error == EINVAL);
	CHECK(bytes_zero(shared, size));
	memset(bad_peer, 0, total);
	memset(shared, SENTINEL_BYTE, sizeof(shared));
	error = cryc_ecc_ecdh(ec, one, bad_peer, 1U, shared);
	CHECK(error == EINVAL);

	/* A wrong length of the uncompressed form is EINVAL. */
	memcpy(bad_peer, peer, total);
	bad_peer[total] = 0;
	bad_peer[total + 1U] = 0;
	memset(shared, SENTINEL_BYTE, sizeof(shared));
	error = cryc_ecc_ecdh(ec, one, bad_peer, total - 1U, shared);
	CHECK(error == EINVAL);
	CHECK(bytes_zero(shared, size));
	error = cryc_ecc_ecdh(ec, one, bad_peer, total + 1U, shared);
	CHECK(error == EINVAL);
	CHECK(bytes_zero(shared, size));

	/* A point off the curve (y changed) is EINVAL. */
	memcpy(bad_peer, peer, total);
	bad_peer[total - 1U] ^= 1U;
	memset(shared, SENTINEL_BYTE, sizeof(shared));
	error = cryc_ecc_ecdh(ec, one, bad_peer, total, shared);
	CHECK(error == EINVAL);
	CHECK(bytes_zero(shared, size));

	/* A coordinate equal to p is EINVAL (out of range). */
	memcpy(bad_peer, peer, total);
	memcpy(bad_peer + 1U, prime, size);
	memset(shared, SENTINEL_BYTE, sizeof(shared));
	error = cryc_ecc_ecdh(ec, one, bad_peer, total, shared);
	CHECK(error == EINVAL);
	CHECK(bytes_zero(shared, size));

	/* A curve never prepared is refused. */
	memset(&bad, 0, sizeof(bad));
	error = cryc_ecc_ecdh(&bad, one, peer, total, shared);
	CHECK(error == EINVAL);
}

/* Checks ECDH on generated keys: both sides agree, x(d Q) = x((n - d) Q), and x(d G) is the public key's x. */
static void
check_ecdh_properties(
	const struct params *params,
	const struct cryc_ecp *ec,
	unsigned pairs)
{
	uint8_t private_a[CRYC_ECP_MAX_BYTES];
	uint8_t private_b[CRYC_ECP_MAX_BYTES];
	uint8_t negated[CRYC_ECP_MAX_BYTES];
	uint8_t public_a[1U + 2U * CRYC_ECP_MAX_BYTES];
	uint8_t public_b[1U + 2U * CRYC_ECP_MAX_BYTES];
	uint8_t base[1U + 2U * CRYC_ECP_MAX_BYTES];
	uint8_t shared_a[CRYC_ECP_MAX_BYTES];
	uint8_t shared_b[CRYC_ECP_MAX_BYTES];
	uint8_t shared_c[CRYC_ECP_MAX_BYTES];
	uint32_t scalar[CRYC_ECP_MAX_LIMBS];
	uint32_t other[CRYC_ECP_MAX_LIMBS];
	unsigned pair;
	size_t total;
	int error;

	/* The base point as a peer. */
	total = 1U + 2U * ec->size;
	base[0] = 0x04U;
	CHECK(hex_to_size(params->gx, base + 1U, ec->size) == 0);
	CHECK(hex_to_size(params->gy, base + 1U + ec->size, ec->size) == 0);

	/* Each pair of generated keys. */
	for (pair = 0; pair < pairs; pair++) {
		error = cryc_ecc_keygen(ec, private_a, public_a, prng_random, NULL);
		CHECK(error == 0);
		error = cryc_ecc_keygen(ec, private_b, public_b, prng_random, NULL);
		CHECK(error == 0);

		/* The private keys are in [1, n - 1] and the public keys start with 0x04. */
		scalar_from_bytes(scalar, ec, private_a);
		CHECK(cryc_bn_lt(scalar, ec->order.modulus, ec->limbs) != 0U);
		CHECK(cryc_bn_is_zero(scalar, ec->limbs) == 0U);
		CHECK(public_a[0] == 0x04U);
		CHECK(public_b[0] == 0x04U);

		/* Both sides compute the same secret. */
		error = cryc_ecc_ecdh(ec, private_a, public_b, total, shared_a);
		CHECK(error == 0);
		error = cryc_ecc_ecdh(ec, private_b, public_a, total, shared_b);
		CHECK(error == 0);
		CHECK_BYTES(shared_a, shared_b, ec->size);

		/* x(d Q) = x((n - d) Q): -d gives the opposite point with the same x. */
		scalar_from_bytes(other, ec, private_a);
		order_neg(ec, scalar, other);
		cryc_bn_encode(scalar, ec->limbs, negated, ec->size);
		error = cryc_ecc_ecdh(ec, negated, public_b, total, shared_c);
		CHECK(error == 0);
		CHECK_BYTES(shared_c, shared_a, ec->size);

		/* x(d G) is the x of the public key made from d. */
		error = cryc_ecc_ecdh(ec, private_a, base, total, shared_c);
		CHECK(error == 0);
		CHECK_BYTES(shared_c, public_a + 1U, ec->size);
	}
}

/* Looks for a private key whose public point has a zero first x byte, and checks ECDH gives all size bytes. */
static void
check_ecdh_leading_zero(
	const struct params *params,
	const struct cryc_ecp *ec)
{
	uint8_t private_key[CRYC_ECP_MAX_BYTES];
	uint8_t base[1U + 2U * CRYC_ECP_MAX_BYTES];
	uint8_t point[1U + 2U * CRYC_ECP_MAX_BYTES];
	uint8_t shared[CRYC_ECP_MAX_BYTES];
	uint32_t scalar[CRYC_ECP_MAX_LIMBS];
	unsigned tries;
	int error;
	int found;

	/* The base point as the peer. */
	base[0] = 0x04U;
	CHECK(hex_to_size(params->gx, base + 1U, ec->size) == 0);
	CHECK(hex_to_size(params->gy, base + 1U + ec->size, ec->size) == 0);

	/* Searches d = 1, 2, 3, ... for a multiple of G whose x starts with a zero byte. */
	found = 0;
	for (tries = 1; tries <= ZERO_BYTE_TRIES && !found; tries++) {
		memset(scalar, 0, sizeof(scalar));
		scalar[0] = tries;
		base_encode(ec, point, scalar);
		if (point[1] != 0U)
			continue;

		/* The shared secret has all ec->size bytes, the first of them zero. */
		memset(private_key, 0, ec->size);
		cryc_bn_encode(scalar, ec->limbs, private_key, ec->size);
		memset(shared, SENTINEL_BYTE, sizeof(shared));
		error = cryc_ecc_ecdh(ec, private_key, base, 1U + 2U * ec->size, shared);
		CHECK(error == 0);
		CHECK_BYTES(shared, point + 1U, ec->size);
		CHECK(shared[0] == 0U);
		found = 1;
	}
	printf("  %s zero first byte found at d = %u\n", params->name, tries - 1U);
	CHECK(found);
}

/* Hashes size bytes of msg with the hash id into out; returns the digest size. */
static size_t
digest_message(
	enum cryc_hash_id id,
	const uint8_t *msg,
	size_t size,
	uint8_t *out)
{
	int error;

	/* The module's own hash: the vectors give the message, and the digest is only an input. */
	error = cryc_hash_digest(id, msg, size, out);
	CHECK(error == 0);
	return cryc_hash_size(id);
}

/*
 * Signs hash with the private scalar d and the nonce scalar k (FIPS 186-5
 * 6.4.1, written from the standard): R = k G, r = x(R) mod n, s = k^-1
 * (E + r d) mod n, with E the leftmost bits(n) bits of the hash reduced
 * modulo n.  Writes r and s as ec->size bytes; returns 0, or -1 when r or s
 * is zero (the caller draws another nonce).
 */
static int
sign(
	const struct cryc_ecp *ec,
	const uint32_t *d,
	const uint32_t *k,
	const uint8_t *hash,
	size_t hash_size,
	uint8_t *r_out,
	uint8_t *s_out)
{
	struct cryc_ecp_point point;
	uint8_t x_bytes[CRYC_ECP_MAX_BYTES];
	uint32_t e[CRYC_ECP_MAX_LIMBS];
	uint32_t r[CRYC_ECP_MAX_LIMBS];
	uint32_t s[CRYC_ECP_MAX_LIMBS];
	uint32_t rd[CRYC_ECP_MAX_LIMBS];
	uint32_t sum[CRYC_ECP_MAX_LIMBS];
	uint32_t k_inverse[CRYC_ECP_MAX_LIMBS];
	size_t used;
	int error;

	/* E: the leftmost bits(n) bits as an integer, reduced modulo n (it is below 2^bits < 2 n). */
	memset(e, 0, sizeof(e));
	used = hash_size < ec->size ? hash_size : ec->size;
	error = cryc_bn_decode(e, ec->limbs, hash, used);
	CHECK(error == 0);
	if (cryc_bn_lt(e, ec->order.modulus, ec->limbs) == 0U)
		cryc_bn_mod_sub(e, e, ec->order.modulus, &ec->order);

	/* r = x(k G) mod n. */
	cryc_ecp_mul_base(ec, &point, k);
	error = cryc_ecp_affine_x(ec, &point, x_bytes);
	CHECK(error == 0);
	memset(r, 0, sizeof(r));
	error = cryc_bn_decode(r, ec->limbs, x_bytes, ec->size);
	CHECK(error == 0);
	if (cryc_bn_lt(r, ec->order.modulus, ec->limbs) == 0U)
		cryc_bn_mod_sub(r, r, ec->order.modulus, &ec->order);
	if (cryc_bn_is_zero(r, ec->limbs) != 0U)
		return -1;

	/* s = k^-1 (E + r d) mod n. */
	memset(rd, 0, sizeof(rd));
	memset(sum, 0, sizeof(sum));
	memset(s, 0, sizeof(s));
	order_mul(ec, rd, r, d);
	cryc_bn_mod_add(sum, e, rd, &ec->order);
	order_inv(ec, k_inverse, k);
	order_mul(ec, s, k_inverse, sum);
	if (cryc_bn_is_zero(s, ec->limbs) != 0U)
		return -1;

	/* The two as bytes. */
	cryc_bn_encode(r, ec->limbs, r_out, ec->size);
	cryc_bn_encode(s, ec->limbs, s_out, ec->size);
	return 0;
}

/* Makes a key pair and a signature of hash with it (drawing nonces until r and s are non-zero). */
static void
make_signature(
	const struct cryc_ecp *ec,
	uint32_t *d,
	uint8_t *public_key,
	const uint8_t *hash,
	size_t hash_size,
	uint8_t *r,
	uint8_t *s)
{
	uint32_t k[CRYC_ECP_MAX_LIMBS];
	int result;

	/* The key. */
	random_scalar(d, ec);
	base_encode(ec, public_key, d);

	/* The signature. */
	do {
		random_scalar(k, ec);
		result = sign(ec, d, k, hash, hash_size, r, s);
	} while (result != 0);
}

/*
 * Decodes a DER Ecdsa-Sig-Value (SEQUENCE of two INTEGERs), strictly:
 * short-form lengths only, no padding, non-negative integers with no
 * redundant leading zero.  Writes r and s as size bytes.  Returns 0; or 1
 * when the encoding is well formed but an integer is wider than the curve
 * (r and s are then not set); or -1 when it is malformed.
 */
static int
der_signature(
	const uint8_t *sig,
	size_t length,
	size_t size,
	uint8_t *r,
	uint8_t *s)
{
	const uint8_t *cursor;
	const uint8_t *end;
	const uint8_t *body;
	uint8_t *out;
	size_t item_length;
	int which;
	int wide;

	/* The sequence header: a short-form length that covers everything. */
	if (length < 2U || sig[0] != 0x30U)
		return -1;
	if (sig[1] >= 0x80U)
		return -1;
	if ((size_t)sig[1] + 2U != length)
		return -1;

	/* The two integers. */
	cursor = sig + 2;
	end = sig + length;
	wide = 0;
	for (which = 0; which < 2; which++) {
		if ((size_t)(end - cursor) < 2U || cursor[0] != 0x02U)
			return -1;
		item_length = cursor[1];
		if (item_length >= 0x80U || item_length == 0U || (size_t)(end - cursor) < 2U + item_length)
			return -1;
		body = cursor + 2;
		if ((body[0] & 0x80U) != 0U)
			return -1;
		if (item_length > 1U && body[0] == 0U && (body[1] & 0x80U) == 0U)
			return -1;
		cursor = body + item_length;

		/* Leading zero bytes stripped; wider than the curve is flagged. */
		while (item_length > 0U && body[0] == 0U) {
			body++;
			item_length--;
		}
		if (item_length > size) {
			wide = 1;
			continue;
		}
		out = which == 0 ? r : s;
		memset(out, 0, size);
		memcpy(out + (size - item_length), body, item_length);
	}

	/* Nothing after the second integer. */
	if (cursor != end)
		return -1;
	return wide;
}

/* Returns the last section key of the record that names a curve ("P-256,SHA-256" for SigVer, "P-256" for KeyPair), or NULL. */
static const char *
record_section(
	const struct vec_file *file)
{
	size_t index;
	const char *key;

	/* From the last key backwards: the first section key that starts with a curve name (not the key-generation method's). */
	for (index = file->count; index > 0U; index--) {
		key = file->keys[index - 1U];
		if (strncmp(key, "section.", 8) == 0 && key[9] == '-')
			return key + 8;
	}
	return NULL;
}

/* Maps a hash's name as the vectors write it to an id; CRYC_HASH_NONE for one this module lacks. */
static enum cryc_hash_id
hash_from_name(
	const char *name)
{
	/* The three hashes of the module. */
	if (strcmp(name, "SHA-256") == 0)
		return CRYC_HASH_SHA256;
	if (strcmp(name, "SHA-384") == 0)
		return CRYC_HASH_SHA384;
	if (strcmp(name, "SHA-512") == 0)
		return CRYC_HASH_SHA512;
	return CRYC_HASH_NONE;
}

/* Checks the CAVP SigVer cases of the curve with SHA-256, SHA-384 and SHA-512. */
static void
check_sigver_cavp(
	const struct params *params,
	const struct cryc_ecp *ec)
{
	static uint8_t msg[8192];
	struct vec_file file;
	struct cryc_ecp_point point;
	uint8_t public_key[1U + 2U * CRYC_ECP_MAX_BYTES];
	uint8_t digest[HASH_MAX];
	uint8_t r[CRYC_ECP_MAX_BYTES];
	uint8_t s[CRYC_ECP_MAX_BYTES];
	const char *section;
	const char *result;
	size_t msg_size;
	size_t digest_size;
	size_t name_size;
	enum cryc_hash_id hash;
	unsigned long good;
	unsigned long bad;
	int key_error;
	int error;

	good = 0;
	bad = 0;
	name_size = strlen(params->cavp_key);
	error = vec_open(&file, "cavp-186-4ecdsatestvectors-SigVer");
	CHECK(error == 0);
	if (error != 0)
		return;
	while (vec_next(&file)) {
		/* This curve with a hash the module has. */
		section = record_section(&file);
		if (section == NULL || strncmp(section, params->cavp_key, name_size) != 0 || section[name_size] != ',')
			continue;
		hash = hash_from_name(section + name_size + 1U);
		if (hash == CRYC_HASH_NONE)
			continue;

		/* The fields. */
		msg_size = vec_hex(&file, "Msg", msg, sizeof(msg));
		result = vec_get(&file, "Result");
		CHECK(msg_size != (size_t)-1);
		CHECK(result != NULL);
		if (msg_size == (size_t)-1 || result == NULL)
			continue;
		public_key[0] = 0x04U;
		CHECK(hex_to_size(vec_get(&file, "Qx"), public_key + 1U, ec->size) == 0);
		CHECK(hex_to_size(vec_get(&file, "Qy"), public_key + 1U + ec->size, ec->size) == 0);
		CHECK(hex_to_size(vec_get(&file, "R"), r, ec->size) == 0);
		CHECK(hex_to_size(vec_get(&file, "S"), s, ec->size) == 0);
		digest_size = digest_message(hash, msg, msg_size, digest);

		/* A good signature verifies; a bad one gives EBADMSG, or the refusal of the key when the key is not a point of the curve. */
		error = cryc_ecdsa_verify(ec, public_key, 1U + 2U * ec->size, digest, digest_size, r, s);
		if (result[0] == 'P') {
			CHECK(error == 0);
			good++;
		} else {
			CHECK(result[0] == 'F');
			key_error = cryc_ecp_decode(ec, &point, public_key, 1U + 2U * ec->size);
			CHECK(error == (key_error != 0 ? key_error : EBADMSG));
			bad++;
		}
	}
	vec_close(&file);
	printf("  %s CAVP SigVer: %lu good, %lu bad\n", params->name, good, bad);
	CHECK(good >= 9U);
	CHECK(bad >= 30U);
}

/* Checks one Wycheproof ECDSA file. */
static void
check_wycheproof_ecdsa(
	const struct cryc_ecp *ec,
	const char *name)
{
	static uint8_t msg[4096];
	static uint8_t sig[4096];
	struct vec_file file;
	uint8_t public_key[256];
	uint8_t digest[HASH_MAX];
	uint8_t r[CRYC_ECP_MAX_BYTES];
	uint8_t s[CRYC_ECP_MAX_BYTES];
	const char *result;
	const char *sha;
	size_t msg_size;
	size_t sig_size;
	size_t public_size;
	size_t group_size;
	size_t digest_size;
	enum cryc_hash_id hash;
	unsigned long good;
	unsigned long bad;
	unsigned long malformed;
	unsigned long wide;
	int parsed;
	int error;

	good = 0;
	bad = 0;
	malformed = 0;
	wide = 0;
	group_size = 0;
	error = vec_open(&file, name);
	CHECK(error == 0);
	if (error != 0)
		return;
	while (vec_next(&file)) {
		/*
		 * A group's public key is in a record of its own (its PEM text
		 * ends with a blank line, which ends the record), before the
		 * records of the group's tests.
		 */
		if (vec_get(&file, "group.publicKey.uncompressed") != NULL) {
			group_size = vec_hex(&file, "group.publicKey.uncompressed", public_key, sizeof(public_key));
			CHECK(group_size != (size_t)-1);
			continue;
		}
		result = vec_get(&file, "result");
		sha = vec_get(&file, "group.sha");
		msg_size = vec_hex(&file, "msg", msg, sizeof(msg));
		sig_size = vec_hex(&file, "sig", sig, sizeof(sig));
		public_size = group_size;
		CHECK(result != NULL);
		CHECK(sha != NULL);
		CHECK(msg_size != (size_t)-1);
		CHECK(public_size != (size_t)-1 && public_size != 0U);
		if (result == NULL || sha == NULL || msg_size == (size_t)-1 || public_size == (size_t)-1 || public_size == 0U)
			continue;
		hash = hash_from_name(sha);
		CHECK(hash != CRYC_HASH_NONE);
		if (hash == CRYC_HASH_NONE)
			continue;
		digest_size = digest_message(hash, msg, msg_size, digest);

		/* A signature that is not strict DER (or too long to hold here) is invalid by the file's own verdict. */
		if (sig_size == (size_t)-1) {
			CHECK(strcmp(result, "invalid") == 0);
			malformed++;
			continue;
		}
		parsed = der_signature(sig, sig_size, ec->size, r, s);
		if (parsed < 0) {
			CHECK(strcmp(result, "invalid") == 0);
			malformed++;
			continue;
		}
		if (parsed > 0) {
			CHECK(strcmp(result, "invalid") == 0);
			wide++;
			continue;
		}

		/* A well-formed signature verifies when the file says valid, and not otherwise. */
		error = cryc_ecdsa_verify(ec, public_key, public_size, digest, digest_size, r, s);
		if (strcmp(result, "valid") == 0) {
			CHECK(error == 0);
			good++;
		} else {
			CHECK(strcmp(result, "invalid") == 0);
			CHECK(error == EBADMSG);
			bad++;
		}
	}
	vec_close(&file);
	printf("  %s: %lu valid, %lu invalid verified, %lu malformed DER, %lu wider than the curve\n", name, good, bad, malformed, wide);
	CHECK(good > 100U);
	CHECK(bad > 50U);
}

/* Checks signatures made here: round trips, each field changed, the hash cut and padded. */
static void
check_roundtrip(
	const struct cryc_ecp *ec,
	unsigned iterations)
{
	static const size_t hash_sizes[6] = { 1U, 20U, 32U, 48U, 64U, 100U };
	uint8_t hash[128];
	uint8_t altered[128];
	uint8_t padded[CRYC_ECP_MAX_BYTES];
	uint8_t public_key[1U + 2U * CRYC_ECP_MAX_BYTES];
	uint8_t bad_key[1U + 2U * CRYC_ECP_MAX_BYTES];
	uint8_t r[CRYC_ECP_MAX_BYTES];
	uint8_t s[CRYC_ECP_MAX_BYTES];
	uint8_t other[CRYC_ECP_MAX_BYTES];
	uint8_t n_bytes[CRYC_ECP_MAX_BYTES];
	uint32_t d[CRYC_ECP_MAX_LIMBS];
	size_t hash_size;
	size_t used;
	size_t index;
	size_t total;
	unsigned iteration;
	int error;

	total = 1U + 2U * ec->size;
	cryc_bn_encode(ec->order.modulus, ec->limbs, n_bytes, ec->size);
	for (iteration = 0; iteration < iterations; iteration++) {
		/* A signature over a hash of one of several sizes (a short hash must keep its first byte non-zero for the padding checks). */
		hash_size = hash_sizes[iteration % 6U];
		random_bytes(hash, sizeof(hash));
		hash[0] |= 0x80U;
		make_signature(ec, d, public_key, hash, hash_size, r, s);
		error = cryc_ecdsa_verify(ec, public_key, total, hash, hash_size, r, s);
		CHECK(error == 0);
		used = hash_size < ec->size ? hash_size : ec->size;

		/* The same (r, n - s) also verifies: ECDSA does not require a low s. */
		bytes_sub(other, n_bytes, s, ec->size);
		error = cryc_ecdsa_verify(ec, public_key, total, hash, hash_size, r, other);
		CHECK(error == 0);

		/* Each used byte of the hash matters: a changed first, middle and last byte fail. */
		for (index = 0; index < 3U; index++) {
			memcpy(altered, hash, hash_size);
			altered[index == 0U ? 0U : (index == 1U ? used / 2U : used - 1U)] ^= 0x01U;
			error = cryc_ecdsa_verify(ec, public_key, total, altered, hash_size, r, s);
			CHECK(error == EBADMSG);
		}

		/* A hash longer than the curve is cut to its leftmost bytes: a change beyond them does not matter. */
		if (hash_size > ec->size) {
			memcpy(altered, hash, hash_size);
			altered[ec->size] ^= 0x55U;
			altered[hash_size - 1U] ^= 0x01U;
			error = cryc_ecdsa_verify(ec, public_key, total, altered, hash_size, r, s);
			CHECK(error == 0);
		}

		/* A hash shorter than the curve is the shorter integer: zeros on the left are the same, zeros on the right are not. */
		if (hash_size < ec->size) {
			memset(padded, 0, sizeof(padded));
			memcpy(padded + (ec->size - hash_size), hash, hash_size);
			error = cryc_ecdsa_verify(ec, public_key, total, padded, ec->size, r, s);
			CHECK(error == 0);
			memset(padded, 0, sizeof(padded));
			memcpy(padded, hash, hash_size);
			error = cryc_ecdsa_verify(ec, public_key, total, padded, ec->size, r, s);
			CHECK(error == EBADMSG);
		}

		/* The remaining changes are made on the first iterations only (each is a verification). */
		if (iteration >= 2U)
			continue;

		/* A bit changed in each of several bytes of r and of s. */
		for (index = 0; index < 3U; index++) {
			memcpy(other, r, ec->size);
			other[index == 0U ? 0U : (index == 1U ? ec->size / 2U : ec->size - 1U)] ^= 0x01U;
			error = cryc_ecdsa_verify(ec, public_key, total, hash, hash_size, other, s);
			CHECK(error == EBADMSG);
			memcpy(other, s, ec->size);
			other[index == 0U ? 0U : (index == 1U ? ec->size / 2U : ec->size - 1U)] ^= 0x01U;
			error = cryc_ecdsa_verify(ec, public_key, total, hash, hash_size, r, other);
			CHECK(error == EBADMSG);
		}

		/* r and s swapped. */
		error = cryc_ecdsa_verify(ec, public_key, total, hash, hash_size, s, r);
		CHECK(error == EBADMSG);

		/* The key changed: a bit of x or of y leaves the curve (EINVAL). */
		memcpy(bad_key, public_key, total);
		bad_key[1U + ec->size / 2U] ^= 0x01U;
		error = cryc_ecdsa_verify(ec, bad_key, total, hash, hash_size, r, s);
		CHECK(error == EINVAL);
		memcpy(bad_key, public_key, total);
		bad_key[total - 1U] ^= 0x01U;
		error = cryc_ecdsa_verify(ec, bad_key, total, hash, hash_size, r, s);
		CHECK(error == EINVAL);

		/* Another valid key fails the signature (the generator of another key pair). */
		random_scalar(d, ec);
		base_encode(ec, bad_key, d);
		error = cryc_ecdsa_verify(ec, bad_key, total, hash, hash_size, r, s);
		CHECK(error == EBADMSG);
	}
}

/* Checks r and s out of range, and every refusal of the key and of the hash size. */
static void
check_verify_errors(
	const struct params *params,
	const struct cryc_ecp *ec)
{
	uint8_t hash[HASH_MAX];
	uint8_t public_key[1U + 2U * CRYC_ECP_MAX_BYTES + 2U];
	uint8_t bad_key[1U + 2U * CRYC_ECP_MAX_BYTES + 2U];
	uint8_t r[CRYC_ECP_MAX_BYTES];
	uint8_t s[CRYC_ECP_MAX_BYTES];
	uint8_t n_bytes[CRYC_ECP_MAX_BYTES];
	uint8_t prime[CRYC_ECP_MAX_BYTES];
	uint8_t edge[CRYC_ECP_MAX_BYTES];
	uint8_t good_r[CRYC_ECP_MAX_BYTES];
	uint8_t good_s[CRYC_ECP_MAX_BYTES];
	struct cryc_ecp bad;
	uint32_t d[CRYC_ECP_MAX_LIMBS];
	size_t size;
	size_t total;
	size_t length;
	unsigned which;
	int error;

	/* A valid signature to alter. */
	size = ec->size;
	total = 1U + 2U * size;
	CHECK(hex_to_size(params->n, n_bytes, size) == 0);
	cryc_bn_encode(ec->field.modulus, ec->limbs, prime, size);
	random_bytes(hash, sizeof(hash));
	make_signature(ec, d, public_key, hash, 32U, good_r, good_s);
	error = cryc_ecdsa_verify(ec, public_key, total, hash, 32U, good_r, good_s);
	CHECK(error == 0);

	/* r (which = 0) and s (which = 1) at 0, n - 1 (in range, wrong), n, n + 1 and all ones. */
	for (which = 0; which < 2U; which++) {
		memcpy(r, good_r, size);
		memcpy(s, good_s, size);

		/* Zero is out of range. */
		memset(edge, 0, size);
		memcpy(which == 0U ? r : s, edge, size);
		error = cryc_ecdsa_verify(ec, public_key, total, hash, 32U, r, s);
		CHECK(error == EBADMSG);

		/* n - 1 is in range but the signature is wrong. */
		memcpy(edge, n_bytes, size);
		bytes_dec(edge, size);
		memcpy(which == 0U ? r : s, edge, size);
		error = cryc_ecdsa_verify(ec, public_key, total, hash, 32U, r, s);
		CHECK(error == EBADMSG);

		/* n, n + 1 and all ones are out of range. */
		memcpy(edge, n_bytes, size);
		memcpy(which == 0U ? r : s, edge, size);
		error = cryc_ecdsa_verify(ec, public_key, total, hash, 32U, r, s);
		CHECK(error == EBADMSG);
		bytes_inc(edge, size);
		memcpy(which == 0U ? r : s, edge, size);
		error = cryc_ecdsa_verify(ec, public_key, total, hash, 32U, r, s);
		CHECK(error == EBADMSG);
		memset(edge, 0xff, size);
		memcpy(which == 0U ? r : s, edge, size);
		error = cryc_ecdsa_verify(ec, public_key, total, hash, 32U, r, s);
		CHECK(error == EBADMSG);
	}

	/* A hash of no bytes is EINVAL. */
	error = cryc_ecdsa_verify(ec, public_key, total, hash, 0U, good_r, good_s);
	CHECK(error == EINVAL);

	/* A key of no bytes is EINVAL. */
	error = cryc_ecdsa_verify(ec, public_key, 0U, hash, 32U, good_r, good_s);
	CHECK(error == EINVAL);

	/* A compressed key, of any length, is ENOTSUP (the key is checked before the signature). */
	for (length = 1; length <= total + 1U; length++) {
		memcpy(bad_key, public_key, total);
		bad_key[total] = 0;
		bad_key[0] = 0x02U;
		error = cryc_ecdsa_verify(ec, bad_key, length, hash, 32U, good_r, good_s);
		CHECK(error == ENOTSUP);
		bad_key[0] = 0x03U;
		error = cryc_ecdsa_verify(ec, bad_key, length, hash, 32U, good_r, good_s);
		CHECK(error == ENOTSUP);
	}

	/* Another first byte, the point at infinity included, is EINVAL. */
	memcpy(bad_key, public_key, total);
	bad_key[0] = 0x00U;
	error = cryc_ecdsa_verify(ec, bad_key, total, hash, 32U, good_r, good_s);
	CHECK(error == EINVAL);
	bad_key[0] = 0x06U;
	error = cryc_ecdsa_verify(ec, bad_key, total, hash, 32U, good_r, good_s);
	CHECK(error == EINVAL);

	/* A wrong length of the uncompressed form is EINVAL. */
	memcpy(bad_key, public_key, total);
	bad_key[total] = 0;
	error = cryc_ecdsa_verify(ec, bad_key, total - 1U, hash, 32U, good_r, good_s);
	CHECK(error == EINVAL);
	error = cryc_ecdsa_verify(ec, bad_key, total + 1U, hash, 32U, good_r, good_s);
	CHECK(error == EINVAL);

	/* A point off the curve and a coordinate equal to p are EINVAL. */
	memcpy(bad_key, public_key, total);
	bad_key[total - 1U] ^= 1U;
	error = cryc_ecdsa_verify(ec, bad_key, total, hash, 32U, good_r, good_s);
	CHECK(error == EINVAL);
	memcpy(bad_key, public_key, total);
	memcpy(bad_key + 1U + size, prime, size);
	error = cryc_ecdsa_verify(ec, bad_key, total, hash, 32U, good_r, good_s);
	CHECK(error == EINVAL);

	/* A bad key beats a bad signature (the key error is reported). */
	memset(r, 0, size);
	error = cryc_ecdsa_verify(ec, bad_key, total, hash, 32U, r, r);
	CHECK(error == EINVAL);

	/* A curve never prepared is refused. */
	memset(&bad, 0, sizeof(bad));
	error = cryc_ecdsa_verify(&bad, public_key, total, hash, 32U, good_r, good_s);
	CHECK(error == EINVAL);
}

/* Checks E at or above n, E = 0, and a verification whose result R is at infinity. */
static void
check_hash_edges(
	const struct params *params,
	const struct cryc_ecp *ec)
{
	uint8_t public_key[1U + 2U * CRYC_ECP_MAX_BYTES];
	uint8_t n_bytes[CRYC_ECP_MAX_BYTES];
	uint8_t n_plus_1[CRYC_ECP_MAX_BYTES];
	uint8_t not_n[CRYC_ECP_MAX_BYTES];
	uint8_t ones[CRYC_ECP_MAX_BYTES];
	uint8_t zeros[CRYC_ECP_MAX_BYTES];
	uint8_t one[CRYC_ECP_MAX_BYTES];
	uint8_t two[CRYC_ECP_MAX_BYTES];
	uint8_t r[CRYC_ECP_MAX_BYTES];
	uint8_t s[CRYC_ECP_MAX_BYTES];
	uint8_t e_bytes[CRYC_ECP_MAX_BYTES];
	uint32_t d[CRYC_ECP_MAX_LIMBS];
	uint32_t k[CRYC_ECP_MAX_LIMBS];
	uint32_t r_value[CRYC_ECP_MAX_LIMBS];
	uint32_t s_value[CRYC_ECP_MAX_LIMBS];
	uint32_t product[CRYC_ECP_MAX_LIMBS];
	uint32_t e_value[CRYC_ECP_MAX_LIMBS];
	size_t size;
	size_t total;
	size_t index;
	int result;
	int error;

	/* The special hashes: n, n + 1, ~n (2^bits - 1 - n), all ones, zero, one and two. */
	size = ec->size;
	total = 1U + 2U * size;
	CHECK(hex_to_size(params->n, n_bytes, size) == 0);
	memcpy(n_plus_1, n_bytes, size);
	bytes_inc(n_plus_1, size);
	for (index = 0; index < size; index++)
		not_n[index] = (uint8_t)(n_bytes[index] ^ 0xffU);
	memset(ones, 0xff, size);
	memset(zeros, 0, size);
	memset(one, 0, size);
	one[size - 1U] = 1U;
	memset(two, 0, size);
	two[size - 1U] = 2U;

	/* E = 0 (an all-zero hash): u1 is zero, and the same signature verifies for the hash n (E = n mod n = 0). */
	make_signature(ec, d, public_key, zeros, size, r, s);
	error = cryc_ecdsa_verify(ec, public_key, total, zeros, size, r, s);
	CHECK(error == 0);
	error = cryc_ecdsa_verify(ec, public_key, total, n_bytes, size, r, s);
	CHECK(error == 0);
	error = cryc_ecdsa_verify(ec, public_key, total, one, size, r, s);
	CHECK(error == EBADMSG);

	/* The signature of the hash 1 verifies for n + 1 (E = n + 1 - n = 1), but not for 2 or n + 2. */
	make_signature(ec, d, public_key, one, size, r, s);
	error = cryc_ecdsa_verify(ec, public_key, total, one, size, r, s);
	CHECK(error == 0);
	error = cryc_ecdsa_verify(ec, public_key, total, n_plus_1, size, r, s);
	CHECK(error == 0);
	error = cryc_ecdsa_verify(ec, public_key, total, two, size, r, s);
	CHECK(error == EBADMSG);
	bytes_inc(n_plus_1, size);
	error = cryc_ecdsa_verify(ec, public_key, total, n_plus_1, size, r, s);
	CHECK(error == EBADMSG);

	/* The signature of ~n verifies for the all-ones hash (E = 2^bits - 1 - n after one subtraction). */
	make_signature(ec, d, public_key, not_n, size, r, s);
	error = cryc_ecdsa_verify(ec, public_key, total, not_n, size, r, s);
	CHECK(error == 0);
	error = cryc_ecdsa_verify(ec, public_key, total, ones, size, r, s);
	CHECK(error == 0);
	error = cryc_ecdsa_verify(ec, public_key, total, zeros, size, r, s);
	CHECK(error == EBADMSG);

	/* R at infinity: with E = -(r d) mod n, u1 + u2 d = 0 whatever s is, so R = O and the signature is refused. */
	random_scalar(d, ec);
	base_encode(ec, public_key, d);
	random_scalar(r_value, ec);
	random_scalar(s_value, ec);
	order_mul(ec, product, r_value, d);
	order_neg(ec, e_value, product);
	cryc_bn_encode(e_value, ec->limbs, e_bytes, size);
	cryc_bn_encode(r_value, ec->limbs, r, size);
	cryc_bn_encode(s_value, ec->limbs, s, size);
	error = cryc_ecdsa_verify(ec, public_key, total, e_bytes, size, r, s);
	CHECK(error == EBADMSG);

	/* The same key with a nearby E is an ordinary failure, and with a correct signature it verifies. */
	random_scalar(k, ec);
	result = sign(ec, d, k, e_bytes, size, r, s);
	if (result == 0) {
		error = cryc_ecdsa_verify(ec, public_key, total, e_bytes, size, r, s);
		CHECK(error == 0);
	}
}

/* Sets out to the right side x^3 - 3 x + b of the curve equation, all in Montgomery form. */
static void
rhs_mont(
	const struct cryc_ecp *ec,
	uint32_t *out,
	const uint32_t *x_mont)
{
	uint32_t square[CRYC_ECP_MAX_LIMBS];
	uint32_t triple[CRYC_ECP_MAX_LIMBS];

	/* x^3, then minus 3 x, then plus b. */
	memset(square, 0, sizeof(square));
	memset(triple, 0, sizeof(triple));
	cryc_bn_mont_mul(square, x_mont, x_mont, &ec->field);
	cryc_bn_mont_mul(out, square, x_mont, &ec->field);
	cryc_bn_mod_add(triple, x_mont, x_mont, &ec->field);
	cryc_bn_mod_add(triple, triple, x_mont, &ec->field);
	cryc_bn_mod_sub(out, out, triple, &ec->field);
	cryc_bn_mod_add(out, out, ec->b_mont, &ec->field);
}

/*
 * Sets out to a^((p + 1) / 4) (Montgomery form), a square root of a when a
 * is a square and p = 3 (mod 4), as both curves' p are; the caller checks
 * by squaring.
 */
static void
sqrt_mont(
	const struct cryc_ecp *ec,
	uint32_t *out,
	const uint32_t *a)
{
	uint8_t exponent[CRYC_ECP_MAX_BYTES];
	size_t index;

	/* p + 1, shifted right by two. */
	cryc_bn_encode(ec->field.modulus, ec->limbs, exponent, ec->size);
	bytes_inc(exponent, ec->size);
	for (index = ec->size; index > 0U; index--) {
		exponent[index - 1U] = (uint8_t)(exponent[index - 1U] >> 2);
		if (index > 1U)
			exponent[index - 1U] = (uint8_t)(exponent[index - 1U] | (uint8_t)(exponent[index - 2U] << 6));
	}
	cryc_bn_mont_exp(out, a, exponent, ec->size, &ec->field);
}

/*
 * Checks a verification whose R has an x coordinate at or above n.
 *
 * Such a point R = (n + t, y) is found by taking x = n + t for small t until
 * x^3 - 3 x + b is a square.  A key is then built so that the verifier's
 * u1 G + u2 Q is exactly R: with s and u1 chosen, E = u1 s, r = t,
 * u2 = r / s, and Q = u2^-1 R - (u1 / u2) G.  The verifier must reduce the
 * x coordinate modulo n before comparing with r (so r = t verifies), and
 * must refuse r = n + t (out of range).
 */
static void
check_x_above_order(
	const struct params *params,
	const struct cryc_ecp *ec)
{
	struct cryc_ecp_point r_point;
	struct cryc_ecp_point q_point;
	uint8_t n_bytes[CRYC_ECP_MAX_BYTES];
	uint8_t x_bytes[CRYC_ECP_MAX_BYTES];
	uint8_t r_encoded[1U + 2U * CRYC_ECP_MAX_BYTES];
	uint8_t public_key[1U + 2U * CRYC_ECP_MAX_BYTES];
	uint8_t hash[CRYC_ECP_MAX_BYTES];
	uint8_t r[CRYC_ECP_MAX_BYTES];
	uint8_t s[CRYC_ECP_MAX_BYTES];
	uint8_t big_r[CRYC_ECP_MAX_BYTES];
	uint32_t x_plain[CRYC_ECP_MAX_LIMBS];
	uint32_t x_mont[CRYC_ECP_MAX_LIMBS];
	uint32_t rhs[CRYC_ECP_MAX_LIMBS];
	uint32_t root[CRYC_ECP_MAX_LIMBS];
	uint32_t square[CRYC_ECP_MAX_LIMBS];
	uint32_t y_plain[CRYC_ECP_MAX_LIMBS];
	uint32_t s_value[CRYC_ECP_MAX_LIMBS];
	uint32_t s_inverse[CRYC_ECP_MAX_LIMBS];
	uint32_t u1[CRYC_ECP_MAX_LIMBS];
	uint32_t u2[CRYC_ECP_MAX_LIMBS];
	uint32_t u2_inverse[CRYC_ECP_MAX_LIMBS];
	uint32_t r_value[CRYC_ECP_MAX_LIMBS];
	uint32_t e_value[CRYC_ECP_MAX_LIMBS];
	uint32_t coefficient[CRYC_ECP_MAX_LIMBS];
	uint32_t negative[CRYC_ECP_MAX_LIMBS];
	size_t size;
	size_t total;
	unsigned t;
	unsigned hits;
	unsigned variant;
	int error;

	size = ec->size;
	total = 1U + 2U * size;
	CHECK(hex_to_size(params->n, n_bytes, size) == 0);
	hits = 0;
	memcpy(x_bytes, n_bytes, size);
	for (t = 1; t <= 80U && hits < 3U; t++) {
		/* x = n + t, which is below p (p - n is far larger than 80). */
		bytes_inc(x_bytes, size);
		scalar_from_bytes(x_plain, ec, x_bytes);
		CHECK(cryc_bn_lt(x_plain, ec->field.modulus, ec->limbs) != 0U);

		/* A square root of the curve equation's right side, if there is one. */
		memset(x_mont, 0, sizeof(x_mont));
		memset(rhs, 0, sizeof(rhs));
		memset(root, 0, sizeof(root));
		memset(square, 0, sizeof(square));
		memset(y_plain, 0, sizeof(y_plain));
		cryc_bn_to_mont(x_mont, x_plain, &ec->field);
		rhs_mont(ec, rhs, x_mont);
		sqrt_mont(ec, root, rhs);
		cryc_bn_mont_mul(square, root, root, &ec->field);
		if (memcmp(square, rhs, ec->limbs * sizeof(square[0])) != 0)
			continue;
		hits++;

		/* R = (n + t, y) as a point of the curve. */
		cryc_bn_from_mont(y_plain, root, &ec->field);
		r_encoded[0] = 0x04U;
		memcpy(r_encoded + 1U, x_bytes, size);
		cryc_bn_encode(y_plain, ec->limbs, r_encoded + 1U + size, size);
		error = cryc_ecp_decode(ec, &r_point, r_encoded, total);
		CHECK(error == 0);
		if (error != 0)
			continue;

		/* s and u1 are random; E = u1 s. */
		random_scalar(s_value, ec);
		random_scalar(u1, ec);
		order_inv(ec, s_inverse, s_value);
		order_mul(ec, e_value, u1, s_value);
		cryc_bn_encode(e_value, ec->limbs, hash, size);
		cryc_bn_encode(s_value, ec->limbs, s, size);

		/*
		 * Four values of r: t (the reduced x coordinate, which must
		 * verify) and t plus one bit in the second, the middle and the
		 * top limb (which must not: the comparison looks at every limb).
		 * Each gets its own key, so that the verifier's R is the same
		 * point for all four: u2 = r / s and Q = u2^-1 R - (u1 / u2) G.
		 */
		for (variant = 0; variant < 4U; variant++) {
			memset(r_value, 0, sizeof(r_value));
			r_value[0] = t;
			if (variant == 1U)
				r_value[1] += 1U;
			if (variant == 2U)
				r_value[ec->limbs / 2U] += 1U;
			if (variant == 3U)
				r_value[ec->limbs - 1U] += 1U;
			order_mul(ec, u2, r_value, s_inverse);
			order_inv(ec, u2_inverse, u2);
			order_mul(ec, coefficient, u1, u2_inverse);
			order_neg(ec, negative, coefficient);
			cryc_ecp_mul_add_public(ec, &q_point, negative, u2_inverse, &r_point);
			error = cryc_ecp_encode(ec, &q_point, public_key, total);
			CHECK(error == 0);
			if (error != 0)
				continue;
			cryc_bn_encode(r_value, ec->limbs, r, size);

			/* Only the reduced x coordinate verifies. */
			error = cryc_ecdsa_verify(ec, public_key, total, hash, size, r, s);
			CHECK(error == (variant == 0U ? 0 : EBADMSG));

			/* The x coordinate itself as r is out of range (it is not below n), and so is the next r. */
			if (variant == 0U) {
				memcpy(big_r, x_bytes, size);
				error = cryc_ecdsa_verify(ec, public_key, total, hash, size, big_r, s);
				CHECK(error == EBADMSG);
				memcpy(big_r, r, size);
				bytes_inc(big_r, size);
				error = cryc_ecdsa_verify(ec, public_key, total, hash, size, big_r, s);
				CHECK(error == EBADMSG);
			}
		}
	}
	printf("  %s verifications with x(R) at or above n: %u\n", params->name, hits);
	CHECK(hits == 3U);
}

/* Runs the checks of one curve. */
static void
run_curve(
	const struct params *params,
	const struct cavp_ecdh *cavp,
	const char *wycheproof_a,
	const char *wycheproof_b)
{
	struct cryc_ecp ec;
	int error;

	/* The curve. */
	printf("%s\n", params->name);
	error = cryc_ecp_init(&ec, params->id);
	CHECK(error == 0);
	if (error != 0)
		return;

	/* Key generation. */
	check_keygen_script(params, &ec);
	check_keygen_cavp(params, &ec);

	/* ECDH. */
	check_ecdh_cavp(params, &ec, cavp);
	if (params->id == CRYC_CURVE_P256)
		check_ecdh_wycheproof(params, &ec);
	check_ecdh_errors(params, &ec);
	check_ecdh_properties(params, &ec, 4U);
	check_ecdh_leading_zero(params, &ec);

	/* ECDSA. */
	check_sigver_cavp(params, &ec);
	check_wycheproof_ecdsa(&ec, wycheproof_a);
	check_wycheproof_ecdsa(&ec, wycheproof_b);
	check_roundtrip(&ec, 12U);
	check_verify_errors(params, &ec);
	check_hash_edges(params, &ec);
	check_x_above_order(params, &ec);
}

/* Runs the checks. */
int
main(void)
{
	/* P-256, then P-384. */
	run_curve(&curves[0], cavp_ecdh_p256, "wycheproof-ecdsa_secp256r1_sha256_test", "wycheproof-ecdsa_secp256r1_sha512_test");
	run_curve(&curves[1], cavp_ecdh_p384, "wycheproof-ecdsa_secp384r1_sha384_test", "wycheproof-ecdsa_secp384r1_sha512_test");

	/* The result. */
	return check_finish("test-ecc");
}
