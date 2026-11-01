/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the public EVP_PKEY functions (api-pkey.c): raw Ed25519 and
 * X25519 keys, EVP_DigestSign and EVP_DigestVerify with Ed25519,
 * EVP_PKEY_derive with X25519, and key generation, encapsulation and
 * decapsulation with ML-KEM-768.
 *
 * Expected values: the test vectors of RFC 8032 7.1 and the signatures of
 * the illustration of RFC 8032 6 through the EVP interface; the examples
 * of RFC 7748 5.2 (two vectors and the iterated test, 1 and 1,000
 * iterations) and 6.1 through EVP_PKEY_derive; the Wycheproof vectors of
 * ed25519_test, x25519_test (zero results must fail) and, for ML-KEM-768,
 * mlkem_768_test (a key made from the seed has the published ek and
 * decapsulates c to K), mlkem_768_encaps_test (an ek that is not reduced
 * or has another length is refused) and mlkem_768_semi_expanded_decaps_test
 * (a decapsulation key put into a key object).  A key object whose
 * private form is not made by the public interface (ML-KEM-768 from a
 * seed or a dk) is made by hand from the structure of cryc-api.h, with
 * the private form in memory EVP_PKEY_free releases.  Every refusal is
 * checked for the one error it queues; every success for an empty queue;
 * a failed call is checked to leave the objects and the output buffers
 * as they were.  The reference counts are read from the key objects, and
 * several threads take and drop references to one key.
 */

#include <errno.h>

#include "check.h"
#include "vecfile.h"

#include <compat/crypto/err.h>
#include <compat/crypto/evp.h>

#include "userland/base/libcrypto-compat/cryc-api.h"
#include "userland/base/libcrypto-compat/cryc-err.h"
#include "userland/base/libcrypto-compat/cryc-ed25519.h"
#include "userland/base/libcrypto-compat/cryc-mlkem.h"
#include "userland/base/libcrypto-compat/cryc-x25519.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

/* A byte that fills a buffer to show later that nothing was written to it. */
#define SENTINEL_BYTE		0x5aU

/* The largest message the Wycheproof Ed25519 records carry. */
#define MESSAGE_BYTES		4096U

/* The threads that take and drop references to one key together, and the rounds each makes. */
#define PARALLEL_THREADS	4U
#define THREAD_ROUNDS		5000U

/* The reasons the implementation queues (the header's constants, spelled for each condition). */
#define R_NULL		CRYC_R_PASSED_NULL_PARAMETER
#define R_ARGUMENT	CRYC_R_PASSED_INVALID_ARGUMENT
#define R_ALGORITHM	CRYC_R_EVP_UNSUPPORTED_ALGORITHM
#define R_NOT_SET_UP	CRYC_R_EVP_OPERATION_NOT_INITIALIZED
#define R_KEY_LENGTH	CRYC_R_EVP_INVALID_KEY_LENGTH
#define R_TOO_SMALL	CRYC_R_EVP_BUFFER_TOO_SMALL
#define R_INVALID_KEY	CRYC_R_EVP_INVALID_KEY
#define R_DIFFERENT	CRYC_R_EVP_DIFFERENT_KEY_TYPES
#define R_UNSUPPORTED	CRYC_R_UNSUPPORTED

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

/* What a thread of the reference-count test is given: the key it shares and what it saw. */
struct thread_job {
	EVP_PKEY *key;
	unsigned ok;
};

