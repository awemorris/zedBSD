/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks Ed25519 (cryc-ed25519.h).
 *
 * Expected values: the five test vectors of RFC 8032 (January 2017) 7.1
 * (public key and signature from the secret key, and the verification),
 * 26 further vectors (message lengths around the SHA-512 block borders,
 * up to 4096 bytes) made with the Python illustration of RFC 8032
 * section 6 from seeds and messages given in the table, and the 210
 * Wycheproof vectors of ed25519_test read from the vector file (a record
 * whose signature is not 64 bytes long or whose key is not 32 bytes long
 * cannot be given to the fixed-size interface and is counted as skipped;
 * "acceptable" records may verify or not, and are counted).  Properties
 * checked beside them: the determinism of signing, round trips over the
 * lengths 0 to 300 and a long message, every single-bit change of a
 * signature, a message and a key, the rejection of S + k * L (the
 * equation still holds, only the range check refuses), the boundary of
 * the S < L check with a signature under the neutral element as key, the
 * non-canonical and non-decodable encodings of R and of the key, outputs
 * written only where documented, inputs left unchanged, and random
 * inputs.
 */

#include "check.h"
#include "vecfile.h"

#include "userland/base/libcrypto-compat/cryc-ed25519.h"

/* A byte that fills a buffer to show later that nothing was written to it. */
#define SENTINEL_BYTE	0x5aU

/* The most bytes of a message in the tables of this file. */
#define MESSAGE_MAX	1024U

/* The state of the pseudo-random generator of the tests. */
static uint32_t rng_state = 0x2468aceU;

/*
 * One test vector of RFC 8032 7.1: name, secret key (seed), public key,
 * message and signature, all hex text.
 *
 * Values referenced: RFC 8032 (January 2017) 7.1, TEST 1, TEST 2, TEST 3,
 * TEST 1024 and TEST SHA(abc).
 */
struct rfc_vector {
	const char *name;
	const char *seed;
	const char *public_key;
	const char *message;
	const char *signature;
};

/*
 * A vector made with the Python illustration of RFC 8032 section 6: the
 * message is length bytes with byte i equal to (i * 7 + 3) mod 256, the
 * seed, public key and signature are hex text.
 *
 * Values referenced: RFC 8032 (January 2017) 6, the functions
 * secret_to_public and sign of the illustration, run on the seeds below.
 */
struct made_vector {
	unsigned int length;
	const char *seed;
	const char *public_key;
	const char *signature;
};

/* The vectors of RFC 8032 7.1. */
static const struct rfc_vector rfc_vectors[] = {
	{
	    "1",
	    "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
	    "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
	    "",
	    "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
	    "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"
	},
	{
	    "2",
	    "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
	    "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
	    "72",
	    "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
	    "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"
	},
	{
	    "3",
	    "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
	    "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
	    "af82",
	    "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
	    "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"
	},
	{
	    "1024",
	    "f5e5767cf153319517630f226876b86c8160cc583bc013744c6bf255f5cc0ee5",
	    "278117fc144c72340f67d0f2316e8386ceffbf2b2428c9c51fef7c597f1d426e",
	    "08b8b2b733424243760fe426a4b54908632110a66c2f6591eabd3345e3e4eb98"
	    "fa6e264bf09efe12ee50f8f54e9f77b1e355f6c50544e23fb1433ddf73be84d8"
	    "79de7c0046dc4996d9e773f4bc9efe5738829adb26c81b37c93a1b270b20329d"
	    "658675fc6ea534e0810a4432826bf58c941efb65d57a338bbd2e26640f89ffbc"
	    "1a858efcb8550ee3a5e1998bd177e93a7363c344fe6b199ee5d02e82d522c4fe"
	    "ba15452f80288a821a579116ec6dad2b3b310da903401aa62100ab5d1a36553e"
	    "06203b33890cc9b832f79ef80560ccb9a39ce767967ed628c6ad573cb116dbef"
	    "efd75499da96bd68a8a97b928a8bbc103b6621fcde2beca1231d206be6cd9ec7"
	    "aff6f6c94fcd7204ed3455c68c83f4a41da4af2b74ef5c53f1d8ac70bdcb7ed1"
	    "85ce81bd84359d44254d95629e9855a94a7c1958d1f8ada5d0532ed8a5aa3fb2"
	    "d17ba70eb6248e594e1a2297acbbb39d502f1a8c6eb6f1ce22b3de1a1f40cc24"
	    "554119a831a9aad6079cad88425de6bde1a9187ebb6092cf67bf2b13fd65f270"
	    "88d78b7e883c8759d2c4f5c65adb7553878ad575f9fad878e80a0c9ba63bcbcc"
	    "2732e69485bbc9c90bfbd62481d9089beccf80cfe2df16a2cf65bd92dd597b07"
	    "07e0917af48bbb75fed413d238f5555a7a569d80c3414a8d0859dc65a46128ba"
	    "b27af87a71314f318c782b23ebfe808b82b0ce26401d2e22f04d83d1255dc51a"
	    "ddd3b75a2b1ae0784504df543af8969be3ea7082ff7fc9888c144da2af58429e"
	    "c96031dbcad3dad9af0dcbaaaf268cb8fcffead94f3c7ca495e056a9b47acdb7"
	    "51fb73e666c6c655ade8297297d07ad1ba5e43f1bca32301651339e22904cc8c"
	    "42f58c30c04aafdb038dda0847dd988dcda6f3bfd15c4b4c4525004aa06eeff8"
	    "ca61783aacec57fb3d1f92b0fe2fd1a85f6724517b65e614ad6808d6f6ee34df"
	    "f7310fdc82aebfd904b01e1dc54b2927094b2db68d6f903b68401adebf5a7e08"
	    "d78ff4ef5d63653a65040cf9bfd4aca7984a74d37145986780fc0b16ac451649"
	    "de6188a7dbdf191f64b5fc5e2ab47b57f7f7276cd419c17a3ca8e1b939ae49e4"
	    "88acba6b965610b5480109c8b17b80e1b7b750dfc7598d5d5011fd2dcc5600a3"
	    "2ef5b52a1ecc820e308aa342721aac0943bf6686b64b2579376504ccc493d97e"
	    "6aed3fb0f9cd71a43dd497f01f17c0e2cb3797aa2a2f256656168e6c496afc5f"
	    "b93246f6b1116398a346f1a641f3b041e989f7914f90cc2c7fff357876e506b5"
	    "0d334ba77c225bc307ba537152f3f1610e4eafe595f6d9d90d11faa933a15ef1"
	    "369546868a7f3a45a96768d40fd9d03412c091c6315cf4fde7cb68606937380d"
	    "b2eaaa707b4c4185c32eddcdd306705e4dc1ffc872eeee475a64dfac86aba41c"
	    "0618983f8741c5ef68d3a101e8a3b8cac60c905c15fc910840b94c00a0b9d0",
	    "0aab4c900501b3e24d7cdf4663326a3a87df5e4843b2cbdb67cbf6e460fec350"
	    "aa5371b1508f9f4528ecea23c436d94b5e8fcd4f681e30a6ac00a9704a188a03"
	},
	{
	    "SHA(abc)",
	    "833fe62409237b9d62ec77587520911e9a759cec1d19755b7da901b96dca3d42",
	    "ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf",
	    "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
	    "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
	    "dc2a4459e7369633a52b1bf277839a00201009a3efbf3ecb69bea2186c26b589"
	    "09351fc9ac90b3ecfdfbc7c66431e0303dca179c138ac17ad9bef1177331a704"
	},
};

