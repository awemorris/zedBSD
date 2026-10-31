#!/bin/sh
# Fetches the specifications and the test vectors of libcrypto-compat and libssl-compat into caches outside git,
# checks each file's SHA-256 against the list below, and converts the vectors into the record files the unit tests
# read (vectors.py).  Plain GETs without parameters; a file already present with the right digest is not fetched
# again.  A digest that does not match is an error (the file is left as <name>.bad for Q1 to inspect).
#   sh plan/ws335/tests/fetch.sh specs      into WS335_SPECS   (default build/ws335-specs)
#   sh plan/ws335/tests/fetch.sh vectors    into WS335_VECTORS (default build/ws335-vectors), then converts
# The shared, lasting copies are /home/awe/zedBSD-claude1/build/sources/specs/tls/ and
# /home/awe/zedBSD-claude1/build/sources/tls-vectors/ (Q1 places them); point the variables there to use them.
# Last line: fetch: PASS or fetch: FAIL.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."

what=${1:-}
case "$what" in
specs) dir=${WS335_SPECS:-build/ws335-specs} ;;
vectors) dir=${WS335_VECTORS:-build/ws335-vectors} ;;
*) echo "usage: sh plan/ws335/tests/fetch.sh specs|vectors"; exit 2 ;;
esac
mkdir -p "$dir"
status=0

# name sha256 url
get() {
	if [ -f "$dir/$1" ] && [ "$(sha256sum "$dir/$1" | cut -d' ' -f1)" = "$2" ]; then
		return 0
	fi
	if ! curl -sS -f -L --max-time 300 -o "$dir/$1.part" "$3"; then
		echo "FAILED fetch $1"
		status=1
		return 1
	fi
	if [ "$(sha256sum "$dir/$1.part" | cut -d' ' -f1)" != "$2" ]; then
		mv -- "$dir/$1.part" "$dir/$1.bad"
		echo "FAILED digest $1 (kept as $1.bad)"
		status=1
		return 1
	fi
	mv -- "$dir/$1.part" "$dir/$1"
}

rfc() {
	get "rfc$1.txt" "$2" "https://www.rfc-editor.org/rfc/rfc$1.txt"
}

nist=https://nvlpubs.nist.gov/nistpubs
cavp=https://csrc.nist.gov/CSRC/media/Projects/Cryptographic-Algorithm-Validation-Program/documents
itu='https://www.itu.int/rec/dologin_pub.asp?lang=e&type=items&id=T-REC'