/* Returns the next pseudo-random 32-bit value (xorshift32). */
static uint32_t
random32(void)
{
	/* Marsaglia's xorshift32 steps. */
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

/* Fills size bytes with pseudo-random bytes. */
static void
random_bytes(
	uint8_t *out,
	size_t size)
{
	size_t index;

	/* One byte at a time. */
	for (index = 0; index < size; index++)
		out[index] = (uint8_t)(random32() >> 11);
}

/* Fills out with the message of the illustration: byte i is (i * 7 + 3) mod 256. */
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

/* Decodes exactly size bytes of hex text into out and checks that the text had that length. */
static void
decode_fixed(
	const char *text,
	uint8_t *out,
	size_t size)
{
	size_t got;

	/* The decoder reports the byte count, or (size_t)-1. */
	got = hex_decode(text, out, size);
	CHECK(got == size);
}

/* Checks that the call queued exactly one error, of the EVP library with the given reason, and empties the queue; line names the call in a report. */
static void
check_refusal_at(
	unsigned reason,
	int line)
{
	unsigned long code;
	unsigned long second;

	/* The one error. */
	code = ERR_get_error();
	if (code != cryc_err_pack(CRYC_LIB_EVP, reason))
		printf("  refusal at line %d: error code %08lx, wanted reason %u\n", line, code, reason);
	CHECK(code == cryc_err_pack(CRYC_LIB_EVP, reason));
	second = ERR_get_error();
	CHECK(second == 0UL);
}

/* The same check for a call made outside the REFUSED macro. */
#define check_refusal(reason) check_refusal_at((reason), __LINE__)

/* Runs a call that must return the failure value and queue the one error of the reason. */
#define REFUSED(call, failure, reason) \
	do { \
		ERR_clear_error(); \
		CHECK((call) == (failure)); \
		check_refusal_at((reason), __LINE__); \
	} while (0)

/* Runs a call that must succeed (return 1) and queue nothing. */
#define OK(call) \
	do { \
		ERR_clear_error(); \
		CHECK((call) == 1); \
		CHECK(ERR_peek_error() == 0UL); \
	} while (0)

/* Makes an Ed25519 or X25519 key from a private form given in hex; the call must succeed. */
static EVP_PKEY *
private_key_from_hex(
	int type,
	const char *hex)
{
	uint8_t raw[32];
	EVP_PKEY *key;

	/* The decoded private form. */
	decode_fixed(hex, raw, sizeof(raw));

	/* The key. */
	ERR_clear_error();
	key = EVP_PKEY_new_raw_private_key(type, NULL, raw, sizeof(raw));
	CHECK(key != NULL);
	CHECK(ERR_peek_error() == 0UL);
	return key;
}

/* Makes a public key of a type from 32 bytes given in hex; the call must succeed. */
static EVP_PKEY *
public_key_from_hex(
	int type,
	const char *hex)
{
	uint8_t raw[32];
	EVP_PKEY *key;

	/* The decoded public form. */
	decode_fixed(hex, raw, sizeof(raw));

	/* The key. */
	ERR_clear_error();
	key = EVP_PKEY_new_raw_public_key(type, NULL, raw, sizeof(raw));
	CHECK(key != NULL);
	CHECK(ERR_peek_error() == 0UL);
	return key;
}

/*
 * Makes an ML-KEM-768 key object by hand from a decapsulation key (2400
 * bytes), the way a key made by the public interface looks: the private
 * form in allocated memory, the encapsulation key stored inside dk
 * (bytes 1152 to 2335, FIPS 203 Algorithm 16 line 20) as the public form.
 */
static EVP_PKEY *
mlkem_key_from_dk(
	const uint8_t *dk)
{
	EVP_PKEY *key;

	/* The object, with one reference. */
	key = calloc(1, sizeof(*key));
	CHECK(key != NULL);
	if (key == NULL)
		return NULL;
	key->private_key = malloc(CRYC_MLKEM768_DK);
	CHECK(key->private_key != NULL);
	if (key->private_key == NULL) {
		free(key);
		return NULL;
	}

	/* Fills it. */
	key->references = 1;
	key->type = CRYC_PKEY_MLKEM768;
	memcpy(key->public_key, dk + 1152, CRYC_MLKEM768_EK);
	key->public_size = CRYC_MLKEM768_EK;
	memcpy(key->private_key, dk, CRYC_MLKEM768_DK);
	key->private_size = CRYC_MLKEM768_DK;
	key->has_private = 1;
	return key;
}

/* Makes an ML-KEM-768 key object from the seeds d and z (64 bytes, d first) with the internal key generation. */
static EVP_PKEY *
mlkem_key_from_seed(
	const uint8_t *seed)
{
	static uint8_t ek[CRYC_MLKEM768_EK];
	static uint8_t dk[CRYC_MLKEM768_DK];

	/* The key pair of FIPS 203 Algorithm 16 (tested on its own by test-mlkem). */
	cryc_mlkem768_keygen_internal(ek, dk, seed, seed + 32);
	return mlkem_key_from_dk(dk);
}

/* Makes an ML-KEM-768 key pair with the public interface. */
static EVP_PKEY *
mlkem_generate(void)
{
	EVP_PKEY_CTX *generator;
	EVP_PKEY *pair;

	/* The context, the preparation and the generation. */
	pair = NULL;
	generator = EVP_PKEY_CTX_new_from_name(NULL, "ML-KEM-768", NULL);
	CHECK(generator != NULL);
	if (generator == NULL)
		return NULL;
	OK(EVP_PKEY_keygen_init(generator));
	OK(EVP_PKEY_keygen(generator, &pair));
	EVP_PKEY_CTX_free(generator);
	CHECK(pair != NULL);
	return pair;
}

/* ------------------------------------------------------------------ */
/* Raw keys                                                             */
/* ------------------------------------------------------------------ */

/* Checks the refusals and the round trips of the raw private and public key functions. */
static void
test_raw_keys(void)
{
	static const int bad_types[] = { 0, -1, 1, 2, 1033, 1035, 1086, 1088, 9999, 0x7fffffff };
	static const size_t bad_lengths[] = { 0U, 1U, 16U, 31U, 33U, 63U, 64U, 1184U, (size_t)-1 };
	static const int types[2] = { EVP_PKEY_ED25519, EVP_PKEY_X25519 };
	uint8_t raw[64];
	uint8_t out[80];
	uint8_t guard[80];
	uint8_t scalar[32];
	uint8_t expected[32];
	size_t length;
	size_t room;
	EVP_PKEY *key;
	EVP_PKEY *other;
	unsigned index;
	unsigned type_index;
	int fake_engine;

	random_bytes(raw, sizeof(raw));
	fake_engine = 0;

	/* Every refusal of the private key constructor queues the one error. */
	for (type_index = 0; type_index < 2U; type_index++) {
		REFUSED(EVP_PKEY_new_raw_private_key(types[type_index], (ENGINE *)&fake_engine, raw, 32U), NULL, R_ARGUMENT);
		REFUSED(EVP_PKEY_new_raw_private_key(types[type_index], NULL, NULL, 32U), NULL, R_NULL);
		REFUSED(EVP_PKEY_new_raw_private_key(types[type_index], NULL, NULL, 0U), NULL, R_NULL);
		for (index = 0; index < sizeof(bad_lengths) / sizeof(bad_lengths[0]); index++)
			REFUSED(EVP_PKEY_new_raw_private_key(types[type_index], NULL, raw, bad_lengths[index]), NULL, R_KEY_LENGTH);
	}
	for (index = 0; index < sizeof(bad_types) / sizeof(bad_types[0]); index++)
		REFUSED(EVP_PKEY_new_raw_private_key(bad_types[index], NULL, raw, 32U), NULL, R_ALGORITHM);

	/* Every refusal of the public key constructors. */
	for (type_index = 0; type_index < 2U; type_index++) {
		REFUSED(EVP_PKEY_new_raw_public_key(types[type_index], (ENGINE *)&fake_engine, raw, 32U), NULL, R_ARGUMENT);
		REFUSED(EVP_PKEY_new_raw_public_key(types[type_index], NULL, NULL, 32U), NULL, R_NULL);
		for (index = 0; index < sizeof(bad_lengths) / sizeof(bad_lengths[0]); index++)
			REFUSED(EVP_PKEY_new_raw_public_key(types[type_index], NULL, raw, bad_lengths[index]), NULL, R_KEY_LENGTH);
	}
	for (index = 0; index < sizeof(bad_types) / sizeof(bad_types[0]); index++)
		REFUSED(EVP_PKEY_new_raw_public_key(bad_types[index], NULL, raw, 32U), NULL, R_ALGORITHM);

	/* The named constructor: library context, property query, names, lengths. */
	REFUSED(EVP_PKEY_new_raw_public_key_ex((OSSL_LIB_CTX *)&fake_engine, "ED25519", NULL, raw, 32U), NULL, R_ARGUMENT);
	REFUSED(EVP_PKEY_new_raw_public_key_ex(NULL, "ED25519", "provider=default", raw, 32U), NULL, R_ARGUMENT);
	REFUSED(EVP_PKEY_new_raw_public_key_ex(NULL, NULL, NULL, raw, 32U), NULL, R_NULL);
	REFUSED(EVP_PKEY_new_raw_public_key_ex(NULL, "ED25519", NULL, NULL, 32U), NULL, R_NULL);
	REFUSED(EVP_PKEY_new_raw_public_key_ex(NULL, "RSA", NULL, raw, 32U), NULL, R_ALGORITHM);
	REFUSED(EVP_PKEY_new_raw_public_key_ex(NULL, "", NULL, raw, 32U), NULL, R_ALGORITHM);
	REFUSED(EVP_PKEY_new_raw_public_key_ex(NULL, "ML-KEM-512", NULL, raw, 32U), NULL, R_ALGORITHM);
	REFUSED(EVP_PKEY_new_raw_public_key_ex(NULL, "ML-KEM-768 ", NULL, raw, 1184U), NULL, R_ALGORITHM);
	REFUSED(EVP_PKEY_new_raw_public_key_ex(NULL, "X25519X", NULL, raw, 32U), NULL, R_ALGORITHM);
	REFUSED(EVP_PKEY_new_raw_public_key_ex(NULL, "X2551", NULL, raw, 32U), NULL, R_ALGORITHM);
	REFUSED(EVP_PKEY_new_raw_public_key_ex(NULL, "ED25519", NULL, raw, 31U), NULL, R_KEY_LENGTH);
	REFUSED(EVP_PKEY_new_raw_public_key_ex(NULL, "X25519", NULL, raw, 33U), NULL, R_KEY_LENGTH);
	REFUSED(EVP_PKEY_new_raw_public_key_ex(NULL, "ML-KEM-768", NULL, raw, 32U), NULL, R_KEY_LENGTH);
	REFUSED(EVP_PKEY_new_raw_public_key_ex(NULL, "ED25519", NULL, raw, 1184U), NULL, R_KEY_LENGTH);

	/* The names are matched without regard to the case of their letters. */
	key = EVP_PKEY_new_raw_public_key_ex(NULL, "ed25519", NULL, raw, 32U);
	CHECK(key != NULL && key->type == CRYC_PKEY_ED25519);
	EVP_PKEY_free(key);
	key = EVP_PKEY_new_raw_public_key_ex(NULL, "x25519", NULL, raw, 32U);
	CHECK(key != NULL && key->type == CRYC_PKEY_X25519);
	EVP_PKEY_free(key);
	key = EVP_PKEY_new_raw_public_key_ex(NULL, "Ed25519", NULL, raw, 32U);
	CHECK(key != NULL && key->type == CRYC_PKEY_ED25519);
	EVP_PKEY_free(key);
	CHECK(ERR_peek_error() == 0UL);

	/* Ed25519: a private key, its public key (RFC 8032 7.1 TEST 1), the exports. */
	key = private_key_from_hex(EVP_PKEY_ED25519, "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
	CHECK(key != NULL);
	if (key != NULL) {
		CHECK(key->type == CRYC_PKEY_ED25519 && key->has_private == 1 && key->references == 1);
		CHECK(key->public_size == 32U && key->private_size == 32U);

		/* The size queries. */
		length = 0U;
		OK(EVP_PKEY_get_raw_public_key(key, NULL, &length));
		CHECK(length == 32U);
		length = 0U;
		OK(EVP_PKEY_get_raw_private_key(key, NULL, &length));
		CHECK(length == 32U);

		/* The exact room, and more room (the length becomes 32 and nothing beyond it is written). */
		decode_fixed("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", expected, 32U);
		memset(out, SENTINEL_BYTE, sizeof(out));
		length = 32U;
		OK(EVP_PKEY_get_raw_public_key(key, out, &length));
		CHECK(length == 32U);
		CHECK_BYTES(out, expected, 32U);
		memset(guard, SENTINEL_BYTE, sizeof(guard));
		CHECK_BYTES(out + 32, guard, sizeof(out) - 32U);
		memset(out, SENTINEL_BYTE, sizeof(out));
		length = 80U;
		OK(EVP_PKEY_get_raw_public_key(key, out, &length));
		CHECK(length == 32U);
		CHECK_BYTES(out, expected, 32U);
		CHECK_BYTES(out + 32, guard, sizeof(out) - 32U);
		decode_fixed("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", expected, 32U);
		memset(out, SENTINEL_BYTE, sizeof(out));
		length = 80U;
		OK(EVP_PKEY_get_raw_private_key(key, out, &length));
		CHECK(length == 32U);
		CHECK_BYTES(out, expected, 32U);
		CHECK_BYTES(out + 32, guard, sizeof(out) - 32U);

		/* A room that is too small is refused for every size below 32, with the buffer and the length as they were. */
		for (room = 0; room < 32U; room++) {
			memset(out, SENTINEL_BYTE, sizeof(out));
			length = room;
			REFUSED(EVP_PKEY_get_raw_public_key(key, out, &length), 0, R_TOO_SMALL);
			CHECK(length == room);
			CHECK_BYTES(out, guard, sizeof(out));
			length = room;
			REFUSED(EVP_PKEY_get_raw_private_key(key, out, &length), 0, R_TOO_SMALL);
			CHECK(length == room);
			CHECK_BYTES(out, guard, sizeof(out));
		}

		/* Missing arguments. */
		length = 32U;
		REFUSED(EVP_PKEY_get_raw_public_key(NULL, out, &length), 0, R_NULL);
		REFUSED(EVP_PKEY_get_raw_public_key(key, out, NULL), 0, R_NULL);
		REFUSED(EVP_PKEY_get_raw_private_key(NULL, out, &length), 0, R_NULL);
		REFUSED(EVP_PKEY_get_raw_private_key(key, out, NULL), 0, R_NULL);
		EVP_PKEY_free(key);
	}

	/* X25519: the private form is kept as received, unclamped (a scalar with its low bits and top bit set). */
	memset(scalar, 0xff, sizeof(scalar));
	key = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, scalar, 32U);
	CHECK(key != NULL);
	if (key != NULL) {
		memset(out, 0, sizeof(out));
		length = 32U;
		OK(EVP_PKEY_get_raw_private_key(key, out, &length));
		CHECK(length == 32U);
		CHECK_BYTES(out, scalar, 32U);

		/* Its public key is X25519(scalar, 9), computed with the clamping (RFC 7748 5, 6.1), by the primitive. */
		cryc_x25519_public(expected, scalar);
		length = 32U;
		OK(EVP_PKEY_get_raw_public_key(key, out, &length));
		CHECK_BYTES(out, expected, 32U);
		EVP_PKEY_free(key);
	}

	/* RFC 7748 6.1: Alice's private key gives her public key. */
	key = private_key_from_hex(EVP_PKEY_X25519, "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
	CHECK(key != NULL);
	if (key != NULL) {
		decode_fixed("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", expected, 32U);
		length = 32U;
		OK(EVP_PKEY_get_raw_public_key(key, out, &length));
		CHECK_BYTES(out, expected, 32U);
		EVP_PKEY_free(key);
	}

	/* A public-only key: the raw public form round-trips (even a value that is no valid Ed25519 point or is not reduced), the private form is refused. */
	for (type_index = 0; type_index < 2U; type_index++) {
		for (index = 0; index < 20U; index++) {
			random_bytes(scalar, sizeof(scalar));
			if (index == 0U)
				memset(scalar, 0xff, sizeof(scalar));
			if (index == 1U)
				memset(scalar, 0, sizeof(scalar));
			ERR_clear_error();
			other = EVP_PKEY_new_raw_public_key(types[type_index], NULL, scalar, 32U);
			CHECK(other != NULL);
			CHECK(ERR_peek_error() == 0UL);
			if (other == NULL)
				continue;
			CHECK(other->has_private == 0 && other->references == 1);
			memset(out, SENTINEL_BYTE, sizeof(out));
			length = 32U;
			OK(EVP_PKEY_get_raw_public_key(other, out, &length));
			CHECK(length == 32U);
			CHECK_BYTES(out, scalar, 32U);
			memset(out, SENTINEL_BYTE, sizeof(out));
			length = 32U;
			REFUSED(EVP_PKEY_get_raw_private_key(other, out, &length), 0, R_INVALID_KEY);
			CHECK(length == 32U);
			CHECK_BYTES(out, guard, sizeof(out));
			length = 32U;
			REFUSED(EVP_PKEY_get_raw_private_key(other, NULL, &length), 0, R_INVALID_KEY);
			EVP_PKEY_free(other);
		}
	}

	/* The named constructor makes the same keys as the numbered one. */
	key = EVP_PKEY_new_raw_public_key_ex(NULL, "ED25519", NULL, raw, 32U);
	CHECK(key != NULL);
	if (key != NULL) {
		CHECK(key->type == CRYC_PKEY_ED25519 && key->has_private == 0);
		length = 32U;
		OK(EVP_PKEY_get_raw_public_key(key, out, &length));
		CHECK_BYTES(out, raw, 32U);
		EVP_PKEY_free(key);
	}
	key = EVP_PKEY_new_raw_public_key_ex(NULL, "X25519", NULL, raw, 32U);
	CHECK(key != NULL);
	if (key != NULL) {
		CHECK(key->type == CRYC_PKEY_X25519 && key->has_private == 0);
		EVP_PKEY_free(key);
	}

	/* Freeing NULL does nothing. */
	EVP_PKEY_free(NULL);
	CHECK(ERR_peek_error() == 0UL);
}

/* ------------------------------------------------------------------ */
/* Reference counts                                                     */
/* ------------------------------------------------------------------ */

/* Checks that every holder of a key (operation contexts, signing and verification contexts, a derivation's peer) owns one reference, taken and dropped at the right moments. */
static void
test_references(void)
{
	uint8_t seed[32];
	uint8_t raw[32];
	EVP_PKEY *key;
	EVP_PKEY *other;
	EVP_PKEY *peer;
	EVP_PKEY_CTX *ctx;
	EVP_PKEY_CTX *ctx2;
	EVP_MD_CTX *md;
	EVP_MD_CTX *md2;
	EVP_PKEY_CTX *pctx;

	random_bytes(seed, sizeof(seed));
	random_bytes(raw, sizeof(raw));

	/* A new key has one reference. */
	key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, seed, 32U);
	other = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, raw, 32U);
	CHECK(key != NULL && other != NULL);
	if (key == NULL || other == NULL) {
		EVP_PKEY_free(key);
		EVP_PKEY_free(other);
		return;
	}
	CHECK(key->references == 1 && other->references == 1);

	/* Operation contexts take a reference, whichever constructor, and give it back when freed. */
	ctx = EVP_PKEY_CTX_new(key, NULL);
	CHECK(ctx != NULL);
	CHECK(key->references == 2);
	ctx2 = EVP_PKEY_CTX_new_from_pkey(NULL, key, NULL);
	CHECK(ctx2 != NULL);
	CHECK(key->references == 3);
	EVP_PKEY_CTX_free(ctx);
	CHECK(key->references == 2);
	EVP_PKEY_CTX_free(ctx2);
	CHECK(key->references == 1);

	/* Refused constructors take no reference. */
	REFUSED(EVP_PKEY_CTX_new(key, (ENGINE *)&raw), NULL, R_ARGUMENT);
	REFUSED(EVP_PKEY_CTX_new_from_pkey((OSSL_LIB_CTX *)&raw, key, NULL), NULL, R_ARGUMENT);
	REFUSED(EVP_PKEY_CTX_new_from_pkey(NULL, key, "x"), NULL, R_ARGUMENT);
	REFUSED(EVP_PKEY_CTX_new(NULL, NULL), NULL, R_NULL);
	REFUSED(EVP_PKEY_CTX_new_from_pkey(NULL, NULL, NULL), NULL, R_NULL);
	CHECK(key->references == 1);

	/* Signing and verification contexts hold a reference until they are freed, restarted or reused as digests. */
	md = EVP_MD_CTX_new();
	md2 = EVP_MD_CTX_new();
	CHECK(md != NULL && md2 != NULL);
	if (md != NULL && md2 != NULL) {
		OK(EVP_DigestSignInit(md, NULL, NULL, NULL, key));
		CHECK(key->references == 2);
		CHECK(md->mode == CRYC_MD_SIGN && md->key == key && md->md == NULL);
		OK(EVP_DigestVerifyInit(md2, NULL, NULL, NULL, key));
		CHECK(key->references == 3);
		CHECK(md2->mode == CRYC_MD_VERIFY && md2->key == key);

		/* Starting again with the same key keeps one reference for the context. */
		OK(EVP_DigestSignInit(md, NULL, NULL, NULL, key));
		CHECK(key->references == 3);

		/* Starting with another key moves the reference. */
		OK(EVP_DigestSignInit(md, NULL, NULL, NULL, other));
		CHECK(key->references == 2);
		CHECK(other->references == 2);
		CHECK(md->key == other);

		/* A refused start keeps what the context held. */
		REFUSED(EVP_DigestSignInit(md, NULL, EVP_sha256(), NULL, key), 0, R_ARGUMENT);
		CHECK(key->references == 2 && other->references == 2 && md->key == other && md->mode == CRYC_MD_SIGN);

		/* Freeing the contexts drops the references. */
		EVP_MD_CTX_free(md);
		CHECK(other->references == 1);
		EVP_MD_CTX_free(md2);
		CHECK(key->references == 1);
	} else {
		EVP_MD_CTX_free(md);
		EVP_MD_CTX_free(md2);
	}

	/* The pctx argument: left alone by a refusal, set to NULL by a success. */
	md = EVP_MD_CTX_new();
	CHECK(md != NULL);
	if (md != NULL) {
		pctx = (EVP_PKEY_CTX *)&raw;
		REFUSED(EVP_DigestSignInit(md, &pctx, NULL, (ENGINE *)&raw, key), 0, R_ARGUMENT);
		CHECK(pctx == (EVP_PKEY_CTX *)&raw);
		OK(EVP_DigestSignInit(md, &pctx, NULL, NULL, key));
		CHECK(pctx == NULL);
		pctx = (EVP_PKEY_CTX *)&raw;
		OK(EVP_DigestVerifyInit(md, &pctx, NULL, NULL, key));
		CHECK(pctx == NULL);
		CHECK(key->references == 2);
		EVP_MD_CTX_free(md);
		CHECK(key->references == 1);
	}

	/* A derivation's peer is held by the context until it is replaced, re-initialised or freed. */
	EVP_PKEY_free(key);
	EVP_PKEY_free(other);
	key = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, seed, 32U);
	peer = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, raw, 32U);
	other = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, seed, 32U);
	CHECK(key != NULL && peer != NULL && other != NULL);
	if (key != NULL && peer != NULL && other != NULL) {
		ctx = EVP_PKEY_CTX_new(key, NULL);
		CHECK(ctx != NULL);
		if (ctx != NULL) {
			OK(EVP_PKEY_derive_init(ctx));
			OK(EVP_PKEY_derive_set_peer(ctx, peer));
			CHECK(peer->references == 2);

			/* The same peer again changes nothing. */
			OK(EVP_PKEY_derive_set_peer(ctx, peer));
			CHECK(peer->references == 2);

			/* Another peer moves the reference. */
			OK(EVP_PKEY_derive_set_peer(ctx, other));
			CHECK(peer->references == 1 && other->references == 2);

			/* A refused peer keeps the old one. */
			REFUSED(EVP_PKEY_derive_set_peer(ctx, NULL), 0, R_NULL);
			CHECK(other->references == 2 && ctx->peer == other);

			/* Initialising again lets the peer go. */
			OK(EVP_PKEY_derive_init(ctx));
			CHECK(other->references == 1 && ctx->peer == NULL);
			OK(EVP_PKEY_derive_set_peer(ctx, peer));
			CHECK(peer->references == 2);

			/* Freeing the context drops the key and the peer. */
			CHECK(key->references == 2);
			EVP_PKEY_CTX_free(ctx);
			CHECK(key->references == 1 && peer->references == 1);
		}
	}
	EVP_PKEY_free(key);
	EVP_PKEY_free(peer);
	EVP_PKEY_free(other);

	/* A key freed with a reference left stays alive: the context's reference is the last one. */
	key = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, seed, 32U);
	CHECK(key != NULL);
	if (key != NULL) {
		ctx = EVP_PKEY_CTX_new(key, NULL);
		CHECK(ctx != NULL);
		EVP_PKEY_free(key);
		if (ctx != NULL) {
			CHECK(ctx->key == key && key->references == 1);
			OK(EVP_PKEY_derive_init(ctx));
			EVP_PKEY_CTX_free(ctx);
		}
	}

	/* A context for key generation holds no key; freeing NULL does nothing. */
	ctx = EVP_PKEY_CTX_new_from_name(NULL, "ML-KEM-768", NULL);
	CHECK(ctx != NULL);
	if (ctx != NULL)
		CHECK(ctx->key == NULL && ctx->peer == NULL && ctx->operation == CRYC_OP_NONE && ctx->keygen_type == CRYC_PKEY_MLKEM768);
	EVP_PKEY_CTX_free(ctx);
	EVP_PKEY_CTX_free(NULL);
	CHECK(ERR_peek_error() == 0UL);
}