/* The vectors made with the illustration of RFC 8032 6. */
static const struct made_vector made_vectors[] = {
	{ 0, "19363a3b66b1e50b1c665591823f6733fe4e68b508f563b9530ff768cc0cb0e8",
	    "cf30913bb21e6d6992a0df01dbcbda8c31f9c92d61a2ab3419ee18ca551fab2d",
	    "a2c7fa195bbcdbce2de0bdd30e6dae33cdaeb00903e1ea3dbb0e467e9d9d9b6e8bbc2406afc2497689103c38f79fbeb1c05e945d759de25d6d437af7bb86a909" },
	{ 1, "81742413f83ec0a9caec1cb3adb756e0cf041f0330a7e315d9acd436caafda18",
	    "0c922dffd94eb86e197ae2aab80d43def5b89a60cb2bf49338754f009622df2f",
	    "a9a53e11311abb66b9bd76ed293767bc7bfd2713bce6a4a9211f7257ccd567dcac78dec4838a4d00739149d59bf92f71af08949c73df810eaabe203d6825c202" },
	{ 2, "8a5ed6dcd5c8478d22053296350718fd804fcafa123addd10282fc54aa29bec1",
	    "7712e0d9125b9832027d841d8d4d4ab721dd4d8275dd17a1fe1595a07de54b0b",
	    "456ea44f5694e31b23f7f171499526d48dca816645e070c013fd2a1e20d3b4f30e3fbb2d5905d2c88307126812a123635431df82da5e15ccd5cc982a7abbc504" },
	{ 31, "905fa17b473d756a974bf21e5479dc00acd56bf2e07b622b1d6e07ac2fa3337a",
	    "cb1786a914337a6f17d3979ce836322f0dc89e10b205ef4b77eceb43e1f085bb",
	    "8cabb0ab6958c6b472e95d82c4ae3cc0113be48b4ff73c0a94083a7c4efb4776157ac1fd8fa6f2d72f1687895078c5ce289dc3374ee72d795147f219db04190f" },
	{ 32, "8229f50ed4d21977ae6fda2412450559a98cbc427d9988ea28543affed1a15d1",
	    "0422d22c01d0e33ed38272bac3dade856558991b13061bf8b5fb67d2e526498c",
	    "90594f9439b9f56a24bb2de4988f705d67abc5932ccbbc4166fd97a3e3844e72ee8ba18844e46719698b0520832a950dc4f01f3cc35ba703441e73ce437b6909" },
	{ 33, "032d2248381f4800b6ff4d57b6bb4dbb891a0993c101462325dd1469a9a8698c",
	    "ae6046607f0f998bd7ed51fcf455e76c428bda89233b443492fd8838940b02b9",
	    "f6dca58d5c320ecee4b312c63739f708448a15852642de8ca1aa95e7fa6127fc4f70a0af9471c6a458f41662892821a036c65559bfffb6aaa2e859dc8e168d05" },
	{ 63, "a8c8e52894b0f7ed1f84ed3a0cca81e8f6340e67f0579a06802a5507bf917f4e",
	    "a2aae720686eb90394197e6b15a7a991aefd9cdc45e89afb03ac2c2df4bdba53",
	    "1271e58f52ddafe380c6a4bc75e65a69102160c9b9eb20f17e64639f8a99858cd8c7415a9708f6db87dc5360ab122c7f78bcd4842c0c1373eb35b17c54fa1d08" },
	{ 64, "6552298b53d9075fb82eb062c607e684d646298b31d14007f06546ce41744a8d",
	    "dd99c62b827f2630ae39615cb7ae934e4d81a3df99c8d28dd5636aedb71df0ee",
	    "a62186c25f43c6ed64a1b3786c85d025d620efc838be55e7c74ccd456cf18af1cc5b9e21997cecc0dc7a8acf1160e86205491aaae788b092f8825f745305210e" },
	{ 65, "18686fc8a6bf3840a08c5c31d7c41693f3a23ed1dc8efb9cfbd8d2692c284fd5",
	    "a9d4aad6592c9d801a1f20b4e40285f002477289dcba24898942359ed2cece82",
	    "502e42caa05e7dd927123a4e00abd969783bedb53db7aeb8e7d06fcefebd2fb1b2c89d5deaf1aae6d754543bff1bb9bbe293244de07d93327a5d7f511513a904" },
	{ 95, "921604a19ce0aeac7d854917e74f13bf95367d609206bdbfbcc9fb8d2390c561",
	    "fa76dad98d02b965bbf9c668ccf89a13052678b8dec94107e29645f270811194",
	    "6ff47d3ab4d98b52231b342b4f4f569255ec8898296e45394b00bd779fa182f49e4e6b57396865c72d54aa2202b9340b44126137cfccb96ee63d79e04e10fa0c" },
	{ 96, "5192686c57730f71532da8e75cf4143e5f9f7e185eab39e0d4064ad9d86d93dc",
	    "31b5f97f2b6eca1589dca019ab63eaf5c43468f5665898e494acd7a005449020",
	    "d3e65d1e7786d5d56409e9fc084fdee7d25d2fdde58a947a4449f5789c063b72255017ba1b1649cdf06beea2f9dd223a59d977a6a42539d15e549c9add6c290d" },
	{ 97, "d23562984d3604288bd938158994e92c22f359972cfeb8f20be6bcc4d9f6a751",
	    "11c501241061ca9ac80d8067ad3c7f66189db565b90157d417eaf207a310fc1b",
	    "a4988f3ed5fdf6bff7b8ad4e728a2146b205ffcf4b41f4e495820adf867fcf5fef9f646b3d8586d0f2c871e404211f72bb61146af7add4e6c9a4e0e2476c3a07" },
	{ 111, "f2aee0fdff585571fca12f8f5931ee5a58e659fecf3e67a4afee1e211f3ce314",
	    "0f6d4fb8c4e61c0503204e7aa399002ad0e2144eee36d7852210e32ab2c7d9f9",
	    "50b1811602be6833939defab5883e3ae0c0730ba6749fe5569e317d079b27c35b9a27a08ac6b76a824c6a5bd70de58888d630b415812a007662c0842f10af909" },
	{ 112, "c196011f02c3c45d3810b3dfdc5b80a6735dccec00e0c27403e4c407be6a4e75",
	    "afebc8df15c2f8f885fca1b9f16ad257659bf549fa1e699b50d0aecabfc832ef",
	    "fcf1a4fc4cd889c9b4b4ad4ebf011a07bda0cedf3773dcd71d64161b6f490bc4522eb6b101d4473552bfc3f9bcc5afc8a9ce7b352666eb4843bca769e8caa109" },
	{ 113, "832f9d1fcdd1417e7fcb8450abe8450ac61d3804ea514f92d5aaac598099a7c0",
	    "7eed31d80273129f6c6d86e660ae3496c27e29d166c4b6ce5ff72cbe2d4b034a",
	    "9c922cdd3c54253f2983239487ec51ccb133216eda695e30948586abbda9235329e91d830d46fc177215c0b8fdb91458b63c4a2100c5bc9ed3f607e3614ac501" },
	{ 127, "195252ede03afc39dd61c7eb7f1ba7ef8a1e51f5c4d8acebdb346955edca6442",
	    "46c830bb961d78e70b357781453fdabbb7d9f79a575eb44971e4a532ab1743d6",
	    "3fe91b09e3b00e616c6bcdc180d1e7598183ff98fb58b1aab24ceab5643925ce972b376978f2d6e7c7d8e245762f55d0683eae7759f1657db001911aeae61b01" },
	{ 128, "01fef45e321402043cd96bdc1dc275aa4b09ae4f1dc5a8f404d4ee741847954d",
	    "d62d55307bead8e6648c407952d09ab441099dd4f6e4cbe981bacc2a74d343e7",
	    "7fb60cb56af5cc4030299bbcdddb1e8e67e9d0e6ccd97a7e0471877802411bf74692f7e1119004ccf3502b6f01622ebbbb809edac1eca6672f1f4bb707fa2903" },
	{ 129, "816e03da01ab913f729f293b17545f98c093a7f2d2adab5463139daefe51224c",
	    "b06cad74ef0eba3a056b88bccaa8abe440ba0068b1df5b86a056fa005ba35df8",
	    "60917e937b65dbfbeffd2ba6289e5e68ecd41552db48bd75c68aa35cb0706afa269b21ead46a53afda11a771a9441f380494db3fef60ac9008f7ae240fd9f309" },
	{ 191, "0c52717d9df8b144e3fd874e3f5583c0f8988d9068b7eeed2f8348dc07e41735",
	    "edc3b2234ccd05ecc2d3fc779845b356ec1a12840044391f33eead76ed4d8317",
	    "848cbe5b6ed15ab18cbc04864e5876880de083c48181438fbb26c02b5fd1556717786e56ff2037f2f7c2dac0d64500f38dc575ff2c4e4e6b6529fd0beaff1a06" },
	{ 192, "565fc2de8473b1a011532626c72b472bdb428fb2a576b58557cf22c71f4e5467",
	    "dca406054019e6a6deb2f39d9c8e76d0623915b9082be9de2e33811b535d1a1e",
	    "afa92217da8b1520d551dffb17a65f0580927047c58379ba381fa268cdbbb01268e32e3dfa81986d6d7d094ebce90a0eb2d0bf2c1088f70e0147fefbc3957505" },
	{ 193, "d96c06b723104e998c49bc852cfc5783d5643de5d2a1f72dd5301d8945df0d3c",
	    "540bfab7c3c07ab04794b68658295a02654a55ef10dcfbb6542ca12c29ffbd6c",
	    "34116491152ea716c4cfb32fd7e75a30b758acbc5ad53d2e0b9d1d84b771b4c501b240734b2ee0d6ba4b97187dc47dffd3ba9711d9ebd88fe79ac891c4d63903" },
	{ 255, "5e2e073d8fa8233dbeccca5a87b779f563bdf8e32237122535c114ae45810e26",
	    "272b74e5c8cb90f22b3cf2d8751abb7496cc9831b5859442d3743990db27500c",
	    "2e5d818cbe9ac84ff836b0136ddfa1fc422b5877b88d58366cd2aaa3eb3b2ab8beb9ea8dbb9951580881b86cba3a25f289f241488c4b08ea4977444d3a3d350f" },
	{ 256, "d2d4cca957b1cd9b8c4d40e717419c03faf7d0cc3a1281070bccf458ed46f750",
	    "c799d129106141715522272beec5ae2afa61c90ff53028245a13329a7f168c39",
	    "769e4432c0099c3c22987d5ca81a136788ec5daa94611e647d3e53e446acc7d4f068d8764f298f3aec78ba86c506691fc3aba76391a048df0614018b138af502" },
	{ 257, "0d73af4e11db7d3e101562e55a2e0f931f09dd6bb4b553ca5b8b6461aa09720a",
	    "b6468634f533caf4fef782354856b90c93348ab5f1ddcfad3232bc59eb806c26",
	    "1077a8c471354c3a98508f7cc2b01198e56f129987d051af0c0ff22343ec63eba300e2f71d53015b1c74a976e5a30e970d918737d0d791ef3b68289620ddd60b" },
	{ 1000, "372e0a9af296441c8397f8e3bde12935844230664eb56bd36d384fe57e9c5e8a",
	    "e0b168f12563f67a115833a725f6f210def368757b0c45d7818784a9af8218f5",
	    "b9bd66c06ac2313803aa24c74ee0d93e34b7706331221842cc9ad76b498122ccd34fd4fd141b2482e1f856965c7fa4b30e9b5403ec397e4e123ea727a256ae07" },
	{ 4096, "ccbd359a0e37c075f262e02570e3e19a5bcbbea5db474f69d7ce2b28ea895930",
	    "82c1257c080bc32b11e54de7ab60ac3be0f99ca0546413868d32491dee4b880d",
	    "ea941b91eb5449e923faead30c6f7b9f3ad628219a03300140a5838fde7850a2038981c912b0a67ac69a9ea4c16037fc1c9dc42ecf1e9303f542902f89b28609" },
};