if [ "$what" = specs ]; then
	rfc 2104 64d5245a9101929025336e470e3737f118704001249d503c85a86e19fe9fbb01
	rfc 3279 6d3f19f18e17fa1c68da5aaf4021327748fabca840d7300443b77357a1fc1614
	rfc 4055 b8a1ef3fb135c32aed4eee121264f3ec83a46def746c8fe68e05bd1b60324e9a
	rfc 4158 5d67d470a12edf63574c421e43d2d6b86f78e2eb6c79282acb73917f824fb3be
	rfc 4231 72178527ce93500e730bc8eb182b857e583096d652b64ece0879c52ba1df973b
	rfc 4648 84e14418f795d503be5f34bf23ce4ebaa119e9ec7c9f667d8caeb111385b178f
	rfc 5246 2ebe1b8e651696aeacc8ab3848bedb465505737901ea3b691acf40c392958feb
	rfc 5280 a2f2628c0a83b873fc4786abd921f9b2c02395954b655d190bf16b831633345d
	rfc 5288 f68375c3f1948802240ca5b21f3aa44311ea02f877e4e240d1fc70927ecf1307
	rfc 5289 4bc7e5ae5b8a9e8c022a85530989593ba95bff2d2153aaf2aa5c017f2bd1410b
	rfc 5480 593bf29fd0da2ff8b903c3ebf1c9d189a770039159e2ba46a0c3b91355037f26
	rfc 5746 fb894a464bffd123ef7be802b63d7309b7af2bc34091683873c311556de2036d
	rfc 5758 4d02628ff0875a1960d34be584a68f88528b96242bdc5a05a40a29ef01cf1532
	rfc 5869 7a40eb3835b35fc947eb12a2ed614db079d43b26e50dbc537c31fba16397089c
	rfc 6066 6a33cceaa5e1b6518923d9d04ffaa738bad0417806b3c94a995484e140f5cbc0
	rfc 6125 ee1f413f420bfa6199302b1a85669243f27076c803a120d55c15198e300d7cf1
	rfc 6234 8f39f02a57bfd1da15634706724a585766e6223f77ecdaf243cad16cd3a1aa1b
	rfc 7301 ba122aee58871c604c9e98e2c9f81d5042e4056bd497cd41e23ad80fe3123222
	rfc 7468 0b2c3c2087cc0b099789c90e61c0208e87b25793f0ce40090979e8c734b3d989
	rfc 7627 3e908b1a511f5894b64b2c7867c5e0e8db43896add4d99a340fe67a07ee11a59
	rfc 7748 279ca0ecc5e92e2962e27b846986aeb74729d9dd34bd4a04a362f80dcb596ad3
	rfc 7905 afa1e233822c6bda1f1ced014d32e6d740454415b53130fa84630a9c5ad2fe69
	rfc 8017 1e72dc473d18df3fc5598cdc12795a9f18f36f1aef15abc23a55eb0d58151d11
	rfc 8032 ed63657ff389301282b169b0abde9b5dd2c7e4d524fdfa5da6ff3094fc93c4c3
	rfc 8422 cac45754399e5115342cf1d773a51ac5a77ad221e9336e43ceadd8f294712350
	rfc 8439 25bef70fbf7a07ff45c2fe4cb7c6ce954eac687413d8610603268b4e4415324c
	rfc 8446 47871bc8820a2c3b6ea89f061055577058862cf543686b82d10131239702b3bd
	rfc 8448 6564d1376d1ec744fc7a9993da15ebc1b9be361908b166091f47ef605c537fba
	rfc 8731 26dcf1e8481d83e0b2403e41df47df3fb08cae154d1e464631a23b3cd025f9d1
	rfc 8996 a8250792df7fc533328f2021dbdcce847bbdfbc703de89a93e18ce165d4b8d14
	rfc 9155 87c859e927ab6bbf245f4f3cd909852b28c5d253e2305cbdd715e93aa79db7d2
	rfc 9325 e3475381d056bde1eed65252954cce68250168454387f6799b38660806f215bc
	rfc 9525 ed8116c2435376dffe37ec2ca688c3a8515e36820852b29e0b6cdfbe92abd2c5
	get nist-fips-180-4.pdf 0455b406d89648d20cbde375561e19c245b9815e894164c2670772e3d54deb82 $nist/FIPS/NIST.FIPS.180-4.pdf
	get nist-fips-186-5.pdf fbb9c7c2ba442f03c57b63b43c888311903c9d0f29f89b06efdebd9b619140c5 $nist/FIPS/NIST.FIPS.186-5.pdf
	get nist-fips-197-upd1.pdf 62c86eb567f13edb8f71826e985da870b04ef6381634f303cdb16e84d47becd1 $nist/FIPS/NIST.FIPS.197-upd1.pdf
	get nist-fips-202.pdf 1592607831ff0908cc590632ce371c6c95e94025bb1a0c8ae90a4d0ec1ed025e $nist/FIPS/NIST.FIPS.202.pdf
	get nist-fips-203.pdf fe1f12f32a7e44ec9fdebbf400cda843a40b506dee676725234dc6f7923b6cac $nist/FIPS/NIST.FIPS.203.pdf
	get nist-sp-800-38d.pdf d99f3921ccebca049e7522426553aba071dae14ec3d5b6041e8c111a6cb57bba \
		$nist/Legacy/SP/nistspecialpublication800-38d.pdf
	get nist-sp-800-56a-r3.pdf 6b315e3a91012981f9a88ad62c97ea7d4b170a7746e713f34dd29538ee46f403 \
		$nist/SpecialPublications/NIST.SP.800-56Ar3.pdf
	get nist-sp-800-186.pdf 53c7cf3340528aaa0a7bbeaef947f2099568192367d4341a7d4abceb55f78432 \
		$nist/SpecialPublications/NIST.SP.800-186.pdf
	get secg-sec1-v2.pdf 54687189af3de645756a30a801b596fd14c6566992c13bfd58eede399f324a89 https://www.secg.org/sec1-v2.pdf
	get itu-t-x680-202102.pdf b69dc4754f0709a7e344c886f277ed9e37a4336029593ba91bc6df6065f9ee14 "$itu-X.680-202102-I!!PDF-E"
	get itu-t-x690-202102.pdf 6ac799a2d1ebed36abe39c9eac3586185bfb7db679f801e737189d6fdb1bbd6c "$itu-X.690-202102-I!!PDF-E"