/* ------------------------------------------------------------------ */
/* Threads                                                              */
/* ------------------------------------------------------------------ */

/* What a thread does: takes a reference to the shared key through a context and a signing context, and drops them, many times. */
static void
thread_body(
	struct thread_job *job)
{
	EVP_PKEY_CTX *ctx;
	EVP_MD_CTX *md;
	unsigned round;

	/* Many rounds of take and drop. */
	job->ok = 1;
	for (round = 0; round < THREAD_ROUNDS; round++) {
		ctx = EVP_PKEY_CTX_new(job->key, NULL);
		md = EVP_MD_CTX_new();
		if (ctx == NULL || md == NULL)
			job->ok = 0;
		if (md != NULL && EVP_DigestVerifyInit(md, NULL, NULL, NULL, job->key) != 1)
			job->ok = 0;
		EVP_PKEY_CTX_free(ctx);
		EVP_MD_CTX_free(md);
	}
}

#ifdef _WIN32
/* The thread function of Windows. */
static DWORD WINAPI
thread_entry(
	LPVOID argument)
{
	/* Runs the body. */
	thread_body((struct thread_job *)argument);

	/* Succeeded. */
	return 0;
}
#else
/* The thread function of POSIX. */
static void *
thread_entry(
	void *argument)
{
	/* Runs the body. */
	thread_body((struct thread_job *)argument);

	/* Succeeded. */
	return NULL;
}
#endif

/* Checks that references taken and dropped by several threads together balance, so the key is neither freed early nor kept. */
static void
test_threads(void)
{
	struct thread_job jobs[PARALLEL_THREADS];
	uint8_t seed[32];
	EVP_PKEY *key;
	unsigned index;
#ifdef _WIN32
	HANDLE handles[PARALLEL_THREADS];
#else
	pthread_t handles[PARALLEL_THREADS];
#endif

	/* One key shared by all. */
	random_bytes(seed, sizeof(seed));
	key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, seed, 32U);
	CHECK(key != NULL);
	if (key == NULL)
		return;

	/* Starts the threads. */
	for (index = 0; index < PARALLEL_THREADS; index++) {
		jobs[index].key = key;
		jobs[index].ok = 0;
#ifdef _WIN32
		handles[index] = CreateThread(NULL, 0, thread_entry, &jobs[index], 0, NULL);
		CHECK(handles[index] != NULL);
#else
		CHECK(pthread_create(&handles[index], NULL, thread_entry, &jobs[index]) == 0);
#endif
	}

	/* Waits for them. */
	for (index = 0; index < PARALLEL_THREADS; index++) {
#ifdef _WIN32
		WaitForSingleObject(handles[index], INFINITE);
		CloseHandle(handles[index]);
#else
		pthread_join(handles[index], NULL);
#endif
		CHECK(jobs[index].ok == 1);
	}

	/* Every reference was dropped: only the test's remains. */
	CHECK(key->references == 1);
	EVP_PKEY_free(key);
}

/* ------------------------------------------------------------------ */
/* Ed25519                                                              */
/* ------------------------------------------------------------------ */

/*
 * Signs a message through EVP_DigestSign with a key made from the seed,
 * checks the signature against the expected one (when given) and that a
 * verification context built from the public key accepts it, then that
 * the verification fails for a signature with any one byte changed
 * (every byte position, with a rotating bit) and for a changed message.
 */
static void
check_sign_verify(
	EVP_PKEY *private_key,
	EVP_PKEY *public_key,
	const uint8_t *message,
	size_t message_size,
	const uint8_t *expected_signature)
{
	uint8_t signature[80];
	uint8_t changed[64];
	uint8_t other_message[32];
	size_t length;
	size_t position;
	EVP_MD_CTX *sign_ctx;
	EVP_MD_CTX *verify_ctx;
	int status;

	/* The contexts. */
	sign_ctx = EVP_MD_CTX_new();
	verify_ctx = EVP_MD_CTX_new();
	CHECK(sign_ctx != NULL && verify_ctx != NULL);
	if (sign_ctx == NULL || verify_ctx == NULL) {
		EVP_MD_CTX_free(sign_ctx);
		EVP_MD_CTX_free(verify_ctx);
		return;
	}
	OK(EVP_DigestSignInit(sign_ctx, NULL, NULL, NULL, private_key));
	OK(EVP_DigestVerifyInit(verify_ctx, NULL, NULL, NULL, public_key));

	/* The signature: 64 bytes, the length reported, nothing written beyond them. */
	memset(signature, SENTINEL_BYTE, sizeof(signature));
	length = 80U;
	OK(EVP_DigestSign(sign_ctx, signature, &length, message, message_size));
	CHECK(length == 64U);
	for (position = 64U; position < sizeof(signature); position++)
		CHECK(signature[position] == SENTINEL_BYTE);
	if (expected_signature != NULL)
		CHECK_BYTES(signature, expected_signature, 64U);

	/* It verifies, also when asked again (the context is not used up). */
	ERR_clear_error();
	status = EVP_DigestVerify(verify_ctx, signature, 64U, message, message_size);
	CHECK(status == 1);
	status = EVP_DigestVerify(verify_ctx, signature, 64U, message, message_size);
	CHECK(status == 1);
	CHECK(ERR_peek_error() == 0UL);

	/* A change to any one byte of the signature makes the verification fail without an error. */
	for (position = 0; position < 64U; position++) {
		memcpy(changed, signature, 64U);
		changed[position] ^= (uint8_t)(1U << (position % 8U));
		status = EVP_DigestVerify(verify_ctx, changed, 64U, message, message_size);
		CHECK(status == 0);
	}
	CHECK(ERR_peek_error() == 0UL);

	/* A different message fails too: the last byte changed, a byte appended, one removed. */
	if (message_size > 0U && message_size <= sizeof(other_message)) {
		memcpy(other_message, message, message_size);
		other_message[message_size - 1U] ^= 0x80U;
		status = EVP_DigestVerify(verify_ctx, signature, 64U, other_message, message_size);
		CHECK(status == 0);
		status = EVP_DigestVerify(verify_ctx, signature, 64U, message, message_size - 1U);
		CHECK(status == 0);
	}
	if (message_size == 0U) {
		status = EVP_DigestVerify(verify_ctx, signature, 64U, (const unsigned char *)"\0", 1U);
		CHECK(status == 0);
	}
	CHECK(ERR_peek_error() == 0UL);

	/* The signing context verifies nothing and the verification context signs nothing. */
	REFUSED(EVP_DigestVerify(sign_ctx, signature, 64U, message, message_size), -1, R_NOT_SET_UP);
	length = 80U;
	REFUSED(EVP_DigestSign(verify_ctx, signature, &length, message, message_size), 0, R_NOT_SET_UP);
	EVP_MD_CTX_free(sign_ctx);
	EVP_MD_CTX_free(verify_ctx);
}

/* Checks signing and verification with the vectors of RFC 8032 7.1 and the signatures of the illustration of RFC 8032 6. */
static void
test_ed25519_vectors(void)
{
	static uint8_t message[MESSAGE_BYTES];
	uint8_t seed[32];
	uint8_t public_form[32];
	uint8_t signature[64];
	uint8_t out[32];
	size_t length;
	size_t size;
	unsigned index;
	EVP_PKEY *private_key;
	EVP_PKEY *public_key;

	/* The five vectors of RFC 8032 7.1. */
	for (index = 0; index < sizeof(rfc_vectors) / sizeof(rfc_vectors[0]); index++) {
		decode_fixed(rfc_vectors[index].seed, seed, sizeof(seed));
		decode_fixed(rfc_vectors[index].public_key, public_form, sizeof(public_form));
		decode_fixed(rfc_vectors[index].signature, signature, sizeof(signature));
		size = hex_decode(rfc_vectors[index].message, message, sizeof(message));
		CHECK(size != (size_t)-1);
		if (size == (size_t)-1)
			continue;
		private_key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, seed, 32U);
		public_key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, public_form, 32U);
		CHECK(private_key != NULL && public_key != NULL);
		if (private_key != NULL && public_key != NULL) {
			/* The public key computed from the seed is the vector's. */
			length = 32U;
			OK(EVP_PKEY_get_raw_public_key(private_key, out, &length));
			CHECK_BYTES(out, public_form, 32U);
			check_sign_verify(private_key, public_key, message, size, signature);

			/* The key made from the seed verifies as well (a private key holds the public one). */
			check_sign_verify(private_key, private_key, message, size, signature);
		}
		EVP_PKEY_free(private_key);
		EVP_PKEY_free(public_key);
	}

	/* The signatures of the illustration of RFC 8032 6 over messages of 0 to 1000 bytes. */
	for (index = 0; index < sizeof(made_vectors) / sizeof(made_vectors[0]); index++) {
		decode_fixed(made_vectors[index].seed, seed, sizeof(seed));
		decode_fixed(made_vectors[index].public_key, public_form, sizeof(public_form));
		decode_fixed(made_vectors[index].signature, signature, sizeof(signature));
		pattern_message(message, made_vectors[index].length);
		private_key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, seed, 32U);
		public_key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, public_form, 32U);
		CHECK(private_key != NULL && public_key != NULL);
		if (private_key != NULL && public_key != NULL)
			check_sign_verify(private_key, public_key, message, made_vectors[index].length, signature);
		EVP_PKEY_free(private_key);
		EVP_PKEY_free(public_key);
	}
}

/* Checks the Wycheproof Ed25519 verification vectors through EVP_DigestVerify (every signature size, valid, invalid and the acceptable ones). */
static void
test_ed25519_wycheproof(void)
{
	static uint8_t message[MESSAGE_BYTES];
	static uint8_t scratch[MESSAGE_BYTES];
	struct vec_file file;
	uint8_t public_form[32];
	uint8_t signature[64];
	EVP_PKEY *key;
	EVP_MD_CTX *ctx;
	const char *text;
	const char *result;
	unsigned long tests;
	unsigned long valid;
	unsigned long invalid;
	unsigned long acceptable;
	unsigned long accepted;
	unsigned long other_sizes;
	size_t public_size;
	size_t message_size;
	size_t signature_size;
	unsigned tc_id;
	unsigned index;
	int want_accept;
	int status;
	int have_key;

	if (vec_open(&file, "wycheproof-ed25519_test") != 0) {
		CHECK(0);
		return;
	}
	tests = 0;
	valid = 0;
	invalid = 0;
	acceptable = 0;
	accepted = 0;
	other_sizes = 0;
	have_key = 0;
	key = NULL;
	ctx = EVP_MD_CTX_new();
	CHECK(ctx != NULL);
	memset(public_form, 0, sizeof(public_form));

	/* The group's key is on the first record of the group; a record with a tcId is a test. */
	while (vec_next(&file)) {
		text = vec_get(&file, "group.publicKey.pk");
		if (text != NULL) {
			public_size = hex_decode(text, public_form, sizeof(public_form));
			CHECK(public_size == 32U);
			EVP_PKEY_free(key);
			key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, public_form, 32U);
			CHECK(key != NULL);
			have_key = 1;
		}
		if (vec_get(&file, "tcId") == NULL)
			continue;
		CHECK(have_key == 1);
		if (key == NULL || ctx == NULL)
			continue;
		tests++;
		result = vec_get(&file, "result");
		CHECK(result != NULL);
		if (result == NULL)
			continue;
		message_size = vec_hex(&file, "msg", message, sizeof(message));
		CHECK(message_size != (size_t)-1);
		signature_size = vec_hex(&file, "sig", scratch, sizeof(scratch));
		CHECK(signature_size != (size_t)-1);
		if (message_size == (size_t)-1 || signature_size == (size_t)-1)
			continue;
		OK(EVP_DigestVerifyInit(ctx, NULL, NULL, NULL, key));

		/* A signature of another size does not verify (and queues nothing). */
		if (signature_size != 64U) {
			CHECK(strcmp(result, "invalid") == 0);
			ERR_clear_error();
			status = EVP_DigestVerify(ctx, scratch, signature_size, message, message_size);
			CHECK(status == 0);
			CHECK(ERR_peek_error() == 0UL);
			other_sizes++;
			continue;
		}
		memcpy(signature, scratch, sizeof(signature));
		ERR_clear_error();
		status = EVP_DigestVerify(ctx, signature, 64U, message, message_size);
		CHECK(ERR_peek_error() == 0UL);
		if (strcmp(result, "valid") == 0) {
			CHECK(status == 1);
			valid++;
		} else if (strcmp(result, "invalid") == 0) {
			CHECK(status == 0);
			invalid++;
		} else {
			/* Either answer is allowed by the file; the equation of RFC 8032 5.1.7 step 3 with the full k accepts exactly the listed ones. */
			tc_id = (unsigned)strtoul(vec_get(&file, "tcId"), NULL, 10);
			want_accept = 0;
			for (index = 0; index < sizeof(accepted_ids) / sizeof(accepted_ids[0]); index++) {
				if (accepted_ids[index] == tc_id)
					want_accept = 1;
			}
			CHECK(status == want_accept);
			acceptable++;
			if (status == 1)
				accepted++;
		}
	}
	vec_close(&file);
	EVP_MD_CTX_free(ctx);
	EVP_PKEY_free(key);

	/* All 210 records were read; the counts match the file (117 valid, 64 invalid of which 12 with another signature size, 29 acceptable of which 8 verify). */
	CHECK(tests == 210U);
	CHECK(valid == 117U);
	CHECK(invalid + other_sizes == 64U);
	CHECK(other_sizes == 12U);
	CHECK(acceptable == 29U);
	CHECK(accepted == 8U);
	printf("wycheproof ed25519: %lu valid, %lu invalid, %lu other sizes, %lu acceptable of which %lu verified\n",
	    valid, invalid, other_sizes, acceptable, accepted);
}