/*
 * The group order L of RFC 8032 5.1, Table 1, little-endian: 2^252 +
 * 27742317777372353535851937790883648493.
 *
 * Values referenced: RFC 8032 (January 2017) 5.1, Table 1, L.
 */
static const char order_hex[] = "edd3f55c1a631258d69cf7a2def9de1400000000000000000000000000000010";

/*
 * The encodings of points the tests build signatures from.
 *
 * Values referenced: RFC 8032 (January 2017) 5.1.2 and 5.1, Table 1:
 * the neutral element (0, 1) is y = 1; the base point B has y = 4/5
 * (0x58 and 0x66 31 times) and an even x, so -B is the same bytes with
 * bit 255 set; y = p - 1 is the point (0, -1); p, p + 1 and 2^255 - 1 are
 * the values of y at or above p (5.1.3 step 1).
 */
static const char identity_hex[] = "0100000000000000000000000000000000000000000000000000000000000000";
static const char base_hex[] = "5866666666666666666666666666666666666666666666666666666666666666";
static const char minus_base_hex[] = "58666666666666666666666666666666666666666666666666666666666666e6";
static const char identity_sign_hex[] = "0100000000000000000000000000000000000000000000000000000000000080";
static const char identity_plus_p_hex[] = "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f";
static const char y_equals_p_hex[] = "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f";
static const char y_all_ones_hex[] = "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f";
static const char y_minus_one_hex[] = "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f";
static const char y_minus_one_sign_hex[] = "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";

