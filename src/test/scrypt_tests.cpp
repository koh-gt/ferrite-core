#include <boost/test/unit_test.hpp>

#include <crypto/scrypt.h>
#include <uint256.h>
#include <util/strencodings.h>

BOOST_AUTO_TEST_SUITE(scrypt_tests)

BOOST_AUTO_TEST_CASE(scrypt_hashtest)
{
    // Test Scrypt hash with known inputs against expected outputs: five Litecoin block headers,
    // then Ferrite's genesis block and its blocks at the checkpoint heights and at the assumevalid block.
    #define HASHCOUNT 12
    const char* inputhex[HASHCOUNT] = {
        "020000004c1271c211717198227392b029a64a7971931d351b387bb80db027f270411e398a07046f7d4a08dd815412a8712f874a7ebf0507e3878bd24e20a3b73fd750a667d2f451eac7471b00de6659", // Litecoin
        "0200000011503ee6a855e900c00cfdd98f5f55fffeaee9b6bf55bea9b852d9de2ce35828e204eef76acfd36949ae56d1fbe81c1ac9c0209e6331ad56414f9072506a77f8c6faf551eac7471b00389d01", // Litecoin
        "02000000a72c8a177f523946f42f22c3e86b8023221b4105e8007e59e81f6beb013e29aaf635295cb9ac966213fb56e046dc71df5b3f7f67ceaeab24038e743f883aff1aaafaf551eac7471b0166249b", // Litecoin
        "010000007824bc3a8a1b4628485eee3024abd8626721f7f870f8ad4d2f33a27155167f6a4009d1285049603888fe85a84b6c803a53305a8d497965a5e896e1a00568359589faf551eac7471b0065434e", // Litecoin
        "0200000050bfd4e4a307a8cb6ef4aef69abc5c0f2d579648bd80d7733e1ccc3fbc90ed664a7f74006cb11bde87785f229ecd366c2d4e44432832580e0608c579e4cb76f383f7f551eac7471b00c36982", // Litecoin
        "0100000000000000000000000000000000000000000000000000000000000000000000009295b10524dcac615317ba916b900962e7b9bdf504c48df3b8568b92aab5b23d07ff7c63f0ff0f1ea0f51a00", // Ferrite genesis
        "040000207b24eb2ac499c60dea099537a8c13b2cb1388780d6b36aa029ad1bb92457229a436095d4ce41f0dff47f8e1bf2afdd85d6fa146bdeb555ea1a00c666f6dbc245f9095c68aac22a1c84da45f0", // Ferrite 450,000
        "00000020c6acbd19d4b5ed1d65d8ea7cdda010225c2a85d4e8c3e53556ac9726669ee347f037a33001ee14c3eba80998181db6d65dfa2ff21a56800b35cdc4ed0147ecf0398afb68a860411c5773c7de", // Ferrite 602,214
        "000000201a915cc0e32ec362e2bf55183032390146acc3876a9c7fbc95ee4cd392969bcc400535303289f8f3a5992f87799edcc9e3ae8cab0d2f9048966e9aa94cd4f590dd2f9069726f031c38201459", // Ferrite 750,000
        "00000020d515af41600967fff967328a24f743cc55751309e2718dc950a4f741d431a237cff1463bda79f25d4f5434ad8a0dd1fdbb0858d5ebcc5efb679744f3abe90c9a03b92a6ae95b0d1cb5a86b5c", // Ferrite 903,321
        "00000020d30d52992434d2cd9bc0ba927db57e763d0dc7f3a06a3b45448ea3ba7720532b37183f2e38ec7597261e1a69bad638e3b0088043003fbe4762bffcfc7f38b15079c1946ab22e141c621a01cc", // Ferrite 1,000,000
        "000000205be3e810cdba2c99b1077dc8d6915d2684f877307ac5f60db315fc2c409aa0306f5389e74a1ca791bf0f63ce1598abdf6d9b02f1bc4a86fb3c1d17251155a9472b62c76ad08b011c0b618b94", // Ferrite 1,046,922
    };
    const char* expected[HASHCOUNT] = {
        "00000000002bef4107f882f6115e0b01f348d21195dacd3582aa2dabd7985806",
        "00000000003a0d11bdd5eb634e08b7feddcfbbf228ed35d250daf19f1c88fc94",
        "00000000000b40f895f288e13244728a6c2d9d59d8aff29c65f8dd5114a8ca81",
        "00000000003007005891cd4923031e99d8e8d72f6e8e7edc6a86181897e105fe",
        "000000000018f0b426a4afc7130ccb47fa02af730d345b4fe7c7724d3800ec8c",
        "000006c8a4af2a02c16c5538fe15abd1ee52cd804fd6033f7cb4d2739af74aa6",
        "0000000004b2d310692c2e5416fd53922e577dbc329f5f97eb41f89554921de7",
        "000000002c84e7e090da5139b12b7157a97ea734d1619b1678de7fc08173897d",
        "00000000021f821126efa46df1494dff40e140fa398a730acd5fd98d5c1ccf8d",
        "00000000049e7fb6bce318f284aa9b89c8239db22fba17731fe36cdc5e51051d",
        "0000000010a7c4ec13a6a098dc9f003d9adfef7b024d7777f2220ec619537ad4",
        "000000000173c2987b4dd608d9e025896f3f012a21236679f378c3e42827752f",
    };
#if defined(USE_SSE2)
    (void) scrypt_detect_sse2();
#endif
    uint256 scrypthash;
    std::vector<unsigned char> inputbytes;
    char scratchpad[SCRYPT_SCRATCHPAD_SIZE];
    for (int i = 0; i < HASHCOUNT; i++) {
        inputbytes = ParseHex(inputhex[i]);
#if defined(USE_SSE2)
        // Test SSE2 scrypt
        scrypt_1024_1_1_256_sp_sse2((const char*)&inputbytes[0], BEGIN(scrypthash), scratchpad);
        BOOST_CHECK_EQUAL(scrypthash.ToString().c_str(), expected[i]);
#endif
        // Test generic scrypt
        scrypt_1024_1_1_256_sp_generic((const char*)&inputbytes[0], BEGIN(scrypthash), scratchpad);
        BOOST_CHECK_EQUAL(scrypthash.ToString().c_str(), expected[i]);
    }
}

BOOST_AUTO_TEST_CASE(pbkdf2_sha256_rfc7914)
{
    // PBKDF2-HMAC-SHA256 test vectors from RFC 7914, section 11.
    uint8_t out[64];
    PBKDF2_SHA256((const uint8_t*)"passwd", 6, (const uint8_t*)"salt", 4, 1, out, sizeof(out));
    BOOST_CHECK_EQUAL(HexStr(out), "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783");
    PBKDF2_SHA256((const uint8_t*)"Password", 8, (const uint8_t*)"NaCl", 4, 80000, out, sizeof(out));
    BOOST_CHECK_EQUAL(HexStr(out), "4ddcd8f60b98be21830cee5ef22701f9641a4418d04c0414aeff08876b34ab56a1d425a1225833549adb841b51c9b3176a272bdebba1d078478f62b397f33c8d");
}

BOOST_AUTO_TEST_SUITE_END()