/* Checks the refusals and the state rules of the signing and verification functions. */
static void
test_ed25519_states(void)
{
	static uint8_t message[300];
	uint8_t seed[32];
	uint8_t other_seed[32];
	uint8_t signature[100];
	uint8_t guard[100];
	uint8_t expected[64];
	size_t length;
	size_t room;
	size_t size;
	EVP_PKEY *private_key;
	EVP_PKEY *other_key;
	EVP_PKEY *public_key;
	EVP_PKEY *x25519_key;
	EVP_PKEY *mlkem_public;
	EVP_MD_CTX *ctx;
	EVP_MD_CTX *digest;
	uint8_t ek[CRYC_MLKEM768_EK];
	int fake_engine;
	int status;

	random_bytes(seed, sizeof(seed));
	random_bytes(other_seed, sizeof(other_seed));
	random_bytes(message, sizeof(message));
	memset(guard, SENTINEL_BYTE, sizeof(guard));
	fake_engine = 0;
	private_key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, seed, 32U);
	other_key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, other_seed, 32U);
	x25519_key = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, seed, 32U);
	length = 32U;
	public_key = NULL;
	CHECK(private_key != NULL && other_key != NULL && x25519_key != NULL);
	if (private_key == NULL || other_key == NULL || x25519_key == NULL) {
		EVP_PKEY_free(private_key);
		EVP_PKEY_free(other_key);
		EVP_PKEY_free(x25519_key);
		return;
	}
	(void)length;
	memcpy(expected, private_key->public_key, 32U);
	public_key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, expected, 32U);
	CHECK(public_key != NULL);
	ctx = EVP_MD_CTX_new();
	digest = EVP_MD_CTX_new();
	CHECK(ctx != NULL && digest != NULL && public_key != NULL);
	if (ctx == NULL || digest == NULL || public_key == NULL) {
		EVP_MD_CTX_free(ctx);
		EVP_MD_CTX_free(digest);
		EVP_PKEY_free(private_key);
		EVP_PKEY_free(other_key);
		EVP_PKEY_free(x25519_key);
		EVP_PKEY_free(public_key);
		return;
	}

	/* A context that was never started refuses everything: sign, verify (negative), as a digest context too. */
	length = 100U;
	REFUSED(EVP_DigestSign(ctx, signature, &length, message, 10U), 0, R_NOT_SET_UP);
	CHECK(length == 100U);
	REFUSED(EVP_DigestVerify(ctx, signature, 64U, message, 10U), -1, R_NOT_SET_UP);

	/* Refusals of the start functions. */
	REFUSED(EVP_DigestSignInit(NULL, NULL, NULL, NULL, private_key), 0, R_NULL);
	REFUSED(EVP_DigestSignInit(ctx, NULL, NULL, NULL, NULL), 0, R_NULL);
	REFUSED(EVP_DigestSignInit(ctx, NULL, NULL, (ENGINE *)&fake_engine, private_key), 0, R_ARGUMENT);
	REFUSED(EVP_DigestSignInit(ctx, NULL, EVP_sha256(), NULL, private_key), 0, R_ARGUMENT);
	REFUSED(EVP_DigestSignInit(ctx, NULL, NULL, NULL, x25519_key), 0, R_ALGORITHM);
	REFUSED(EVP_DigestSignInit(ctx, NULL, NULL, NULL, public_key), 0, R_INVALID_KEY);
	REFUSED(EVP_DigestVerifyInit(NULL, NULL, NULL, NULL, public_key), 0, R_NULL);
	REFUSED(EVP_DigestVerifyInit(ctx, NULL, NULL, NULL, NULL), 0, R_NULL);
	REFUSED(EVP_DigestVerifyInit(ctx, NULL, NULL, (ENGINE *)&fake_engine, public_key), 0, R_ARGUMENT);
	REFUSED(EVP_DigestVerifyInit(ctx, NULL, EVP_sha256(), NULL, public_key), 0, R_ARGUMENT);
	REFUSED(EVP_DigestVerifyInit(ctx, NULL, NULL, NULL, x25519_key), 0, R_ALGORITHM);
	CHECK(ctx->mode == CRYC_MD_NONE && ctx->key == NULL);

	/* A public key that is not an Ed25519 key of another type, an ML-KEM key: refused for both. */
	random_bytes(ek, sizeof(ek));
	mlkem_public = mlkem_generate();
	CHECK(mlkem_public != NULL);
	if (mlkem_public != NULL) {
		REFUSED(EVP_DigestSignInit(ctx, NULL, NULL, NULL, mlkem_public), 0, R_ALGORITHM);
		REFUSED(EVP_DigestVerifyInit(ctx, NULL, NULL, NULL, mlkem_public), 0, R_ALGORITHM);
		EVP_PKEY_free(mlkem_public);
	}

	/* A digest context started for SHA-256 gives up its digest when it is started for signing, and signs. */
	OK(EVP_DigestInit_ex(digest, EVP_sha256(), NULL));
	OK(EVP_DigestUpdate(digest, "abc", 3U));
	OK(EVP_DigestSignInit(digest, NULL, NULL, NULL, private_key));
	CHECK(digest->mode == CRYC_MD_SIGN && digest->md == NULL);
	REFUSED(EVP_DigestUpdate(digest, "abc", 3U), 0, R_NOT_SET_UP);
	length = 64U;
	OK(EVP_DigestSign(digest, signature, &length, message, 20U));
	CHECK(length == 64U);

	/* A signing context started again as a digest works as a digest. */
	OK(EVP_DigestInit_ex(digest, EVP_sha256(), NULL));
	CHECK(digest->mode == CRYC_MD_DIGEST && digest->key == NULL);
	length = 64U;
	REFUSED(EVP_DigestSign(digest, signature, &length, message, 20U), 0, R_NOT_SET_UP);

	/* The size query needs a signing context and writes nothing. */
	OK(EVP_DigestSignInit(ctx, NULL, NULL, NULL, private_key));
	memset(signature, SENTINEL_BYTE, sizeof(signature));
	length = 0U;
	OK(EVP_DigestSign(ctx, NULL, &length, message, 10U));
	CHECK(length == 64U);
	length = 0U;
	OK(EVP_DigestSign(ctx, NULL, &length, NULL, 0U));
	CHECK(length == 64U);
	CHECK_BYTES(signature, guard, sizeof(signature));

	/* Missing arguments and a buffer that is too small leave the buffer and the length as they were. */
	length = 64U;
	REFUSED(EVP_DigestSign(NULL, signature, &length, message, 10U), 0, R_NULL);
	REFUSED(EVP_DigestSign(ctx, signature, NULL, message, 10U), 0, R_NULL);
	REFUSED(EVP_DigestSign(ctx, NULL, NULL, message, 10U), 0, R_NULL);
	REFUSED(EVP_DigestSign(ctx, signature, &length, NULL, 5U), 0, R_NULL);
	CHECK_BYTES(signature, guard, sizeof(signature));
	for (room = 0; room < 64U; room++) {
		length = room;
		REFUSED(EVP_DigestSign(ctx, signature, &length, message, 10U), 0, R_TOO_SMALL);
		CHECK(length == room);
		CHECK_BYTES(signature, guard, sizeof(signature));
	}

	/* A message given as NULL with length 0 is the empty message: RFC 8032 7.1 TEST 1's signature. */
	EVP_PKEY_free(private_key);
	private_key = private_key_from_hex(EVP_PKEY_ED25519, "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
	CHECK(private_key != NULL);
	if (private_key != NULL) {
		decode_fixed("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
		    "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b", expected, 64U);
		OK(EVP_DigestSignInit(ctx, NULL, NULL, NULL, private_key));
		length = 64U;
		OK(EVP_DigestSign(ctx, signature, &length, NULL, 0U));
		CHECK_BYTES(signature, expected, 64U);
		length = 64U;
		OK(EVP_DigestSign(ctx, signature, &length, (const unsigned char *)"", 0U));
		CHECK_BYTES(signature, expected, 64U);
		decode_fixed("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", seed, 32U);
		EVP_PKEY_free(public_key);
		public_key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, seed, 32U);
		CHECK(public_key != NULL);
		OK(EVP_DigestVerifyInit(ctx, NULL, NULL, NULL, public_key));
		ERR_clear_error();
		status = EVP_DigestVerify(ctx, expected, 64U, NULL, 0U);
		CHECK(status == 1);
		CHECK(ERR_peek_error() == 0UL);
	}

	/* Signing in place over the message's own memory still gives the signature of the message. */
	memcpy(signature, message, 100U);
	OK(EVP_DigestSignInit(ctx, NULL, NULL, NULL, other_key));
	length = 100U;
	OK(EVP_DigestSign(ctx, signature, &length, signature, 64U));
	memcpy(expected, message, 64U);
	{
		uint8_t again[64];

		length = 64U;
		OK(EVP_DigestSign(ctx, again, &length, expected, 64U));
		CHECK_BYTES(signature, again, 64U);
	}

	/* The verification: a wrong key, every signature length from 0 to 130 (only 64 can verify), and the NULL combinations. */
	OK(EVP_DigestSignInit(ctx, NULL, NULL, NULL, other_key));
	length = 64U;
	OK(EVP_DigestSign(ctx, signature, &length, message, 77U));
	OK(EVP_DigestVerifyInit(digest, NULL, NULL, NULL, other_key));
	ERR_clear_error();
	CHECK(EVP_DigestVerify(digest, signature, 64U, message, 77U) == 1);
	OK(EVP_DigestVerifyInit(digest, NULL, NULL, NULL, public_key));
	CHECK(EVP_DigestVerify(digest, signature, 64U, message, 77U) == 0);
	for (size = 0; size <= 130U; size++) {
		uint8_t longer[131];

		random_bytes(longer, sizeof(longer));
		if (size >= 64U)
			memcpy(longer, signature, 64U);
		status = EVP_DigestVerify(digest, longer, size, message, 77U);
		CHECK(status == 0);
	}
	CHECK(EVP_DigestVerify(digest, signature, (size_t)-1, message, 77U) == 0);
	CHECK(ERR_peek_error() == 0UL);
	REFUSED(EVP_DigestVerify(NULL, signature, 64U, message, 77U), -1, R_NULL);
	REFUSED(EVP_DigestVerify(digest, NULL, 64U, message, 77U), 0, R_NULL);
	REFUSED(EVP_DigestVerify(digest, signature, 64U, NULL, 5U), 0, R_NULL);
	ERR_clear_error();
	CHECK(EVP_DigestVerify(digest, NULL, 0U, message, 5U) == 0);
	CHECK(ERR_peek_error() == 0UL);

	/* A public key that does not decode (y = p, not a point of the curve for a signature) fails without an error. */
	memset(expected, 0xff, 32U);
	EVP_PKEY_free(public_key);
	public_key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, expected, 32U);
	CHECK(public_key != NULL);
	if (public_key != NULL) {
		OK(EVP_DigestVerifyInit(digest, NULL, NULL, NULL, public_key));
		CHECK(EVP_DigestVerify(digest, signature, 64U, message, 77U) == 0);
		CHECK(ERR_peek_error() == 0UL);
	}

	EVP_MD_CTX_free(ctx);
	EVP_MD_CTX_free(digest);
	EVP_PKEY_free(private_key);
	EVP_PKEY_free(other_key);
	EVP_PKEY_free(x25519_key);
	EVP_PKEY_free(public_key);
}

/* Checks that the signature through EVP matches the primitive and verifies with it, for random seeds and messages of many sizes (including 0 and the block boundaries of SHA-512). */
static void
test_ed25519_random(void)
{
	static const size_t sizes[] = { 0U, 1U, 2U, 31U, 32U, 33U, 63U, 64U, 65U, 111U, 112U, 113U, 127U, 128U, 129U, 255U, 256U, 257U, 1000U, 4096U, 100000U };
	static uint8_t message[100008];
	uint8_t seed[32];
	uint8_t reference[64];
	uint8_t made[64];
	size_t length;
	unsigned round;
	unsigned index;
	EVP_PKEY *private_key;
	EVP_PKEY *public_key;
	EVP_MD_CTX *sign_ctx;
	EVP_MD_CTX *verify_ctx;
	int status;

	/* Messages and seeds from the generator. */
	random_bytes(message, sizeof(message));
	sign_ctx = EVP_MD_CTX_new();
	verify_ctx = EVP_MD_CTX_new();
	CHECK(sign_ctx != NULL && verify_ctx != NULL);
	if (sign_ctx == NULL || verify_ctx == NULL) {
		EVP_MD_CTX_free(sign_ctx);
		EVP_MD_CTX_free(verify_ctx);
		return;
	}
	for (round = 0; round < 3U; round++) {
		random_bytes(seed, sizeof(seed));
		private_key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, seed, 32U);
		CHECK(private_key != NULL);
		if (private_key == NULL)
			continue;
		public_key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, private_key->public_key, 32U);
		CHECK(public_key != NULL);
		if (public_key == NULL) {
			EVP_PKEY_free(private_key);
			continue;
		}
		OK(EVP_DigestSignInit(sign_ctx, NULL, NULL, NULL, private_key));
		OK(EVP_DigestVerifyInit(verify_ctx, NULL, NULL, NULL, public_key));
		for (index = 0; index < sizeof(sizes) / sizeof(sizes[0]); index++) {
			cryc_ed25519_sign(reference, message + round, sizes[index], seed);
			length = 64U;
			OK(EVP_DigestSign(sign_ctx, made, &length, message + round, sizes[index]));
			CHECK(length == 64U);
			CHECK_BYTES(made, reference, 64U);
			status = EVP_DigestVerify(verify_ctx, made, 64U, message + round, sizes[index]);
			CHECK(status == 1);
			CHECK(cryc_ed25519_verify(made, message + round, sizes[index], private_key->public_key) == 0);
		}
		EVP_PKEY_free(private_key);
		EVP_PKEY_free(public_key);
	}
	EVP_MD_CTX_free(sign_ctx);
	EVP_MD_CTX_free(verify_ctx);
}

/* ------------------------------------------------------------------ */
/* X25519                                                               */
/* ------------------------------------------------------------------ */

/* Derives with a private scalar and a peer's u-coordinate (hex) and checks the 32-byte result against the expected hex. */
static void
check_derive_known(
	const char *scalar_hex,
	const char *u_hex,
	const char *shared_hex)
{
	uint8_t shared[32];
	uint8_t expected[32];
	size_t length;
	EVP_PKEY *own;
	EVP_PKEY *peer;
	EVP_PKEY_CTX *ctx;

	/* The keys and the context. */
	own = private_key_from_hex(EVP_PKEY_X25519, scalar_hex);
	peer = public_key_from_hex(EVP_PKEY_X25519, u_hex);
	decode_fixed(shared_hex, expected, sizeof(expected));
	CHECK(own != NULL && peer != NULL);
	if (own == NULL || peer == NULL) {
		EVP_PKEY_free(own);
		EVP_PKEY_free(peer);
		return;
	}
	ctx = EVP_PKEY_CTX_new(own, NULL);
	CHECK(ctx != NULL);
	if (ctx != NULL) {
		OK(EVP_PKEY_derive_init(ctx));
		OK(EVP_PKEY_derive_set_peer(ctx, peer));
		memset(shared, SENTINEL_BYTE, sizeof(shared));
		length = 32U;
		OK(EVP_PKEY_derive(ctx, shared, &length));
		CHECK(length == 32U);
		CHECK_BYTES(shared, expected, 32U);
		EVP_PKEY_CTX_free(ctx);
	}
	EVP_PKEY_free(own);
	EVP_PKEY_free(peer);
}