/*
 * The tcId of the Wycheproof "acceptable" records (small-order and
 * mixed-order public keys) that the cofactorless equation [S]B = R +
 * [k]A' of 5.1.7 step 3 accepts, with k the full 512-bit integer of step
 * 2 (not reduced modulo L, which changes the result for a key with a
 * component of small order).
 *
 * Values referenced: RFC 8032 (January 2017) 5.1.7 steps 1 to 3 evaluated
 * on the 29 acceptable records with the point functions of the
 * illustration in section 6 (the equation of step 3, k not reduced).
 */
static const unsigned int accepted_ids[] = { 152U, 161U, 162U, 165U, 172U, 175U, 176U, 177U };

/* Returns the next pseudo-random 32-bit value (xorshift32). */
static uint32_t
random32(void)
{
	/* Marsaglia's xorshift32 with the shifts 13, 17, 5. */
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

/* Fills size bytes with pseudo-random bytes. */
static void random_bytes(uint8_t *out, size_t size);

/* Fills out with the message pattern of the made vectors: byte i is (i * 7 + 3) mod 256. */
static void pattern_message(uint8_t *out, size_t size);

/* Decodes exactly size bytes of hex text into out; checks the length. */
static void decode_fixed(const char *text, uint8_t *out, size_t size);

/* Checks one vector: public key from the seed, signature from the seed and message, and the verification. */
static void check_vector(const uint8_t *seed, const uint8_t *message, size_t message_size, const uint8_t *want_public, const uint8_t *want_signature);

/* Fills size bytes with pseudo-random bytes. */
static void
random_bytes(
	uint8_t *out,
	size_t size)
{
	size_t index;

	/* One byte at a time. */
	for (index = 0; index < size; index++)
		out[index] = (uint8_t)(random32() >> 9);
}

/* Fills out with the message pattern of the made vectors: byte i is (i * 7 + 3) mod 256. */
static void
pattern_message(
	uint8_t *out,
	size_t size)
{
	size_t index;

	/* One byte at a time. */
	for (index = 0; index < size; index++)
		out[index] = (uint8_t)((index * 7U + 3U) & 255U);
}

/* Decodes exactly size bytes of hex text into out; checks the length. */
static void
decode_fixed(
	const char *text,
	uint8_t *out,
	size_t size)
{
	size_t got;

	/* The text must be exactly size bytes of hex. */
	got = hex_decode(text, out, size);
	CHECK(got == size);
}

/* Checks one vector: public key from the seed, signature from the seed and message, and the verification. */
static void
check_vector(
	const uint8_t *seed,
	const uint8_t *message,
	size_t message_size,
	const uint8_t *want_public,
	const uint8_t *want_signature)
{
	uint8_t public_key[32];
	uint8_t signature[64];

	/* The public key of the seed. */
	memset(public_key, SENTINEL_BYTE, sizeof(public_key));
	CHECK(cryc_ed25519_public(public_key, seed) == 0);
	CHECK_BYTES(public_key, want_public, 32U);

	/* The signature of the message. */
	memset(signature, SENTINEL_BYTE, sizeof(signature));
	CHECK(cryc_ed25519_sign(signature, message, message_size, seed) == 0);
	CHECK_BYTES(signature, want_signature, 64U);

	/* The published signature verifies under the published key. */
	CHECK(cryc_ed25519_verify(want_signature, message, message_size, want_public) == 0);
}

/* Checks the vectors of RFC 8032 7.1. */
static void
check_known(void)
{
	uint8_t seed[32];
	uint8_t public_key[32];
	uint8_t signature[64];
	uint8_t message[MESSAGE_MAX];
	size_t size;
	size_t index;

	/* The five vectors of RFC 8032 7.1. */
	for (index = 0; index < sizeof(rfc_vectors) / sizeof(rfc_vectors[0]); index++) {
		decode_fixed(rfc_vectors[index].seed, seed, sizeof(seed));
		decode_fixed(rfc_vectors[index].public_key, public_key, sizeof(public_key));
		decode_fixed(rfc_vectors[index].signature, signature, sizeof(signature));
		size = hex_decode(rfc_vectors[index].message, message, sizeof(message));
		CHECK(size != (size_t)-1);
		check_vector(seed, message, size, public_key, signature);
	}

	/* The 1023-byte message of TEST 1024 and the 64-byte one of TEST SHA(abc) were read in full. */
	CHECK(hex_decode(rfc_vectors[3].message, message, sizeof(message)) == 1023U);
	CHECK(hex_decode(rfc_vectors[4].message, message, sizeof(message)) == 64U);
}

/* Checks the made vectors, whose messages (up to 4096 bytes) need a larger buffer. */
static void
check_made(void)
{
	static uint8_t message[4096];
	uint8_t seed[32];
	uint8_t public_key[32];
	uint8_t signature[64];
	size_t index;

	/* Each vector: the pattern message of its length. */
	for (index = 0; index < sizeof(made_vectors) / sizeof(made_vectors[0]); index++) {
		decode_fixed(made_vectors[index].seed, seed, sizeof(seed));
		decode_fixed(made_vectors[index].public_key, public_key, sizeof(public_key));
		decode_fixed(made_vectors[index].signature, signature, sizeof(signature));
		pattern_message(message, made_vectors[index].length);
		check_vector(seed, message, made_vectors[index].length, public_key, signature);
	}

	/* The table reaches the empty message and the longest. */
	CHECK(made_vectors[0].length == 0U);
	CHECK(made_vectors[sizeof(made_vectors) / sizeof(made_vectors[0]) - 1U].length == 4096U);
}

/* Checks the Wycheproof vectors: each record that fits the fixed sizes, by its result. */
static void
check_wycheproof(void)
{
	struct vec_file file;
	uint8_t public_key[32];
	uint8_t signature[64];
	uint8_t message[MESSAGE_MAX];
	uint8_t scratch[MESSAGE_MAX];
	const char *text;
	const char *result;
	size_t public_size;
	size_t signature_size;
	size_t message_size;
	int have_key;
	int error;
	int want_accept;
	unsigned int tc_id;
	size_t index;
	unsigned long valid;
	unsigned long invalid;
	unsigned long acceptable;
	unsigned long accepted;
	unsigned long skipped;
	unsigned long tests;
	const char *tc_text;
	int opened;
	int more;
	int valid_match;
	int invalid_match;

	/* Opens the Wycheproof vector file. */
	opened = vec_open(&file, "wycheproof-ed25519_test");
	if (opened != 0) {
		CHECK(0);
		return;
	}

	/* Starts with no record counted and no group key seen. */
	valid = 0;
	invalid = 0;
	acceptable = 0;
	accepted = 0;
	skipped = 0;
	tests = 0;
	have_key = 0;
	public_size = 0;
	memset(public_key, 0, sizeof(public_key));

	/*
	 * The group's key is on the first record of the group (the PEM text in
	 * the group fields holds a blank line, which ends a record), so it is
	 * remembered; a record with a "tcId" is a test.
	 */
	for (more = vec_next(&file);
	     more;
	     more = vec_next(&file)) {
		text = vec_get(&file, "group.publicKey.pk");
		if (text != NULL) {
			public_size = hex_decode(text, scratch, sizeof(scratch));
			CHECK(public_size == 32U);
			memcpy(public_key, scratch, sizeof(public_key));
			have_key = 1;
		}

		/* A record without a test id carries only the group fields. */
		tc_text = vec_get(&file, "tcId");
		if (tc_text == NULL)
			continue;
		CHECK(have_key == 1);
		tests++;
		result = vec_get(&file, "result");
		CHECK(result != NULL);
		message_size = vec_hex(&file, "msg", message, sizeof(message));
		CHECK(message_size != (size_t)-1);
		signature_size = vec_hex(&file, "sig", scratch, sizeof(scratch));
		CHECK(signature_size != (size_t)-1);

		/* A signature of another size cannot be given to the interface; the file says such records are invalid. */
		if (signature_size != 64U) {
			CHECK(strcmp(result, "invalid") == 0);
			skipped++;
			continue;
		}

		/* Verifies the signature of the record. */
		memcpy(signature, scratch, sizeof(signature));
		error = cryc_ed25519_verify(signature, message, message_size, public_key);
		/* Which verdict the file gives for the record. */
		valid_match = strcmp(result, "valid");
		invalid_match = strcmp(result, "invalid");
		if (valid_match == 0) {
			CHECK(error == 0);
			valid++;
		} else if (invalid_match == 0) {
			CHECK(error == EBADMSG);
			invalid++;
		} else {
			/* Either answer is allowed by the file; this implementation's answer is the equation of step 3 with the full k. */
			tc_id = (unsigned int)strtoul(tc_text, NULL, 10);
			want_accept = 0;
			for (index = 0; index < sizeof(accepted_ids) / sizeof(accepted_ids[0]); index++) {
				if (accepted_ids[index] == tc_id)
					want_accept = 1;
			}

			/* The expected answer follows the table of accepted ids. */
			if (want_accept != 0)
				CHECK(error == 0);
			else
				CHECK(error == EBADMSG);
			acceptable++;
			if (error == 0)
				accepted++;
		}
	}

	/* Releases the file. */
	vec_close(&file);

	/* All 210 records were read; the counts match the file (117 valid, 64 invalid, 29 acceptable; 12 of the invalid ones with another signature size). */
	CHECK(tests == 210U);
	CHECK(valid == 117U);
	CHECK(invalid + skipped == 64U);
	CHECK(skipped == 12U);
	CHECK(acceptable == 29U);
	CHECK(accepted == 8U);
	printf("wycheproof: %lu valid, %lu invalid, %lu skipped (other sizes), %lu acceptable of which %lu verified\n",
	    valid, invalid, skipped, acceptable, accepted);
}

/* Checks signing for the lengths 0 to 300: deterministic, verifies, and the key matches. */
static void
check_round_trips(void)
{
	uint8_t seed[32];
	uint8_t public_key[32];
	uint8_t other_public[32];
	uint8_t signature[64];
	uint8_t again[64];
	uint8_t message[300];
	size_t size;
	unsigned int round;
	static uint8_t long_message[1000];

	/* One fresh key per length, a pattern message of that length. */
	for (size = 0; size <= sizeof(message); size++) {
		random_bytes(seed, sizeof(seed));
		pattern_message(message, size);
		CHECK(cryc_ed25519_public(public_key, seed) == 0);
		CHECK(cryc_ed25519_sign(signature, message, size, seed) == 0);

		/* The signature verifies, and is the same again (the nonce comes from the seed and the message). */
		CHECK(cryc_ed25519_verify(signature, message, size, public_key) == 0);
		memset(again, 0xa5, sizeof(again));
		CHECK(cryc_ed25519_sign(again, message, size, seed) == 0);
		CHECK_BYTES(again, signature, 64U);

		/* The key is the same from a second call. */
		CHECK(cryc_ed25519_public(other_public, seed) == 0);
		CHECK_BYTES(other_public, public_key, 32U);
	}

	/* Random seeds and random messages of random length below 1000. */
	for (round = 0; round < 40U; round++) {
		random_bytes(seed, sizeof(seed));
		size = random32() % 1000U;
		random_bytes(long_message, size);
		CHECK(cryc_ed25519_public(public_key, seed) == 0);
		CHECK(cryc_ed25519_sign(signature, long_message, size, seed) == 0);
		CHECK(cryc_ed25519_verify(signature, long_message, size, public_key) == 0);
	}
}

/* Checks a long message (more than a megabyte). */
static void
check_long_message(void)
{
	uint8_t seed[32];
	uint8_t public_key[32];
	uint8_t signature[64];
	uint8_t *message;
	size_t size;

	/* A message of 1 MiB + 3 bytes. */
	size = (size_t)1048576U + 3U;
	message = malloc(size);
	CHECK(message != NULL);
	if (message == NULL)
		return;
	pattern_message(message, size);
	random_bytes(seed, sizeof(seed));
	CHECK(cryc_ed25519_public(public_key, seed) == 0);
	CHECK(cryc_ed25519_sign(signature, message, size, seed) == 0);
	CHECK(cryc_ed25519_verify(signature, message, size, public_key) == 0);

	/* The last byte of the message matters. */
	message[size - 1U] ^= 1U;
	CHECK(cryc_ed25519_verify(signature, message, size, public_key) == EBADMSG);
	free(message);
}

/* Checks that every single-bit change of a signature, a message and a key is refused. */
static void
check_tampering(void)
{
	static const unsigned int lengths[] = { 0U, 1U, 129U };
	uint8_t seed[32];
	uint8_t public_key[32];
	uint8_t signature[64];
	uint8_t altered[64];
	uint8_t message[129];
	uint8_t changed[130];
	uint8_t key[32];
	unsigned int which;
	unsigned int bit;

	/* Each length: a fresh key, a signature, and then every bit change. */
	for (which = 0; which < sizeof(lengths) / sizeof(lengths[0]); which++) {
		random_bytes(seed, sizeof(seed));
		pattern_message(message, lengths[which]);
		CHECK(cryc_ed25519_public(public_key, seed) == 0);
		CHECK(cryc_ed25519_sign(signature, message, lengths[which], seed) == 0);
		CHECK(cryc_ed25519_verify(signature, message, lengths[which], public_key) == 0);

		/* Each of the 512 bits of the signature. */
		for (bit = 0; bit < 512U; bit++) {
			memcpy(altered, signature, sizeof(altered));
			altered[bit >> 3] ^= (uint8_t)(1U << (bit & 7U));
			CHECK(cryc_ed25519_verify(altered, message, lengths[which], public_key) == EBADMSG);
		}

		/* Each bit of the message. */
		for (bit = 0; bit < lengths[which] * 8U; bit++) {
			memcpy(changed, message, lengths[which]);
			changed[bit >> 3] ^= (uint8_t)(1U << (bit & 7U));
			CHECK(cryc_ed25519_verify(signature, changed, lengths[which], public_key) == EBADMSG);
		}

		/* Each of the 256 bits of the key. */
		for (bit = 0; bit < 256U; bit++) {
			memcpy(key, public_key, sizeof(key));
			key[bit >> 3] ^= (uint8_t)(1U << (bit & 7U));
			CHECK(cryc_ed25519_verify(signature, message, lengths[which], key) == EBADMSG);
		}

		/* A signature does not verify an extra byte or one byte less. */
		if (lengths[which] > 0U)
			CHECK(cryc_ed25519_verify(signature, message, lengths[which] - 1U, public_key) == EBADMSG);
		memcpy(changed, message, lengths[which]);
		changed[lengths[which]] = 0U;
		CHECK(cryc_ed25519_verify(signature, changed, lengths[which] + 1U, public_key) == EBADMSG);
	}
}

/* Checks that S + k * L (k = 1 to 8) is refused although the equation holds, and S with its high bits set. */
static void
check_malleability(void)
{
	uint8_t seed[32];
	uint8_t public_key[32];
	uint8_t signature[64];
	uint8_t altered[64];
	uint8_t order[32];
	uint8_t message[9];
	unsigned int carry;
	unsigned int sum;
	unsigned int index;
	unsigned int multiple;

	/* A key and a signature over a short message. */
	decode_fixed(order_hex, order, sizeof(order));
	memcpy(message, "malleable", sizeof(message));
	random_bytes(seed, sizeof(seed));
	CHECK(cryc_ed25519_public(public_key, seed) == 0);
	CHECK(cryc_ed25519_sign(signature, message, sizeof(message), seed) == 0);
	CHECK(cryc_ed25519_verify(signature, message, sizeof(message), public_key) == 0);

	/* S + k * L for k = 1 to 8: below 2^256, the same point, never accepted. */
	memcpy(altered, signature, sizeof(altered));
	for (multiple = 1; multiple <= 8U; multiple++) {
		carry = 0;
		for (index = 0; index < 32U; index++) {
			sum = (unsigned int)altered[32U + index] + (unsigned int)order[index] + carry;
			altered[32U + index] = (uint8_t)sum;
			carry = sum >> 8;
		}

		/* The sum stayed below 2^256, and the altered signature is refused. */
		CHECK(carry == 0U);
		CHECK(cryc_ed25519_verify(altered, message, sizeof(message), public_key) == EBADMSG);
	}

	/* The top three bits of S set (as large as 2^256 - 1 allows). */
	memcpy(altered, signature, sizeof(altered));
	altered[63] |= 0xe0U;
	CHECK(cryc_ed25519_verify(altered, message, sizeof(message), public_key) == EBADMSG);
	memset(altered + 32, 0xff, 32U);
	CHECK(cryc_ed25519_verify(altered, message, sizeof(message), public_key) == EBADMSG);
}

/*
 * Checks the boundary of S < L and the strict decoding of R and of the
 * key, with signatures under the neutral element as the key: [k]A is the
 * neutral element for every k, so the equation is [S]B = R.
 */
static void
check_edges(void)
{
	uint8_t identity[32];
	uint8_t base[32];
	uint8_t minus_base[32];
	uint8_t order[32];
	uint8_t signature[64];
	uint8_t key[32];
	uint8_t one;
	uint8_t message[3];
	unsigned int index;
	unsigned int accepted;
	unsigned int refused;
	int error;

	/* The encodings of the points the checks use. */
	decode_fixed(identity_hex, identity, sizeof(identity));
	decode_fixed(base_hex, base, sizeof(base));
	decode_fixed(minus_base_hex, minus_base, sizeof(minus_base));
	decode_fixed(order_hex, order, sizeof(order));
	memcpy(message, "abc", sizeof(message));

	/* R = the neutral element and S = 0: [0]B = R, accepted by the cofactorless equation. */
	memcpy(signature, identity, 32U);
	memset(signature + 32, 0, 32U);
	CHECK(cryc_ed25519_verify(signature, message, sizeof(message), identity) == 0);

	/* S = L (equal to L): the equation holds ([L]B is the neutral element) and only the range check refuses. */
	memcpy(signature + 32, order, 32U);
	CHECK(cryc_ed25519_verify(signature, message, sizeof(message), identity) == EBADMSG);

	/* S = L + 1: refused too; S = 1: [1]B is not the neutral element. */
	one = 1U;
	signature[32] = (uint8_t)(order[0] + one);
	CHECK(cryc_ed25519_verify(signature, message, sizeof(message), identity) == EBADMSG);
	memset(signature + 32, 0, 32U);
	signature[32] = 1U;
	CHECK(cryc_ed25519_verify(signature, message, sizeof(message), identity) == EBADMSG);

	/* S = L - 1 with R = -B: [L - 1]B = -B, accepted (the range check lets L - 1 through). */
	memcpy(signature, minus_base, 32U);
	memcpy(signature + 32, order, 32U);
	signature[32] = (uint8_t)(order[0] - one);
	CHECK(cryc_ed25519_verify(signature, message, sizeof(message), identity) == 0);

	/* The same S with R = B: -B is not B. */
	memcpy(signature, base, 32U);
	CHECK(cryc_ed25519_verify(signature, message, sizeof(message), identity) == EBADMSG);

	/* A S below L that is not L - 1 (the top byte 0x0f): the range check passes and the equation refuses. */
	memcpy(signature, minus_base, 32U);
	memcpy(signature + 32, order, 32U);
	signature[32] = (uint8_t)(order[0] - one);
	signature[63] = 0x0fU;
	CHECK(cryc_ed25519_verify(signature, message, sizeof(message), identity) == EBADMSG);

	/* R that is a valid point but not the canonical bytes (y = 1 written as p + 1) never matches. */
	decode_fixed(identity_plus_p_hex, signature, 32U);
	memset(signature + 32, 0, 32U);
	CHECK(cryc_ed25519_verify(signature, message, sizeof(message), identity) == EBADMSG);

	/* R = the neutral element with the sign bit set: x = 0 with x_0 = 1 does not decode. */
	decode_fixed(identity_sign_hex, signature, 32U);
	CHECK(cryc_ed25519_verify(signature, message, sizeof(message), identity) == EBADMSG);

	/* R with y at or above p: p and 2^255 - 1, with and without the sign bit. */
	decode_fixed(y_equals_p_hex, signature, 32U);
	CHECK(cryc_ed25519_verify(signature, message, sizeof(message), identity) == EBADMSG);
	decode_fixed(y_all_ones_hex, signature, 32U);
	CHECK(cryc_ed25519_verify(signature, message, sizeof(message), identity) == EBADMSG);
	signature[31] |= 0x80U;
	CHECK(cryc_ed25519_verify(signature, message, sizeof(message), identity) == EBADMSG);

	/* Keys that do not decode, with R = neutral element and S = 0 (which a lax decoder would take). */
	memcpy(signature, identity, 32U);
	memset(signature + 32, 0, 32U);
	decode_fixed(identity_plus_p_hex, key, sizeof(key));
	CHECK(cryc_ed25519_verify(signature, message, sizeof(message), key) == EBADMSG);
	decode_fixed(identity_sign_hex, key, sizeof(key));
	CHECK(cryc_ed25519_verify(signature, message, sizeof(message), key) == EBADMSG);
	decode_fixed(y_equals_p_hex, key, sizeof(key));
	CHECK(cryc_ed25519_verify(signature, message, sizeof(message), key) == EBADMSG);

	/* y = p read as y = 0 is a point of order 4 that would verify for every k divisible by 4: tried over 64 messages. */
	refused = 0;
	for (index = 0; index < 64U; index++) {
		message[0] = (uint8_t)index;
		error = cryc_ed25519_verify(signature, message, 1U, key);
		if (error == EBADMSG)
			refused++;
	}

	/* Every message was refused. */
	CHECK(refused == 64U);
	decode_fixed(y_all_ones_hex, key, sizeof(key));
	CHECK(cryc_ed25519_verify(signature, message, sizeof(message), key) == EBADMSG);
	decode_fixed(y_minus_one_sign_hex, key, sizeof(key));
	CHECK(cryc_ed25519_verify(signature, message, sizeof(message), key) == EBADMSG);

	/* The point (0, -1) of order 2 decodes: with R = neutral element and S = 0 it verifies for an even k and not for an odd one, so some message gives each. */
	decode_fixed(y_minus_one_hex, key, sizeof(key));
	accepted = 0;
	refused = 0;
	for (index = 0; index < 32U; index++) {
		message[0] = (uint8_t)index;
		error = cryc_ed25519_verify(signature, message, 1U, key);
		if (error == 0)
			accepted++;
		else
			refused++;
	}

	/* Both answers occurred, as the order of the point promises. */
	CHECK(accepted > 4U);
	CHECK(refused > 4U);
}

/* Checks outputs written only where documented and inputs left unchanged. */
static void
check_buffers(void)
{
	struct guarded {
		uint8_t before[16];
		uint8_t data[64];
		uint8_t after[16];
	} sign_out;
	struct guarded_key {
		uint8_t before[16];
		uint8_t data[32];
		uint8_t after[16];
	} key_out;
	uint8_t seed[32];
	uint8_t seed_copy[32];
	uint8_t message[50];
	uint8_t message_copy[50];
	uint8_t public_key[32];
	uint8_t public_copy[32];
	uint8_t sentinel[16];

	/* The reference copies of the inputs, which must stay as they are. */
	memset(sentinel, SENTINEL_BYTE, sizeof(sentinel));
	random_bytes(seed, sizeof(seed));
	random_bytes(message, sizeof(message));
	memcpy(seed_copy, seed, sizeof(seed));
	memcpy(message_copy, message, sizeof(message));

	/* The public key: 32 bytes, nothing around them. */
	memset(&key_out, SENTINEL_BYTE, sizeof(key_out));
	CHECK(cryc_ed25519_public(key_out.data, seed) == 0);
	CHECK_BYTES(key_out.before, sentinel, 16U);
	CHECK_BYTES(key_out.after, sentinel, 16U);
	CHECK_BYTES(seed, seed_copy, 32U);
	memcpy(public_key, key_out.data, sizeof(public_key));
	memcpy(public_copy, public_key, sizeof(public_copy));

	/* The signature: 64 bytes, nothing around them; it does not depend on what the buffer held. */
	memset(&sign_out, SENTINEL_BYTE, sizeof(sign_out));
	CHECK(cryc_ed25519_sign(sign_out.data, message, sizeof(message), seed) == 0);
	CHECK_BYTES(sign_out.before, sentinel, 16U);
	CHECK_BYTES(sign_out.after, sentinel, 16U);
	CHECK_BYTES(seed, seed_copy, 32U);
	CHECK_BYTES(message, message_copy, sizeof(message));
	CHECK(cryc_ed25519_verify(sign_out.data, message, sizeof(message), public_key) == 0);

	/* Verification leaves every input as it was. */
	CHECK_BYTES(sign_out.before, sentinel, 16U);
	CHECK_BYTES(message, message_copy, sizeof(message));
	CHECK_BYTES(public_key, public_copy, 32U);

	/* An empty message is given as NULL as well as a pointer. */
	CHECK(cryc_ed25519_sign(sign_out.data, NULL, 0U, seed) == 0);
	CHECK(cryc_ed25519_verify(sign_out.data, NULL, 0U, public_key) == 0);
	CHECK(cryc_ed25519_verify(sign_out.data, message, 0U, public_key) == 0);
}

/* Checks random inputs: nothing verifies, nothing crashes (the sanitizers watch), and the keys of random seeds decode. */
static void
check_random_inputs(void)
{
	uint8_t signature[64];
	uint8_t key[32];
	uint8_t message[40];
	uint8_t seed[32];
	uint8_t public_key[32];
	size_t size;
	unsigned int round;
	unsigned int refused;
	unsigned int odd;
	unsigned int even;
	int error;

	/* Random signature, key and message. */
	refused = 0;
	for (round = 0; round < 300U; round++) {
		random_bytes(signature, sizeof(signature));
		random_bytes(key, sizeof(key));
		size = random32() % sizeof(message);
		random_bytes(message, size);
		error = cryc_ed25519_verify(signature, message, size, key);
		if (error == EBADMSG)
			refused++;
	}

	/* Every random input was refused. */
	CHECK(refused == 300U);

	/* A random signature under a real key; a real signature of another message. */
	refused = 0;
	odd = 0;
	even = 0;
	for (round = 0; round < 100U; round++) {
		random_bytes(seed, sizeof(seed));
		CHECK(cryc_ed25519_public(public_key, seed) == 0);
		random_bytes(signature, sizeof(signature));
		signature[63] &= 0x0fU;
		size = random32() % sizeof(message);
		random_bytes(message, size);
		error = cryc_ed25519_verify(signature, message, size, public_key);
		if (error == EBADMSG)
			refused++;
		if ((public_key[31] & 0x80U) != 0U)
			odd++;
		else
			even++;
	}

	/* Every random signature was refused. */
	CHECK(refused == 100U);

	/* Both signs of x occur among the keys. */
	CHECK(odd > 20U);
	CHECK(even > 20U);
}

/* Runs the checks. */
int
main(void)
{
	/* The groups. */
	check_known();
	check_made();
	check_wycheproof();
	check_round_trips();
	check_long_message();
	check_malleability();
	check_edges();
	check_buffers();
	check_random_inputs();
	check_tampering();

	/* The result. */
	return check_finish("test-ed25519");
}