fi

if [ "$what" = vectors ]; then
	get wycheproof-d17cc89fa5c8.tar.gz 88dcda078c6ff643cf95c676e781c595f76e768d66ad85c01e1704f26d062ddc \
		https://github.com/C2SP/wycheproof/archive/d17cc89fa5c8f4f4d378b5385ebf75da5d0926c9.tar.gz
	get x509-limbo-1080e565f26e.json 611e337b9fb477b927bae65434650692cb9d2070ff5b05f975d0403510acd4de \
		https://raw.githubusercontent.com/C2SP/x509-limbo/1080e565f26e544bbe9cf99953be8a57c08e7e20/limbo.json
	get cavp-shabytetestvectors.zip 929ef80b7b3418aca026643f6f248815913b60e01741a44bba9e118067f4c9b8 \
		$cavp/shs/shabytetestvectors.zip
	get cavp-sha-3bytetestvectors.zip cd07701af2e47f5cc889d642528b4bf11f8b6eb55797c7307a96828ed8d8fc8c \
		$cavp/sha3/sha-3bytetestvectors.zip
	get cavp-shakebytetestvectors.zip debfebc3157b3ceea002b84ca38476420389a3bf7e97dc5f53ea4689a16de4c7 \
		$cavp/sha3/shakebytetestvectors.zip
	get cavp-KAT_AES.zip a203b16c9246b2ebae31dee5de21a606be80cf78ceabaca37150236fa098eb60 $cavp/aes/KAT_AES.zip
	get cavp-gcmtestvectors.zip f9fc479e134cde2980b3bb7cddbcb567b2cd96fd753835243ed067699f26a023 $cavp/mac/gcmtestvectors.zip
	get cavp-186-4ecdsatestvectors.zip fe47cc92b4cee418236125c9ffbcd9bb01c8c34e74a4ba195d954bcb72824752 \
		$cavp/dss/186-4ecdsatestvectors.zip
	get cavp-186-3rsatestvectors.zip 8405aeb3572a4f98ed4b1a3ccb3f2f49e725462dd28ec4759d6a15d88855d19c \
		$cavp/dss/186-3rsatestvectors.zip
	get cavp-ecccdhtestvectors.zip 5fff092551f2d72e89a3d9362711878708f9a14b502f0dfae819649105b0ea39 \
		$cavp/components/ecccdhtestvectors.zip
	if [ "$status" = 0 ]; then
		specs=${WS335_SPECS:-build/ws335-specs}
		if ! python3 -I plan/ws335/tests/vectors.py "$dir" "$specs/rfc8448.txt"; then
			echo "FAILED vectors.py"
			status=1
		fi
	fi
fi

if [ "$status" = 0 ]; then echo "fetch: PASS"; else echo "fetch: FAIL"; fi
exit "$status"