/* Checks the examples of RFC 7748 5.2 and 6.1 through the EVP interface. */
static void
test_x25519_rfc(void)
{
	uint8_t k[32];
	uint8_t u[32];
	uint8_t out[32];
	uint8_t want[32];
	char scalar_hex[65];
	char u_hex[65];
	size_t length;
	unsigned iteration;
	unsigned index;
	EVP_PKEY *own;
	EVP_PKEY *peer;
	EVP_PKEY_CTX *ctx;
	EVP_PKEY *alice;
	EVP_PKEY *bob;

	/* Values referenced: RFC 7748 (January 2016) 5.2, X25519 test vectors 1 and 2. */
	check_derive_known("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4",
	    "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c",
	    "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");
	check_derive_known("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d",
	    "e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493",
	    "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957");

	/* Values referenced: RFC 7748 (January 2016) 5.2, the iterated test (k = u = 9) after 1 and after 1,000 iterations, each through new key objects and a context. */
	memset(k, 0, sizeof(k));
	k[0] = 9U;
	memcpy(u, k, sizeof(u));
	for (iteration = 1; iteration <= 1000U; iteration++) {
		own = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, k, 32U);
		peer = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, u, 32U);
		CHECK(own != NULL && peer != NULL);
		ctx = (own != NULL) ? EVP_PKEY_CTX_new(own, NULL) : NULL;
		CHECK(ctx != NULL);
		if (ctx == NULL || peer == NULL) {
			EVP_PKEY_CTX_free(ctx);
			EVP_PKEY_free(own);
			EVP_PKEY_free(peer);
			break;
		}
		OK(EVP_PKEY_derive_init(ctx));
		OK(EVP_PKEY_derive_set_peer(ctx, peer));
		length = 32U;
		OK(EVP_PKEY_derive(ctx, out, &length));
		CHECK(length == 32U);
		memcpy(u, k, sizeof(u));
		memcpy(k, out, sizeof(k));
		EVP_PKEY_CTX_free(ctx);
		EVP_PKEY_free(own);
		EVP_PKEY_free(peer);

		/* Values referenced: RFC 7748 (January 2016) 5.2, the value after 1 iteration. */
		if (iteration == 1U) {
			decode_fixed("422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079", want, sizeof(want));
			CHECK_BYTES(k, want, 32U);
		}
	}

	/* Values referenced: RFC 7748 (January 2016) 5.2, the value after 1,000 iterations. */
	decode_fixed("684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51", want, sizeof(want));
	CHECK_BYTES(k, want, 32U);

	/* Values referenced: RFC 7748 (January 2016) 6.1, Alice's and Bob's keys, their public keys and the shared secret K, from both sides. */
	alice = private_key_from_hex(EVP_PKEY_X25519, "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
	bob = private_key_from_hex(EVP_PKEY_X25519, "5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
	CHECK(alice != NULL && bob != NULL);
	if (alice != NULL && bob != NULL) {
		decode_fixed("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", want, sizeof(want));
		CHECK_BYTES(alice->public_key, want, 32U);
		decode_fixed("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f", want, sizeof(want));
		CHECK_BYTES(bob->public_key, want, 32U);
		for (index = 0; index < 32U; index++) {
			sprintf(scalar_hex + 2U * index, "%02x", (unsigned)alice->private_key[index]);
			sprintf(u_hex + 2U * index, "%02x", (unsigned)bob->public_key[index]);
		}
		check_derive_known(scalar_hex, u_hex, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
		for (index = 0; index < 32U; index++) {
			sprintf(scalar_hex + 2U * index, "%02x", (unsigned)bob->private_key[index]);
			sprintf(u_hex + 2U * index, "%02x", (unsigned)alice->public_key[index]);
		}
		check_derive_known(scalar_hex, u_hex, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
	}
	EVP_PKEY_free(alice);
	EVP_PKEY_free(bob);
}

/* Checks the Wycheproof X25519 vectors: the shared secret of every record, and a zero one fails with the output zeroed. */
static void
test_x25519_wycheproof(void)
{
	struct vec_file file;
	uint8_t scalar[32];
	uint8_t u[32];
	uint8_t want[32];
	uint8_t out[32];
	uint8_t zero[32];
	size_t length;
	size_t size;
	unsigned long zeros;
	unsigned long records;
	EVP_PKEY *own;
	EVP_PKEY *peer;
	EVP_PKEY_CTX *ctx;
	int status;

	if (vec_open(&file, "wycheproof-x25519_test") != 0) {
		CHECK(0);
		return;
	}
	memset(zero, 0, sizeof(zero));
	zeros = 0;
	while (vec_next(&file)) {
		/* The three fields of a record. */
		size = vec_hex(&file, "private", scalar, sizeof(scalar));
		CHECK(size == 32U);
		size = vec_hex(&file, "public", u, sizeof(u));
		CHECK(size == 32U);
		size = vec_hex(&file, "shared", want, sizeof(want));
		CHECK(size == 32U);
		own = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, scalar, 32U);
		peer = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, u, 32U);
		ctx = (own != NULL) ? EVP_PKEY_CTX_new(own, NULL) : NULL;
		CHECK(own != NULL && peer != NULL && ctx != NULL);
		if (own != NULL && peer != NULL && ctx != NULL) {
			OK(EVP_PKEY_derive_init(ctx));
			OK(EVP_PKEY_derive_set_peer(ctx, peer));
			memset(out, SENTINEL_BYTE, sizeof(out));
			length = 32U;
			ERR_clear_error();
			status = EVP_PKEY_derive(ctx, out, &length);
			if (memcmp(want, zero, 32U) == 0) {
				/* An all-zero secret fails, queues the one error and leaves zeros. */
				CHECK(status == 0);
				check_refusal(R_INVALID_KEY);
				CHECK_BYTES(out, zero, 32U);
				CHECK(length == 32U);
				zeros++;
			} else {
				CHECK(status == 1);
				CHECK(ERR_peek_error() == 0UL);
				CHECK(length == 32U);
				CHECK_BYTES(out, want, 32U);
			}
		}
		EVP_PKEY_CTX_free(ctx);
		EVP_PKEY_free(own);
		EVP_PKEY_free(peer);
	}
	records = file.records;
	vec_close(&file);

	/* All 518 records were read, and the file has zero-result cases. */
	CHECK(records == 518U);
	CHECK(zeros > 10U);
}

/* Checks the refusals and the state rules of the derivation functions, and random exchanges. */
static void
test_x25519_states(void)
{
	uint8_t seed[32];
	uint8_t raw[32];
	uint8_t shared[40];
	uint8_t guard[40];
	uint8_t reference[32];
	uint8_t other[32];
	size_t length;
	size_t room;
	unsigned round;
	EVP_PKEY *own;
	EVP_PKEY *peer;
	EVP_PKEY *public_only;
	EVP_PKEY *ed_key;
	EVP_PKEY *mlkem_key;
	EVP_PKEY_CTX *ctx;
	EVP_PKEY_CTX *generator;
	EVP_PKEY_CTX *other_ctx;

	random_bytes(seed, sizeof(seed));
	random_bytes(raw, sizeof(raw));
	memset(guard, SENTINEL_BYTE, sizeof(guard));
	own = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, seed, 32U);
	peer = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, raw, 32U);
	public_only = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, seed, 32U);
	ed_key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, seed, 32U);
	mlkem_key = mlkem_generate();
	CHECK(own != NULL && peer != NULL && public_only != NULL && ed_key != NULL && mlkem_key != NULL);
	if (own == NULL || peer == NULL || public_only == NULL || ed_key == NULL || mlkem_key == NULL) {
		EVP_PKEY_free(own);
		EVP_PKEY_free(peer);
		EVP_PKEY_free(public_only);
		EVP_PKEY_free(ed_key);
		EVP_PKEY_free(mlkem_key);
		return;
	}

	/* An operation context made for another purpose or a different key type refuses to derive. */
	ctx = EVP_PKEY_CTX_new(ed_key, NULL);
	CHECK(ctx != NULL);
	REFUSED(EVP_PKEY_derive_init(ctx), 0, R_ALGORITHM);
	CHECK(ctx->operation == CRYC_OP_NONE);
	EVP_PKEY_CTX_free(ctx);
	ctx = EVP_PKEY_CTX_new(mlkem_key, NULL);
	CHECK(ctx != NULL);
	REFUSED(EVP_PKEY_derive_init(ctx), 0, R_ALGORITHM);
	EVP_PKEY_CTX_free(ctx);
	ctx = EVP_PKEY_CTX_new(public_only, NULL);
	CHECK(ctx != NULL);
	REFUSED(EVP_PKEY_derive_init(ctx), 0, R_INVALID_KEY);
	CHECK(ctx->operation == CRYC_OP_NONE);
	EVP_PKEY_CTX_free(ctx);
	generator = EVP_PKEY_CTX_new_from_name(NULL, "ML-KEM-768", NULL);
	CHECK(generator != NULL);
	REFUSED(EVP_PKEY_derive_init(generator), 0, R_ALGORITHM);
	REFUSED(EVP_PKEY_derive_init(NULL), 0, R_NULL);
	EVP_PKEY_CTX_free(generator);

	/* Before the preparation: no peer can be set and nothing can be derived. */
	ctx = EVP_PKEY_CTX_new(own, NULL);
	CHECK(ctx != NULL);
	length = 32U;
	REFUSED(EVP_PKEY_derive_set_peer(ctx, peer), 0, R_NOT_SET_UP);
	REFUSED(EVP_PKEY_derive(ctx, shared, &length), 0, R_NOT_SET_UP);
	OK(EVP_PKEY_derive_init(ctx));
	CHECK(ctx->operation == CRYC_OP_DERIVE);

	/* Prepared but without a peer: nothing to derive (also the size query). */
	REFUSED(EVP_PKEY_derive(ctx, shared, &length), 0, R_NOT_SET_UP);
	REFUSED(EVP_PKEY_derive(ctx, NULL, &length), 0, R_NOT_SET_UP);

	/* Refused peers: missing, and of another type. */
	REFUSED(EVP_PKEY_derive_set_peer(NULL, peer), 0, R_NULL);
	REFUSED(EVP_PKEY_derive_set_peer(ctx, NULL), 0, R_NULL);
	REFUSED(EVP_PKEY_derive_set_peer(ctx, ed_key), 0, R_DIFFERENT);
	REFUSED(EVP_PKEY_derive_set_peer(ctx, mlkem_key), 0, R_DIFFERENT);
	CHECK(ctx->peer == NULL);
	OK(EVP_PKEY_derive_set_peer(ctx, peer));

	/* A public-only key of the same type is a fine peer. */
	OK(EVP_PKEY_derive_set_peer(ctx, public_only));
	OK(EVP_PKEY_derive_set_peer(ctx, peer));

	/* The size query writes nothing and does not need the room. */
	memset(shared, SENTINEL_BYTE, sizeof(shared));
	length = 0U;
	OK(EVP_PKEY_derive(ctx, NULL, &length));
	CHECK(length == 32U);
	CHECK_BYTES(shared, guard, sizeof(shared));

	/* Missing arguments and too little room leave the buffer and the length as they were. */
	REFUSED(EVP_PKEY_derive(NULL, shared, &length), 0, R_NULL);
	REFUSED(EVP_PKEY_derive(ctx, shared, NULL), 0, R_NULL);
	for (room = 0; room < 32U; room++) {
		length = room;
		REFUSED(EVP_PKEY_derive(ctx, shared, &length), 0, R_TOO_SMALL);
		CHECK(length == room);
		CHECK_BYTES(shared, guard, sizeof(shared));
	}

	/* The result with more room: 32 bytes written, the length 32, nothing beyond. */
	length = 40U;
	OK(EVP_PKEY_derive(ctx, shared, &length));
	CHECK(length == 32U);
	CHECK_BYTES(shared + 32, guard, 8U);
	cryc_x25519(reference, seed, raw);
	CHECK_BYTES(shared, reference, 32U);

	/* The same context derives again, and with a second peer. */
	memset(shared, 0, sizeof(shared));
	length = 32U;
	OK(EVP_PKEY_derive(ctx, shared, &length));
	CHECK_BYTES(shared, reference, 32U);
	EVP_PKEY_CTX_free(ctx);

	/* Random exchanges: both sides get the same secret, equal to the primitive's. */
	EVP_PKEY_free(own);
	EVP_PKEY_free(peer);
	for (round = 0; round < 200U; round++) {
		random_bytes(seed, sizeof(seed));
		random_bytes(raw, sizeof(raw));
		own = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, seed, 32U);
		peer = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, raw, 32U);
		CHECK(own != NULL && peer != NULL);
		if (own == NULL || peer == NULL) {
			EVP_PKEY_free(own);
			EVP_PKEY_free(peer);
			continue;
		}
		ctx = EVP_PKEY_CTX_new(own, NULL);
		other_ctx = EVP_PKEY_CTX_new(peer, NULL);
		CHECK(ctx != NULL && other_ctx != NULL);
		if (ctx != NULL && other_ctx != NULL) {
			OK(EVP_PKEY_derive_init(ctx));
			OK(EVP_PKEY_derive_set_peer(ctx, peer));
			OK(EVP_PKEY_derive_init(other_ctx));
			OK(EVP_PKEY_derive_set_peer(other_ctx, own));
			length = 32U;
			OK(EVP_PKEY_derive(ctx, reference, &length));
			length = 32U;
			OK(EVP_PKEY_derive(other_ctx, other, &length));
			CHECK_BYTES(reference, other, 32U);
			CHECK(cryc_x25519(shared, seed, peer->public_key) == 0);
			CHECK_BYTES(reference, shared, 32U);
		}
		EVP_PKEY_CTX_free(ctx);
		EVP_PKEY_CTX_free(other_ctx);
		EVP_PKEY_free(own);
		EVP_PKEY_free(peer);
	}

	/* Peers of small order: the all-zero point and the point 1 give an all-zero result, which fails with zeros written and one error. */
	memset(raw, 0, sizeof(raw));
	own = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, seed, 32U);
	peer = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, raw, 32U);
	ctx = (own != NULL) ? EVP_PKEY_CTX_new(own, NULL) : NULL;
	CHECK(own != NULL && peer != NULL && ctx != NULL);
	if (own != NULL && peer != NULL && ctx != NULL) {
		OK(EVP_PKEY_derive_init(ctx));
		OK(EVP_PKEY_derive_set_peer(ctx, peer));
		memset(shared, SENTINEL_BYTE, sizeof(shared));
		length = 40U;
		REFUSED(EVP_PKEY_derive(ctx, shared, &length), 0, R_INVALID_KEY);
		memset(reference, 0, sizeof(reference));
		CHECK_BYTES(shared, reference, 32U);
		CHECK_BYTES(shared + 32, guard, 8U);
		CHECK(length == 40U);
	}
	EVP_PKEY_CTX_free(ctx);
	EVP_PKEY_free(own);
	EVP_PKEY_free(peer);
	EVP_PKEY_free(public_only);
	EVP_PKEY_free(ed_key);
	EVP_PKEY_free(mlkem_key);
}

/* ------------------------------------------------------------------ */
/* ML-KEM-768                                                           */
/* ------------------------------------------------------------------ */

/*
 * Encapsulates to the public form ek through the EVP interface (key
 * imported by name, context from the key) and checks the ciphertext and
 * the shared key: 1088 and 32 bytes, nothing written beyond them, the
 * key decapsulates through the EVP interface with the key pair and, with
 * the primitive's decapsulation (independent of the EVP layer) with dk.
 */
static void
check_encapsulation(
	const uint8_t *ek,
	EVP_PKEY *pair,
	const uint8_t *dk)
{
	static uint8_t ciphertext[CRYC_MLKEM768_CT + 16U];
	uint8_t shared[48];
	uint8_t decapsulated[48];
	uint8_t reference[32];
	uint8_t guard[48];
	size_t ciphertext_size;
	size_t shared_size;
	size_t decapsulated_size;
	EVP_PKEY *public_key;
	EVP_PKEY_CTX *encapsulator;
	EVP_PKEY_CTX *decapsulator;

	/* The public key by name, and the encapsulation. */
	memset(guard, SENTINEL_BYTE, sizeof(guard));
	public_key = EVP_PKEY_new_raw_public_key_ex(NULL, "ML-KEM-768", NULL, ek, CRYC_MLKEM768_EK);
	CHECK(public_key != NULL);
	if (public_key == NULL)
		return;
	CHECK(public_key->type == CRYC_PKEY_MLKEM768 && public_key->has_private == 0 && public_key->public_size == CRYC_MLKEM768_EK);
	encapsulator = EVP_PKEY_CTX_new_from_pkey(NULL, public_key, NULL);
	CHECK(encapsulator != NULL);
	if (encapsulator == NULL) {
		EVP_PKEY_free(public_key);
		return;
	}
	OK(EVP_PKEY_encapsulate_init(encapsulator, NULL));
	memset(ciphertext, SENTINEL_BYTE, sizeof(ciphertext));
	memset(shared, SENTINEL_BYTE, sizeof(shared));
	ciphertext_size = sizeof(ciphertext);
	shared_size = sizeof(shared);
	OK(EVP_PKEY_encapsulate(encapsulator, ciphertext, &ciphertext_size, shared, &shared_size));
	CHECK(ciphertext_size == CRYC_MLKEM768_CT);
	CHECK(shared_size == CRYC_MLKEM_SHARED);
	CHECK_BYTES(ciphertext + CRYC_MLKEM768_CT, guard, 16U);
	CHECK_BYTES(shared + 32, guard, 16U);

	/* The primitive's decapsulation, which knows nothing of the EVP layer, finds the same key. */
	cryc_mlkem768_decaps(reference, ciphertext, dk);
	CHECK_BYTES(shared, reference, 32U);

	/* The EVP decapsulation too. */
	decapsulator = EVP_PKEY_CTX_new(pair, NULL);
	CHECK(decapsulator != NULL);
	if (decapsulator != NULL) {
		OK(EVP_PKEY_decapsulate_init(decapsulator, NULL));
		memset(decapsulated, SENTINEL_BYTE, sizeof(decapsulated));
		decapsulated_size = sizeof(decapsulated);
		OK(EVP_PKEY_decapsulate(decapsulator, decapsulated, &decapsulated_size, ciphertext, CRYC_MLKEM768_CT));
		CHECK(decapsulated_size == 32U);
		CHECK_BYTES(decapsulated, shared, 32U);
		CHECK_BYTES(decapsulated + 32, guard, 16U);
		EVP_PKEY_CTX_free(decapsulator);
	}
	EVP_PKEY_CTX_free(encapsulator);
	EVP_PKEY_free(public_key);
}

/* Checks the Wycheproof mlkem_768_test vectors: a key made from the seed has the published ek and decapsulates c to K, and a ciphertext of another size is refused. */
static void
test_mlkem_wycheproof_keys(void)
{
	struct vec_file file;
	uint8_t seed[64];
	uint8_t ek[1300];
	uint8_t c[1300];
	uint8_t key[64];
	uint8_t out[48];
	uint8_t guard[48];
	uint8_t made_ek[CRYC_MLKEM768_EK];
	size_t seed_size;
	size_t ek_size;
	size_t c_size;
	size_t key_size;
	size_t length;
	const char *result;
	unsigned long records;
	unsigned long valid;
	unsigned long wrong_ciphertext;
	EVP_PKEY *pair;
	EVP_PKEY_CTX *ctx;

	if (vec_open(&file, "wycheproof-mlkem_768_test") != 0) {
		CHECK(0);
		return;
	}
	memset(guard, SENTINEL_BYTE, sizeof(guard));
	records = 0;
	valid = 0;
	wrong_ciphertext = 0;
	while (vec_next(&file)) {
		result = vec_get(&file, "result");
		if (result == NULL)
			continue;
		records++;
		seed_size = vec_hex(&file, "seed", seed, sizeof(seed));
		ek_size = vec_hex(&file, "ek", ek, sizeof(ek));
		c_size = vec_hex(&file, "c", c, sizeof(c));
		key_size = vec_hex(&file, "K", key, sizeof(key));
		if (strcmp(result, "valid") == 0) {
			CHECK(seed_size == 64U && ek_size == 1184U && c_size == 1088U && key_size == 32U);
			if (seed_size != 64U || ek_size != 1184U || c_size != 1088U || key_size != 32U)
				continue;
			valid++;

			/* The key made from the seed has the published ek (read back through the interface). */
			pair = mlkem_key_from_seed(seed);
			CHECK(pair != NULL);
			if (pair == NULL)
				continue;
			length = sizeof(made_ek);
			OK(EVP_PKEY_get_raw_public_key(pair, made_ek, &length));
			CHECK(length == 1184U);
			CHECK_BYTES(made_ek, ek, 1184U);

			/* It decapsulates c to K (an implicit rejection included: the record's K is then the rejection key). */
			ctx = EVP_PKEY_CTX_new_from_pkey(NULL, pair, NULL);
			CHECK(ctx != NULL);
			if (ctx != NULL) {
				OK(EVP_PKEY_decapsulate_init(ctx, NULL));
				memset(out, SENTINEL_BYTE, sizeof(out));
				length = sizeof(out);
				OK(EVP_PKEY_decapsulate(ctx, out, &length, c, c_size));
				CHECK(length == 32U);
				CHECK_BYTES(out, key, 32U);
				CHECK_BYTES(out + 32, guard, 16U);
				EVP_PKEY_CTX_free(ctx);
			}

			/* The published ek is accepted by name, and an encapsulation to it round-trips. */
			check_encapsulation(ek, pair, pair->private_key);
			EVP_PKEY_free(pair);
		} else if (seed_size == 64U && ek_size == 1184U) {
			/* An invalid record with a good key: the ciphertext has the wrong size and is refused, with the buffer untouched. */
			pair = mlkem_key_from_seed(seed);
			CHECK(pair != NULL);
			if (pair == NULL)
				continue;
			CHECK(c_size != 1088U && c_size != (size_t)-1);
			ctx = EVP_PKEY_CTX_new(pair, NULL);
			CHECK(ctx != NULL);
			if (ctx != NULL) {
				OK(EVP_PKEY_decapsulate_init(ctx, NULL));
				memset(out, SENTINEL_BYTE, sizeof(out));
				length = sizeof(out);
				REFUSED(EVP_PKEY_decapsulate(ctx, out, &length, c, c_size), 0, R_ARGUMENT);
				CHECK(length == sizeof(out));
				CHECK_BYTES(out, guard, sizeof(out));
				EVP_PKEY_CTX_free(ctx);
			}
			EVP_PKEY_free(pair);
			wrong_ciphertext++;
		}
	}
	vec_close(&file);

	/* The file has 201 records, 161 valid; 20 of the invalid ones have a good key and a ciphertext of the wrong size. */
	CHECK(records == 201U);
	CHECK(valid == 161U);
	CHECK(wrong_ciphertext == 20U);
	printf("wycheproof mlkem_768: %lu valid, %lu wrong-size ciphertexts refused\n", valid, wrong_ciphertext);
}

/* Checks the Wycheproof mlkem_768_encaps_test vectors: an ek is accepted exactly when it has 1184 bytes with every coefficient below q, and encapsulating to an accepted one works. */
static void
test_mlkem_wycheproof_encaps(void)
{
	struct vec_file file;
	static uint8_t ek[1300];
	uint8_t ciphertext[CRYC_MLKEM768_CT];
	uint8_t shared[32];
	uint8_t out[CRYC_MLKEM768_EK];
	size_t ek_size;
	size_t ciphertext_size;
	size_t shared_size;
	size_t length;
	const char *result;
	unsigned long records;
	unsigned long accepted;
	unsigned long refused_length;
	unsigned long refused_modulus;
	EVP_PKEY *key;
	EVP_PKEY_CTX *ctx;

	if (vec_open(&file, "wycheproof-mlkem_768_encaps_test") != 0) {
		CHECK(0);
		return;
	}
	records = 0;
	accepted = 0;
	refused_length = 0;
	refused_modulus = 0;
	while (vec_next(&file)) {
		result = vec_get(&file, "result");
		if (result == NULL)
			continue;
		records++;
		ek_size = vec_hex(&file, "ek", ek, sizeof(ek));
		CHECK(ek_size != (size_t)-1);
		if (ek_size == (size_t)-1)
			continue;
		ERR_clear_error();
		key = EVP_PKEY_new_raw_public_key_ex(NULL, "ML-KEM-768", NULL, ek, ek_size);
		if (strcmp(result, "valid") == 0) {
			CHECK(key != NULL);
			CHECK(ERR_peek_error() == 0UL);
			if (key == NULL)
				continue;
			accepted++;

			/* It reads back unchanged and encapsulates. */
			length = sizeof(out);
			OK(EVP_PKEY_get_raw_public_key(key, out, &length));
			CHECK(length == 1184U);
			CHECK_BYTES(out, ek, 1184U);
			ctx = EVP_PKEY_CTX_new(key, NULL);
			CHECK(ctx != NULL);
			if (ctx != NULL) {
				OK(EVP_PKEY_encapsulate_init(ctx, NULL));
				ciphertext_size = sizeof(ciphertext);
				shared_size = sizeof(shared);
				OK(EVP_PKEY_encapsulate(ctx, ciphertext, &ciphertext_size, shared, &shared_size));
				CHECK(ciphertext_size == 1088U && shared_size == 32U);
				EVP_PKEY_CTX_free(ctx);
			}
			EVP_PKEY_free(key);
		} else {
			/* A refusal: the length (1184 bytes are needed) or the modulus check of FIPS 203 7.2. */
			CHECK(key == NULL);
			if (ek_size != 1184U) {
				check_refusal(R_KEY_LENGTH);
				refused_length++;
			} else {
				check_refusal(R_INVALID_KEY);
				refused_modulus++;
			}
			EVP_PKEY_free(key);
		}
	}
	vec_close(&file);

	/* The file has 265 records: 133 valid, 112 whose ek is not reduced (100 "Public key not reduced" and 12 ModulusOverflow), 20 of the wrong length. */
	CHECK(records == 265U);
	CHECK(accepted + refused_length + refused_modulus == 265U);
	CHECK(refused_modulus == 112U);
	CHECK(refused_length == 20U);
	CHECK(accepted == 133U);
	printf("wycheproof mlkem_768_encaps: %lu accepted, %lu refused for the length, %lu for the modulus\n",
	    accepted, refused_length, refused_modulus);
}

/* Checks the Wycheproof mlkem_768_semi_expanded_decaps_test vectors: a decapsulation key put into a key object decapsulates c to K, and a ciphertext of another size is refused. */
static void
test_mlkem_wycheproof_semi_expanded(void)
{
	struct vec_file file;
	static uint8_t dk[2500];
	static uint8_t c[1200];
	uint8_t key[64];
	uint8_t out[32];
	size_t dk_size;
	size_t c_size;
	size_t key_size;
	size_t length;
	const char *result;
	unsigned long valid;
	unsigned long wrong_ciphertext;
	unsigned long records;
	EVP_PKEY *pair;
	EVP_PKEY_CTX *ctx;

	if (vec_open(&file, "wycheproof-mlkem_768_semi_expanded_decaps_test") != 0) {
		CHECK(0);
		return;
	}
	valid = 0;
	wrong_ciphertext = 0;
	records = 0;
	while (vec_next(&file)) {
		result = vec_get(&file, "result");
		if (result == NULL)
			continue;
		records++;
		dk_size = vec_hex(&file, "dk", dk, sizeof(dk));
		c_size = vec_hex(&file, "c", c, sizeof(c));
		key_size = vec_hex(&file, "K", key, sizeof(key));

		/* Only records with a dk of the right size can be given to the interface (a dk is never imported by it). */
		if (dk_size != 2400U)
			continue;
		pair = mlkem_key_from_dk(dk);
		CHECK(pair != NULL);
		if (pair == NULL)
			continue;
		ctx = EVP_PKEY_CTX_new(pair, NULL);
		CHECK(ctx != NULL);
		if (ctx == NULL) {
			EVP_PKEY_free(pair);
			continue;
		}
		OK(EVP_PKEY_decapsulate_init(ctx, NULL));
		length = sizeof(out);
		if (strcmp(result, "valid") == 0) {
			CHECK(c_size == 1088U && key_size == 32U);
			memset(out, SENTINEL_BYTE, sizeof(out));
			OK(EVP_PKEY_decapsulate(ctx, out, &length, c, c_size));
			CHECK(length == 32U);
			CHECK_BYTES(out, key, 32U);
			valid++;
		} else if (c_size != 1088U) {
			REFUSED(EVP_PKEY_decapsulate(ctx, out, &length, c, c_size), 0, R_ARGUMENT);
			wrong_ciphertext++;
		}
		EVP_PKEY_CTX_free(ctx);
		EVP_PKEY_free(pair);
	}
	vec_close(&file);

	/* Nine records: three decapsulate (one plain, two with a zeroed byte that makes the re-encryption comparison matter), two have a wrong ciphertext size. */
	CHECK(records == 9U);
	CHECK(valid == 3U);
	CHECK(wrong_ciphertext == 2U);
}

/* Checks key generation, encapsulation and decapsulation round trips through the EVP interface, and the implicit rejection. */
static void
test_mlkem_round_trips(void)
{
	static const size_t flips[] = { 0U, 1U, 7U, 100U, 319U, 320U, 640U, 959U, 960U, 1000U, 1086U, 1087U };
	uint8_t ek[CRYC_MLKEM768_EK];
	uint8_t first_ek[CRYC_MLKEM768_EK];
	uint8_t ciphertext[CRYC_MLKEM768_CT];
	uint8_t previous[CRYC_MLKEM768_CT];
	uint8_t shared[32];
	uint8_t decapsulated[32];
	uint8_t reference[32];
	uint8_t guard[64];
	size_t length;
	size_t ciphertext_size;
	size_t shared_size;
	unsigned round;
	unsigned index;
	EVP_PKEY *pair;
	EVP_PKEY *previous_pair;
	EVP_PKEY *public_key;
	EVP_PKEY_CTX *encapsulator;
	EVP_PKEY_CTX *decapsulator;

	memset(guard, SENTINEL_BYTE, sizeof(guard));
	previous_pair = NULL;
	memset(first_ek, 0, sizeof(first_ek));
	for (round = 0; round < 24U; round++) {
		/* A key pair, with the sizes of FIPS 203 Table 3 and the encapsulation key stored inside the decapsulation key. */
		pair = mlkem_generate();
		CHECK(pair != NULL);
		if (pair == NULL)
			continue;
		CHECK(pair->type == CRYC_PKEY_MLKEM768 && pair->has_private == 1 && pair->references == 1);
		CHECK(pair->public_size == 1184U && pair->private_size == 2400U);
		CHECK(cryc_mlkem768_check_dk(pair->private_key) == 0);
		CHECK(memcmp(pair->private_key + 1152, pair->public_key, 1184U) == 0);
		CHECK(cryc_mlkem768_check_ek(pair->public_key) == 0);

		/* The public form: the size query, the export, the too-small refusals. */
		length = 0U;
		OK(EVP_PKEY_get_raw_public_key(pair, NULL, &length));
		CHECK(length == 1184U);
		length = 1184U;
		OK(EVP_PKEY_get_raw_public_key(pair, ek, &length));
		CHECK(length == 1184U);
		CHECK_BYTES(ek, pair->public_key, 1184U);
		length = 1183U;
		REFUSED(EVP_PKEY_get_raw_public_key(pair, ek, &length), 0, R_TOO_SMALL);
		CHECK(length == 1183U);

		/* The private form is never exported (not even its size). */
		length = 2400U;
		REFUSED(EVP_PKEY_get_raw_private_key(pair, shared, &length), 0, R_UNSUPPORTED);
		REFUSED(EVP_PKEY_get_raw_private_key(pair, NULL, &length), 0, R_UNSUPPORTED);
		CHECK(length == 2400U);

		/* Two key pairs differ. */
		if (previous_pair != NULL) {
			CHECK(memcmp(previous_pair->public_key, pair->public_key, 1184U) != 0);
			CHECK(memcmp(previous_pair->private_key, pair->private_key, 2400U) != 0);
		}
		if (round == 0U)
			memcpy(first_ek, ek, sizeof(first_ek));

		/* Encapsulation to the imported key and decapsulation with the pair, three times: the secrets agree and the ciphertexts differ. */
		public_key = EVP_PKEY_new_raw_public_key_ex(NULL, "ML-KEM-768", NULL, ek, 1184U);
		CHECK(public_key != NULL);
		encapsulator = (public_key != NULL) ? EVP_PKEY_CTX_new(public_key, NULL) : NULL;
		decapsulator = EVP_PKEY_CTX_new(pair, NULL);
		CHECK(encapsulator != NULL && decapsulator != NULL);
		if (encapsulator != NULL && decapsulator != NULL) {
			OK(EVP_PKEY_encapsulate_init(encapsulator, NULL));
			OK(EVP_PKEY_decapsulate_init(decapsulator, NULL));
			for (index = 0; index < 3U; index++) {
				ciphertext_size = sizeof(ciphertext);
				shared_size = sizeof(shared);
				OK(EVP_PKEY_encapsulate(encapsulator, ciphertext, &ciphertext_size, shared, &shared_size));
				CHECK(ciphertext_size == 1088U && shared_size == 32U);
				length = 32U;
				OK(EVP_PKEY_decapsulate(decapsulator, decapsulated, &length, ciphertext, ciphertext_size));
				CHECK(length == 32U);
				CHECK_BYTES(decapsulated, shared, 32U);
				if (index > 0U)
					CHECK(memcmp(previous, ciphertext, 1088U) != 0);
				memcpy(previous, ciphertext, sizeof(previous));
			}

			/* A changed ciphertext decapsulates without an error to another key: the implicit rejection J(z || c) of FIPS 203 Algorithm 18, which the primitive computes the same way. */
			if (round < 6U) {
				for (index = 0; index < sizeof(flips) / sizeof(flips[0]); index++) {
					ciphertext[flips[index]] ^= (uint8_t)(1U << (index % 8U));
					length = 32U;
					OK(EVP_PKEY_decapsulate(decapsulator, decapsulated, &length, ciphertext, 1088U));
					CHECK(length == 32U);
					CHECK(memcmp(decapsulated, shared, 32U) != 0);
					cryc_mlkem768_decaps(reference, ciphertext, pair->private_key);
					CHECK_BYTES(decapsulated, reference, 32U);
					ciphertext[flips[index]] ^= (uint8_t)(1U << (index % 8U));
				}

				/* The unchanged ciphertext still works. */
				length = 32U;
				OK(EVP_PKEY_decapsulate(decapsulator, decapsulated, &length, ciphertext, 1088U));
				CHECK_BYTES(decapsulated, shared, 32U);
			}
		}
		EVP_PKEY_CTX_free(encapsulator);
		EVP_PKEY_CTX_free(decapsulator);
		EVP_PKEY_free(public_key);
		EVP_PKEY_free(previous_pair);
		previous_pair = pair;
	}
	EVP_PKEY_free(previous_pair);
	CHECK(memcmp(first_ek, guard, 64U) != 0);

	/* One key-generation context makes several keys. */
	{
		EVP_PKEY_CTX *generator;
		EVP_PKEY *a;
		EVP_PKEY *b;

		generator = EVP_PKEY_CTX_new_from_name(NULL, "ml-kem-768", NULL);
		CHECK(generator != NULL);
		a = NULL;
		b = NULL;
		if (generator != NULL) {
			OK(EVP_PKEY_keygen_init(generator));
			OK(EVP_PKEY_keygen(generator, &a));
			OK(EVP_PKEY_keygen(generator, &b));
			CHECK(a != NULL && b != NULL && a != b);
			if (a != NULL && b != NULL)
				CHECK(memcmp(a->public_key, b->public_key, 1184U) != 0);

			/* A second preparation does no harm. */
			OK(EVP_PKEY_keygen_init(generator));
			EVP_PKEY_free(a);
			EVP_PKEY_free(b);
			EVP_PKEY_CTX_free(generator);
		}
	}
}

/* Checks the refusals and the state rules of the key generation, encapsulation and decapsulation functions. */
static void
test_mlkem_states(void)
{
	static const size_t bad_ciphertext_sizes[] = { 0U, 1U, 32U, 1087U, 1089U, 1184U, 2000U, 2400U, (size_t)-1 };
	static uint8_t ciphertext[CRYC_MLKEM768_CT + 64U];
	static uint8_t ek[CRYC_MLKEM768_EK];
	uint8_t shared[64];
	uint8_t guard[64];
	uint8_t seed[32];
	size_t wrapped_length;
	size_t shared_length;
	size_t length;
	size_t position;
	size_t room;
	unsigned index;
	EVP_PKEY *pair;
	EVP_PKEY *public_key;
	EVP_PKEY *ed_key;
	EVP_PKEY *x_key;
	EVP_PKEY *made;
	EVP_PKEY *sentinel_key;
	EVP_PKEY *bad_ek_key;
	EVP_PKEY_CTX *generator;
	EVP_PKEY_CTX *ctx;
	EVP_PKEY_CTX *pair_ctx;
	EVP_PKEY_CTX *public_ctx;
	int fake_engine;
	static OSSL_PARAM fake_params[1];

	fake_engine = 0;
	memset(guard, SENTINEL_BYTE, sizeof(guard));
	random_bytes(seed, sizeof(seed));
	pair = mlkem_generate();
	CHECK(pair != NULL);
	if (pair == NULL)
		return;
	memcpy(ek, pair->public_key, sizeof(ek));
	public_key = EVP_PKEY_new_raw_public_key_ex(NULL, "ML-KEM-768", NULL, ek, sizeof(ek));
	ed_key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, seed, 32U);
	x_key = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, seed, 32U);
	CHECK(public_key != NULL && ed_key != NULL && x_key != NULL);
	if (public_key == NULL || ed_key == NULL || x_key == NULL) {
		EVP_PKEY_free(pair);
		EVP_PKEY_free(public_key);
		EVP_PKEY_free(ed_key);
		EVP_PKEY_free(x_key);
		return;
	}

	/* The key-generation context by name: every refusal. */
	REFUSED(EVP_PKEY_CTX_new_from_name((OSSL_LIB_CTX *)&fake_engine, "ML-KEM-768", NULL), NULL, R_ARGUMENT);
	REFUSED(EVP_PKEY_CTX_new_from_name(NULL, "ML-KEM-768", "fips=yes"), NULL, R_ARGUMENT);
	REFUSED(EVP_PKEY_CTX_new_from_name(NULL, NULL, NULL), NULL, R_NULL);
	REFUSED(EVP_PKEY_CTX_new_from_name(NULL, "ML-KEM-512", NULL), NULL, R_ALGORITHM);
	REFUSED(EVP_PKEY_CTX_new_from_name(NULL, "ML-KEM-1024", NULL), NULL, R_ALGORITHM);
	REFUSED(EVP_PKEY_CTX_new_from_name(NULL, "ML-KEM-768x", NULL), NULL, R_ALGORITHM);
	REFUSED(EVP_PKEY_CTX_new_from_name(NULL, "ML-KEM-76", NULL), NULL, R_ALGORITHM);
	REFUSED(EVP_PKEY_CTX_new_from_name(NULL, "ED25519", NULL), NULL, R_ALGORITHM);
	REFUSED(EVP_PKEY_CTX_new_from_name(NULL, "X25519", NULL), NULL, R_ALGORITHM);
	REFUSED(EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL), NULL, R_ALGORITHM);
	REFUSED(EVP_PKEY_CTX_new_from_name(NULL, "", NULL), NULL, R_ALGORITHM);

	/* Key generation: refused on a context that is not a key generator, and before the preparation. */
	REFUSED(EVP_PKEY_keygen_init(NULL), 0, R_NULL);
	ctx = EVP_PKEY_CTX_new(pair, NULL);
	CHECK(ctx != NULL);
	REFUSED(EVP_PKEY_keygen_init(ctx), 0, R_ALGORITHM);
	CHECK(ctx->operation == CRYC_OP_NONE);
	EVP_PKEY_CTX_free(ctx);
	generator = EVP_PKEY_CTX_new_from_name(NULL, "ML-KEM-768", NULL);
	CHECK(generator != NULL);
	sentinel_key = (EVP_PKEY *)&fake_engine;
	made = sentinel_key;
	if (generator != NULL) {
		REFUSED(EVP_PKEY_keygen(generator, &made), 0, R_NOT_SET_UP);
		CHECK(made == sentinel_key);
		REFUSED(EVP_PKEY_keygen(NULL, &made), 0, R_NULL);
		REFUSED(EVP_PKEY_keygen(generator, NULL), 0, R_NULL);

		/* A generator is not a context for the other operations. */
		REFUSED(EVP_PKEY_encapsulate_init(generator, NULL), 0, R_ALGORITHM);
		REFUSED(EVP_PKEY_decapsulate_init(generator, NULL), 0, R_ALGORITHM);
		REFUSED(EVP_PKEY_derive_init(generator), 0, R_ALGORITHM);
		OK(EVP_PKEY_keygen_init(generator));
		CHECK(generator->operation == CRYC_OP_KEYGEN);
		REFUSED(EVP_PKEY_keygen(generator, NULL), 0, R_NULL);
		REFUSED(EVP_PKEY_keygen(NULL, NULL), 0, R_NULL);
		CHECK(made == sentinel_key);

		/* A generator that was prepared for key generation cannot encapsulate or derive. */
		wrapped_length = sizeof(ciphertext);
		shared_length = sizeof(shared);
		REFUSED(EVP_PKEY_encapsulate(generator, ciphertext, &wrapped_length, shared, &shared_length), 0, R_NOT_SET_UP);
		length = sizeof(shared);
		REFUSED(EVP_PKEY_decapsulate(generator, shared, &length, ciphertext, 1088U), 0, R_NOT_SET_UP);
		REFUSED(EVP_PKEY_derive(generator, shared, &length), 0, R_NOT_SET_UP);
		EVP_PKEY_CTX_free(generator);
	}

	/* The preparation for encapsulation: parameters refused, needs an ML-KEM key. */
	pair_ctx = EVP_PKEY_CTX_new(pair, NULL);
	public_ctx = EVP_PKEY_CTX_new(public_key, NULL);
	CHECK(pair_ctx != NULL && public_ctx != NULL);
	if (pair_ctx == NULL || public_ctx == NULL) {
		EVP_PKEY_CTX_free(pair_ctx);
		EVP_PKEY_CTX_free(public_ctx);
		EVP_PKEY_free(pair);
		EVP_PKEY_free(public_key);
		EVP_PKEY_free(ed_key);
		EVP_PKEY_free(x_key);
		return;
	}
	REFUSED(EVP_PKEY_encapsulate_init(NULL, NULL), 0, R_NULL);
	REFUSED(EVP_PKEY_encapsulate_init(public_ctx, fake_params), 0, R_ARGUMENT);
	REFUSED(EVP_PKEY_decapsulate_init(pair_ctx, fake_params), 0, R_ARGUMENT);
	REFUSED(EVP_PKEY_decapsulate_init(NULL, NULL), 0, R_NULL);
	CHECK(public_ctx->operation == CRYC_OP_NONE && pair_ctx->operation == CRYC_OP_NONE);
	ctx = EVP_PKEY_CTX_new(ed_key, NULL);
	CHECK(ctx != NULL);
	REFUSED(EVP_PKEY_encapsulate_init(ctx, NULL), 0, R_ALGORITHM);
	REFUSED(EVP_PKEY_decapsulate_init(ctx, NULL), 0, R_ALGORITHM);
	EVP_PKEY_CTX_free(ctx);
	ctx = EVP_PKEY_CTX_new(x_key, NULL);
	CHECK(ctx != NULL);
	REFUSED(EVP_PKEY_encapsulate_init(ctx, NULL), 0, R_ALGORITHM);
	REFUSED(EVP_PKEY_decapsulate_init(ctx, NULL), 0, R_ALGORITHM);
	EVP_PKEY_CTX_free(ctx);

	/* Decapsulation needs the private key; encapsulation does not. */
	REFUSED(EVP_PKEY_decapsulate_init(public_ctx, NULL), 0, R_INVALID_KEY);
	CHECK(public_ctx->operation == CRYC_OP_NONE);

	/* Before the preparation: nothing can be done. */
	wrapped_length = sizeof(ciphertext);
	shared_length = sizeof(shared);
	REFUSED(EVP_PKEY_encapsulate(public_ctx, ciphertext, &wrapped_length, shared, &shared_length), 0, R_NOT_SET_UP);
	length = 32U;
	REFUSED(EVP_PKEY_decapsulate(pair_ctx, shared, &length, ciphertext, 1088U), 0, R_NOT_SET_UP);

	/* Encapsulation: the missing arguments. */
	OK(EVP_PKEY_encapsulate_init(public_ctx, NULL));
	CHECK(public_ctx->operation == CRYC_OP_ENCAPSULATE);
	memset(ciphertext, SENTINEL_BYTE, sizeof(ciphertext));
	memset(shared, SENTINEL_BYTE, sizeof(shared));
	REFUSED(EVP_PKEY_encapsulate(NULL, ciphertext, &wrapped_length, shared, &shared_length), 0, R_NULL);
	REFUSED(EVP_PKEY_encapsulate(public_ctx, ciphertext, NULL, shared, &shared_length), 0, R_NULL);
	REFUSED(EVP_PKEY_encapsulate(public_ctx, ciphertext, &wrapped_length, shared, NULL), 0, R_NULL);
	REFUSED(EVP_PKEY_encapsulate(public_ctx, NULL, NULL, NULL, NULL), 0, R_NULL);
	REFUSED(EVP_PKEY_encapsulate(public_ctx, NULL, &wrapped_length, shared, &shared_length), 0, R_NULL);
	REFUSED(EVP_PKEY_encapsulate(public_ctx, ciphertext, &wrapped_length, NULL, &shared_length), 0, R_NULL);

	/* The size query gives both sizes and writes nothing. */
	wrapped_length = 0U;
	shared_length = 0U;
	OK(EVP_PKEY_encapsulate(public_ctx, NULL, &wrapped_length, NULL, &shared_length));
	CHECK(wrapped_length == 1088U && shared_length == 32U);
	CHECK_BYTES(shared, guard, sizeof(shared));

	/* A buffer too small (either one) is refused with both buffers and both lengths as they were. */
	for (room = 0; room < 1088U; room += (room < 40U ? 1U : 97U)) {
		wrapped_length = room;
		shared_length = 32U;
		REFUSED(EVP_PKEY_encapsulate(public_ctx, ciphertext, &wrapped_length, shared, &shared_length), 0, R_TOO_SMALL);
		CHECK(wrapped_length == room && shared_length == 32U);
		CHECK(ciphertext[0] == SENTINEL_BYTE && ciphertext[1087] == SENTINEL_BYTE);
		CHECK_BYTES(shared, guard, sizeof(shared));
	}
	wrapped_length = 1087U;
	shared_length = 32U;
	REFUSED(EVP_PKEY_encapsulate(public_ctx, ciphertext, &wrapped_length, shared, &shared_length), 0, R_TOO_SMALL);
	for (room = 0; room < 32U; room++) {
		wrapped_length = 1088U;
		shared_length = room;
		REFUSED(EVP_PKEY_encapsulate(public_ctx, ciphertext, &wrapped_length, shared, &shared_length), 0, R_TOO_SMALL);
		CHECK(wrapped_length == 1088U && shared_length == room);
		CHECK(ciphertext[0] == SENTINEL_BYTE && ciphertext[1087] == SENTINEL_BYTE);
		CHECK_BYTES(shared, guard, sizeof(shared));
	}

	/* More room than needed: the lengths become the sizes and nothing beyond them is written. */
	wrapped_length = sizeof(ciphertext);
	shared_length = sizeof(shared);
	OK(EVP_PKEY_encapsulate(public_ctx, ciphertext, &wrapped_length, shared, &shared_length));
	CHECK(wrapped_length == 1088U && shared_length == 32U);
	CHECK_BYTES(ciphertext + 1088, guard, 64U);
	CHECK_BYTES(shared + 32, guard, 32U);

	/* The decapsulation context: the preparation, then every refusal of the call. */
	OK(EVP_PKEY_decapsulate_init(pair_ctx, NULL));
	CHECK(pair_ctx->operation == CRYC_OP_DECAPSULATE);
	memset(ek, SENTINEL_BYTE, 40U);
	length = 32U;
	REFUSED(EVP_PKEY_decapsulate(NULL, ek, &length, ciphertext, 1088U), 0, R_NULL);
	REFUSED(EVP_PKEY_decapsulate(pair_ctx, ek, NULL, ciphertext, 1088U), 0, R_NULL);
	REFUSED(EVP_PKEY_decapsulate(pair_ctx, ek, &length, NULL, 1088U), 0, R_NULL);
	REFUSED(EVP_PKEY_decapsulate(pair_ctx, ek, &length, NULL, 0U), 0, R_NULL);
	for (index = 0; index < sizeof(bad_ciphertext_sizes) / sizeof(bad_ciphertext_sizes[0]); index++) {
		length = 32U;
		REFUSED(EVP_PKEY_decapsulate(pair_ctx, ek, &length, ciphertext, bad_ciphertext_sizes[index]), 0, R_ARGUMENT);
		CHECK(length == 32U);
		CHECK(ek[0] == SENTINEL_BYTE && ek[31] == SENTINEL_BYTE);
	}
	for (room = 0; room < 32U; room++) {
		length = room;
		REFUSED(EVP_PKEY_decapsulate(pair_ctx, ek, &length, ciphertext, 1088U), 0, R_TOO_SMALL);
		CHECK(length == room);
		CHECK(ek[0] == SENTINEL_BYTE && ek[31] == SENTINEL_BYTE);
	}

	/* The size query asks nothing of the ciphertext. */
	length = 0U;
	OK(EVP_PKEY_decapsulate(pair_ctx, NULL, &length, NULL, 0U));
	CHECK(length == 32U);
	length = 0U;
	OK(EVP_PKEY_decapsulate(pair_ctx, NULL, &length, ciphertext, 5U));
	CHECK(length == 32U);

	/* The decapsulation with more room writes 32 bytes only. */
	memset(shared, SENTINEL_BYTE, sizeof(shared));
	length = 64U;
	OK(EVP_PKEY_decapsulate(pair_ctx, shared, &length, ciphertext, 1088U));
	CHECK(length == 32U);
	CHECK_BYTES(shared + 32, guard, 32U);

	/* A context prepared for one operation refuses the other until it is prepared for it. */
	length = 32U;
	REFUSED(EVP_PKEY_decapsulate(public_ctx, ek, &length, ciphertext, 1088U), 0, R_NOT_SET_UP);
	wrapped_length = sizeof(ciphertext);
	shared_length = sizeof(shared);
	REFUSED(EVP_PKEY_encapsulate(pair_ctx, ciphertext, &wrapped_length, shared, &shared_length), 0, R_NOT_SET_UP);
	OK(EVP_PKEY_encapsulate_init(pair_ctx, NULL));
	OK(EVP_PKEY_encapsulate(pair_ctx, ciphertext, &wrapped_length, shared, &shared_length));
	length = 32U;
	REFUSED(EVP_PKEY_decapsulate(pair_ctx, ek, &length, ciphertext, 1088U), 0, R_NOT_SET_UP);
	REFUSED(EVP_PKEY_derive(pair_ctx, ek, &length), 0, R_NOT_SET_UP);
	REFUSED(EVP_PKEY_derive_set_peer(pair_ctx, x_key), 0, R_NOT_SET_UP);
	EVP_PKEY_CTX_free(pair_ctx);
	EVP_PKEY_CTX_free(public_ctx);

	/* A key object whose public form does not pass the modulus check (not made by the interface) cannot be encapsulated to: the one error says the key is invalid. */
	bad_ek_key = mlkem_key_from_dk(pair->private_key);
	CHECK(bad_ek_key != NULL);
	if (bad_ek_key != NULL) {
		memset(bad_ek_key->public_key, 0xff, CRYC_MLKEM768_EK);
		ctx = EVP_PKEY_CTX_new(bad_ek_key, NULL);
		CHECK(ctx != NULL);
		if (ctx != NULL) {
			OK(EVP_PKEY_encapsulate_init(ctx, NULL));
			memset(ciphertext, SENTINEL_BYTE, sizeof(ciphertext));
			memset(shared, SENTINEL_BYTE, sizeof(shared));
			wrapped_length = 1088U;
			shared_length = 32U;
			REFUSED(EVP_PKEY_encapsulate(ctx, ciphertext, &wrapped_length, shared, &shared_length), 0, R_INVALID_KEY);
			CHECK(ciphertext[0] == SENTINEL_BYTE && ciphertext[1087] == SENTINEL_BYTE);
			CHECK_BYTES(shared, guard, 32U);
			CHECK(wrapped_length == 1088U && shared_length == 32U);
			EVP_PKEY_CTX_free(ctx);
		}
		EVP_PKEY_free(bad_ek_key);
	}

	/* The import's checks: lengths around 1184, an all-zero key, the largest reduced coefficient everywhere, and a coefficient of q or more at many places. */
	random_bytes(ek, sizeof(ek));
	for (index = 0; index < 1184U; index++)
		ek[index] = 0;
	made = EVP_PKEY_new_raw_public_key_ex(NULL, "ML-KEM-768", NULL, ek, 1184U);
	CHECK(made != NULL);
	EVP_PKEY_free(made);

	/* Every 12-bit coefficient of the first 1152 bytes is 3328 (0xd00) and then the rest of the key is arbitrary. */
	for (index = 0; index < 1152U; index += 3U) {
		ek[index] = 0x00U;
		ek[index + 1U] = 0x0dU | 0x00U;
		ek[index + 2U] = 0xd0U;
	}
	made = EVP_PKEY_new_raw_public_key_ex(NULL, "ML-KEM-768", NULL, ek, 1184U);
	CHECK(made != NULL);
	EVP_PKEY_free(made);
	REFUSED(EVP_PKEY_new_raw_public_key_ex(NULL, "ML-KEM-768", NULL, ek, 1183U), NULL, R_KEY_LENGTH);
	REFUSED(EVP_PKEY_new_raw_public_key_ex(NULL, "ML-KEM-768", NULL, ek, 1185U), NULL, R_KEY_LENGTH);
	REFUSED(EVP_PKEY_new_raw_public_key_ex(NULL, "ML-KEM-768", NULL, ek, 0U), NULL, R_KEY_LENGTH);
	REFUSED(EVP_PKEY_new_raw_public_key_ex(NULL, "ML-KEM-768", NULL, NULL, 1184U), NULL, R_NULL);

	/* A coefficient of 3329 (0xd01) or more, in each of the two halves of a 3-byte group, anywhere among the 384 groups. */
	for (position = 0; position < 1152U; position += 3U) {
		/* The first coefficient of the group set to 3329. */
		memcpy(shared, ek + position, 3U);
		ek[position] = 0x01U;
		ek[position + 1U] = (uint8_t)((ek[position + 1U] & 0xf0U) | 0x0dU);
		REFUSED(EVP_PKEY_new_raw_public_key_ex(NULL, "ML-KEM-768", NULL, ek, 1184U), NULL, R_INVALID_KEY);
		memcpy(ek + position, shared, 3U);

		/* The second coefficient set to 4095. */
		ek[position + 1U] = (uint8_t)(ek[position + 1U] | 0xf0U);
		ek[position + 2U] = 0xffU;
		REFUSED(EVP_PKEY_new_raw_public_key_ex(NULL, "ML-KEM-768", NULL, ek, 1184U), NULL, R_INVALID_KEY);
		memcpy(ek + position, shared, 3U);
	}

	/* The rho part (the last 32 bytes) is never checked: any bytes there are accepted. */
	memset(ek + 1152, 0xff, 32U);
	made = EVP_PKEY_new_raw_public_key_ex(NULL, "ML-KEM-768", NULL, ek, 1184U);
	CHECK(made != NULL);
	EVP_PKEY_free(made);

	/* The interface's numbered constructors refuse ML-KEM (it is made by name): no type number exists for it. */
	REFUSED(EVP_PKEY_new_raw_public_key(CRYC_PKEY_MLKEM768, NULL, ek, 1184U), NULL, R_ALGORITHM);

	EVP_PKEY_free(pair);
	EVP_PKEY_free(public_key);
	EVP_PKEY_free(ed_key);
	EVP_PKEY_free(x_key);
}

/* Runs the checks. */
int
main(void)
{
	/* The raw keys, the reference counts and the threads. */
	test_raw_keys();
	test_references();
	test_threads();

	/* Ed25519. */
	test_ed25519_vectors();
	test_ed25519_wycheproof();
	test_ed25519_states();
	test_ed25519_random();

	/* X25519. */
	test_x25519_rfc();
	test_x25519_wycheproof();
	test_x25519_states();

	/* ML-KEM-768. */
	test_mlkem_wycheproof_keys();
	test_mlkem_wycheproof_encaps();
	test_mlkem_wycheproof_semi_expanded();
	test_mlkem_round_trips();
	test_mlkem_states();

	/* Nothing may be left in the error queue by a successful call. */
	CHECK(ERR_peek_error() == 0UL);

	/* The result. */
	return check_finish("test-api-pkey");
}
