// Copyright (c) 2012-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <rpc/client.h>
#include <rpc/request.h>
#include <rpc/server.h>
#include <rpc/util.h>

#include <amount.h>
#include <core_io.h>
#include <interfaces/chain.h>
#include <node/context.h>
#include <random.h>
#include <test/util/setup_common.h>
#include <tinyformat.h>
#include <util/ref.h>
#include <util/time.h>

#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <boost/algorithm/string.hpp>
#include <boost/test/unit_test.hpp>

#include <univalue.h>

#include <rpc/blockchain.h>

class RPCTestingSetup : public TestingSetup
{
public:
    UniValue CallRPC(std::string args);
};

UniValue RPCTestingSetup::CallRPC(std::string args)
{
    std::vector<std::string> vArgs;
    boost::split(vArgs, args, boost::is_any_of(" \t"));
    std::string strMethod = vArgs[0];
    vArgs.erase(vArgs.begin());
    util::Ref context{m_node};
    JSONRPCRequest request(context);
    request.strMethod = strMethod;
    request.params = RPCConvertValues(strMethod, vArgs);
    request.fHelp = false;
    if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
    try {
        UniValue result = tableRPC.execute(request);
        return result;
    }
    catch (const UniValue& objError) {
        throw std::runtime_error(find_value(objError, "message").get_str());
    }
}


BOOST_FIXTURE_TEST_SUITE(rpc_tests, RPCTestingSetup)

BOOST_AUTO_TEST_CASE(rpc_rawparams)
{
    // Test raw transaction API argument handling
    UniValue r;

    BOOST_CHECK_THROW(CallRPC("getrawtransaction"), std::runtime_error);
    BOOST_CHECK_THROW(CallRPC("getrawtransaction not_hex"), std::runtime_error);
    BOOST_CHECK_THROW(CallRPC("getrawtransaction a3b807410df0b60fcb9736768df5823938b2f838694939ba45f3c0a1bff150ed not_int"), std::runtime_error);

    BOOST_CHECK_THROW(CallRPC("createrawtransaction"), std::runtime_error);
    BOOST_CHECK_THROW(CallRPC("createrawtransaction null null"), std::runtime_error);
    BOOST_CHECK_THROW(CallRPC("createrawtransaction not_array"), std::runtime_error);
    BOOST_CHECK_THROW(CallRPC("createrawtransaction {} {}"), std::runtime_error);
    BOOST_CHECK_NO_THROW(CallRPC("createrawtransaction [] {}"));
    BOOST_CHECK_THROW(CallRPC("createrawtransaction [] {} extra"), std::runtime_error);

    BOOST_CHECK_THROW(CallRPC("decoderawtransaction"), std::runtime_error);
    BOOST_CHECK_THROW(CallRPC("decoderawtransaction null"), std::runtime_error);
    BOOST_CHECK_THROW(CallRPC("decoderawtransaction DEADBEEF"), std::runtime_error);
    std::string rawtx = "0100000001a15d57094aa7a21a28cb20b59aab8fc7d1149a3bdbcddba9c622e4f5f6a99ece010000006c493046022100f93bb0e7d8db7bd46e40132d1f8242026e045f03a0efe71bbb8e3f475e970d790221009337cd7f1f929f00cc6ff01f03729b069a7c21b59b1736ddfee5db5946c5da8c0121033b9b137ee87d5a812d6f506efdd37f0affa7ffc310711c06c7f3e097c9447c52ffffffff0100e1f505000000001976a9140389035a9225b3839e2bbf32d826a1e222031fd888ac00000000";
    BOOST_CHECK_NO_THROW(r = CallRPC(std::string("decoderawtransaction ")+rawtx));
    BOOST_CHECK_EQUAL(find_value(r.get_obj(), "size").get_int(), 193);
    BOOST_CHECK_EQUAL(find_value(r.get_obj(), "version").get_int(), 1);
    BOOST_CHECK_EQUAL(find_value(r.get_obj(), "locktime").get_int(), 0);
    BOOST_CHECK_THROW(CallRPC(std::string("decoderawtransaction ")+rawtx+" extra"), std::runtime_error);
    BOOST_CHECK_NO_THROW(r = CallRPC(std::string("decoderawtransaction ")+rawtx+" false"));
    BOOST_CHECK_THROW(r = CallRPC(std::string("decoderawtransaction ")+rawtx+" false extra"), std::runtime_error);

    // Only check failure cases for sendrawtransaction, there's no network to send to...
    BOOST_CHECK_THROW(CallRPC("sendrawtransaction"), std::runtime_error);
    BOOST_CHECK_THROW(CallRPC("sendrawtransaction null"), std::runtime_error);
    BOOST_CHECK_THROW(CallRPC("sendrawtransaction DEADBEEF"), std::runtime_error);
    BOOST_CHECK_THROW(CallRPC(std::string("sendrawtransaction ")+rawtx+" extra"), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(rpc_togglenetwork)
{
    UniValue r;

    r = CallRPC("getnetworkinfo");
    bool netState = find_value(r.get_obj(), "networkactive").get_bool();
    BOOST_CHECK_EQUAL(netState, true);

    BOOST_CHECK_NO_THROW(CallRPC("setnetworkactive false"));
    r = CallRPC("getnetworkinfo");
    int numConnection = find_value(r.get_obj(), "connections").get_int();
    BOOST_CHECK_EQUAL(numConnection, 0);

    netState = find_value(r.get_obj(), "networkactive").get_bool();
    BOOST_CHECK_EQUAL(netState, false);

    BOOST_CHECK_NO_THROW(CallRPC("setnetworkactive true"));
    r = CallRPC("getnetworkinfo");
    netState = find_value(r.get_obj(), "networkactive").get_bool();
    BOOST_CHECK_EQUAL(netState, true);
}

BOOST_AUTO_TEST_CASE(rpc_rawsign)
{
    UniValue r;
    // input is a 1-of-2 multisig (so is output):
    std::string prevout =
      "[{\"txid\":\"b4cc287e58f87cdae59417329f710f3ecd75a4ee1d2872b7248f50977c8493f3\","
      "\"vout\":1,\"scriptPubKey\":\"a914b10c9df5f7edf436c697f02f1efdba4cf399615187\","
      "\"redeemScript\":\"512103debedc17b3df2badbcdd86d5feb4562b86fe182e5998abd8bcd4f122c6155b1b21027e940bb73ab8732bfdf7f9216ecefca5b94d6df834e77e108f68e66f126044c052ae\"}]";
    r = CallRPC(std::string("createrawtransaction ")+prevout+" "+
      "{\"3HqAe9LtNBjnsfM4CyYaWTnvCaUYT7v4oZ\":11}");
    std::string notsigned = r.get_str();
    std::string privkey1 = "\"T6hoRM7L8u4f9vHd4eGMAmwV6AMCE11PvYi7YjrdegG223kw64r1\"";
    std::string privkey2 = "\"T5Xu6pe5iqQYqXGxhcY2QEFr7NNoVQ5R6A4abpswunCTF9w85g8V\"";
    r = CallRPC(std::string("signrawtransactionwithkey ")+notsigned+" [] "+prevout);
    BOOST_CHECK(find_value(r.get_obj(), "complete").get_bool() == false);
    r = CallRPC(std::string("signrawtransactionwithkey ")+notsigned+" ["+privkey1+","+privkey2+"] "+prevout);
    BOOST_CHECK(find_value(r.get_obj(), "complete").get_bool() == true);
}

BOOST_AUTO_TEST_CASE(rpc_createraw_op_return)
{
    BOOST_CHECK_NO_THROW(CallRPC("createrawtransaction [{\"txid\":\"a3b807410df0b60fcb9736768df5823938b2f838694939ba45f3c0a1bff150ed\",\"vout\":0}] {\"data\":\"68656c6c6f776f726c64\"}"));

    // Key not "data" (bad address)
    BOOST_CHECK_THROW(CallRPC("createrawtransaction [{\"txid\":\"a3b807410df0b60fcb9736768df5823938b2f838694939ba45f3c0a1bff150ed\",\"vout\":0}] {\"somedata\":\"68656c6c6f776f726c64\"}"), std::runtime_error);

    // Bad hex encoding of data output
    BOOST_CHECK_THROW(CallRPC("createrawtransaction [{\"txid\":\"a3b807410df0b60fcb9736768df5823938b2f838694939ba45f3c0a1bff150ed\",\"vout\":0}] {\"data\":\"12345\"}"), std::runtime_error);
    BOOST_CHECK_THROW(CallRPC("createrawtransaction [{\"txid\":\"a3b807410df0b60fcb9736768df5823938b2f838694939ba45f3c0a1bff150ed\",\"vout\":0}] {\"data\":\"12345g\"}"), std::runtime_error);

    // Data 81 bytes long
    BOOST_CHECK_NO_THROW(CallRPC("createrawtransaction [{\"txid\":\"a3b807410df0b60fcb9736768df5823938b2f838694939ba45f3c0a1bff150ed\",\"vout\":0}] {\"data\":\"010203040506070809101112131415161718192021222324252627282930313233343536373839404142434445464748495051525354555657585960616263646566676869707172737475767778798081\"}"));
}

BOOST_AUTO_TEST_CASE(rpc_format_monetary_values)
{
    BOOST_CHECK(ValueFromAmount(0LL).write() == "0.00000000");
    BOOST_CHECK(ValueFromAmount(1LL).write() == "0.00000001");
    BOOST_CHECK(ValueFromAmount(17622195LL).write() == "0.17622195");
    BOOST_CHECK(ValueFromAmount(50000000LL).write() == "0.50000000");
    BOOST_CHECK(ValueFromAmount(89898989LL).write() == "0.89898989");
    BOOST_CHECK(ValueFromAmount(100000000LL).write() == "1.00000000");
    BOOST_CHECK(ValueFromAmount(2099999999999990LL).write() == "20999999.99999990");
    BOOST_CHECK(ValueFromAmount(2099999999999999LL).write() == "20999999.99999999");

    BOOST_CHECK_EQUAL(ValueFromAmount(0).write(), "0.00000000");
    BOOST_CHECK_EQUAL(ValueFromAmount((COIN/10000)*123456789).write(), "12345.67890000");
    BOOST_CHECK_EQUAL(ValueFromAmount(-COIN).write(), "-1.00000000");
    BOOST_CHECK_EQUAL(ValueFromAmount(-COIN/10).write(), "-0.10000000");

    BOOST_CHECK_EQUAL(ValueFromAmount(COIN*100000000).write(), "100000000.00000000");
    BOOST_CHECK_EQUAL(ValueFromAmount(COIN*10000000).write(), "10000000.00000000");
    BOOST_CHECK_EQUAL(ValueFromAmount(COIN*1000000).write(), "1000000.00000000");
    BOOST_CHECK_EQUAL(ValueFromAmount(COIN*100000).write(), "100000.00000000");
    BOOST_CHECK_EQUAL(ValueFromAmount(COIN*10000).write(), "10000.00000000");
    BOOST_CHECK_EQUAL(ValueFromAmount(COIN*1000).write(), "1000.00000000");
    BOOST_CHECK_EQUAL(ValueFromAmount(COIN*100).write(), "100.00000000");
    BOOST_CHECK_EQUAL(ValueFromAmount(COIN*10).write(), "10.00000000");
    BOOST_CHECK_EQUAL(ValueFromAmount(COIN).write(), "1.00000000");
    BOOST_CHECK_EQUAL(ValueFromAmount(COIN/10).write(), "0.10000000");
    BOOST_CHECK_EQUAL(ValueFromAmount(COIN/100).write(), "0.01000000");
    BOOST_CHECK_EQUAL(ValueFromAmount(COIN/1000).write(), "0.00100000");
    BOOST_CHECK_EQUAL(ValueFromAmount(COIN/10000).write(), "0.00010000");
    BOOST_CHECK_EQUAL(ValueFromAmount(COIN/100000).write(), "0.00001000");
    BOOST_CHECK_EQUAL(ValueFromAmount(COIN/1000000).write(), "0.00000100");
    BOOST_CHECK_EQUAL(ValueFromAmount(COIN/10000000).write(), "0.00000010");
    BOOST_CHECK_EQUAL(ValueFromAmount(COIN/100000000).write(), "0.00000001");
}

static UniValue ValueFromString(const std::string &str)
{
    UniValue value;
    BOOST_CHECK(value.setNumStr(str));
    return value;
}

BOOST_AUTO_TEST_CASE(rpc_parse_monetary_values)
{
    BOOST_CHECK_THROW(AmountFromValue(ValueFromString("-0.00000001")), UniValue);
    BOOST_CHECK_EQUAL(AmountFromValue(ValueFromString("0")), 0LL);
    BOOST_CHECK_EQUAL(AmountFromValue(ValueFromString("0.00000000")), 0LL);
    BOOST_CHECK_EQUAL(AmountFromValue(ValueFromString("0.00000001")), 1LL);
    BOOST_CHECK_EQUAL(AmountFromValue(ValueFromString("0.17622195")), 17622195LL);
    BOOST_CHECK_EQUAL(AmountFromValue(ValueFromString("0.5")), 50000000LL);
    BOOST_CHECK_EQUAL(AmountFromValue(ValueFromString("0.50000000")), 50000000LL);
    BOOST_CHECK_EQUAL(AmountFromValue(ValueFromString("0.89898989")), 89898989LL);
    BOOST_CHECK_EQUAL(AmountFromValue(ValueFromString("1.00000000")), 100000000LL);
    BOOST_CHECK_EQUAL(AmountFromValue(ValueFromString("20999999.9999999")), 2099999999999990LL);
    BOOST_CHECK_EQUAL(AmountFromValue(ValueFromString("20999999.99999999")), 2099999999999999LL);

    BOOST_CHECK_EQUAL(AmountFromValue(ValueFromString("1e-8")), COIN/100000000);
    BOOST_CHECK_EQUAL(AmountFromValue(ValueFromString("0.1e-7")), COIN/100000000);
    BOOST_CHECK_EQUAL(AmountFromValue(ValueFromString("0.01e-6")), COIN/100000000);
    BOOST_CHECK_EQUAL(AmountFromValue(ValueFromString("0.0000000000000000000000000000000000000000000000000000000000000000000000000001e+68")), COIN/100000000);
    BOOST_CHECK_EQUAL(AmountFromValue(ValueFromString("10000000000000000000000000000000000000000000000000000000000000000e-64")), COIN);
    BOOST_CHECK_EQUAL(AmountFromValue(ValueFromString("0.000000000000000000000000000000000000000000000000000000000000000100000000000000000000000000000000000000000000000000000e64")), COIN);

    BOOST_CHECK_THROW(AmountFromValue(ValueFromString("1e-9")), UniValue); //should fail
    BOOST_CHECK_THROW(AmountFromValue(ValueFromString("0.000000019")), UniValue); //should fail
    BOOST_CHECK_EQUAL(AmountFromValue(ValueFromString("0.00000001000000")), 1LL); //should pass, cut trailing 0
    BOOST_CHECK_THROW(AmountFromValue(ValueFromString("19e-9")), UniValue); //should fail
    BOOST_CHECK_EQUAL(AmountFromValue(ValueFromString("0.19e-6")), 19); //should pass, leading 0 is present

    BOOST_CHECK_THROW(AmountFromValue(ValueFromString("92233720368.54775808")), UniValue); //overflow error
    BOOST_CHECK_THROW(AmountFromValue(ValueFromString("1e+11")), UniValue); //overflow error
    BOOST_CHECK_THROW(AmountFromValue(ValueFromString("1e11")), UniValue); //overflow error signless
    BOOST_CHECK_THROW(AmountFromValue(ValueFromString("93e+9")), UniValue); //overflow error
}

BOOST_AUTO_TEST_CASE(json_parse_errors)
{
    // Valid
    BOOST_CHECK_EQUAL(ParseNonRFCJSONValue("1.0").get_real(), 1.0);
    // Valid, with leading or trailing whitespace
    BOOST_CHECK_EQUAL(ParseNonRFCJSONValue(" 1.0").get_real(), 1.0);
    BOOST_CHECK_EQUAL(ParseNonRFCJSONValue("1.0 ").get_real(), 1.0);

    BOOST_CHECK_THROW(AmountFromValue(ParseNonRFCJSONValue(".19e-6")), std::runtime_error); //should fail, missing leading 0, therefore invalid JSON
    BOOST_CHECK_EQUAL(AmountFromValue(ParseNonRFCJSONValue("0.00000000000000000000000000000000000001e+30 ")), 1);
    // Invalid, initial garbage
    BOOST_CHECK_THROW(ParseNonRFCJSONValue("[1.0"), std::runtime_error);
    BOOST_CHECK_THROW(ParseNonRFCJSONValue("a1.0"), std::runtime_error);
    // Invalid, trailing garbage
    BOOST_CHECK_THROW(ParseNonRFCJSONValue("1.0sds"), std::runtime_error);
    BOOST_CHECK_THROW(ParseNonRFCJSONValue("1.0]"), std::runtime_error);
    // BTC addresses should fail parsing
    BOOST_CHECK_THROW(ParseNonRFCJSONValue("175tWpb8K1S7NmH4Zx6rewF9WQrcZv245W"), std::runtime_error);
    BOOST_CHECK_THROW(ParseNonRFCJSONValue("3J98t1WpEZ73CNmQviecrnyiWrnqRhWNL"), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(rpc_ban)
{
    BOOST_CHECK_NO_THROW(CallRPC(std::string("clearbanned")));

    UniValue r;
    BOOST_CHECK_NO_THROW(r = CallRPC(std::string("setban 127.0.0.0 add")));
    BOOST_CHECK_THROW(r = CallRPC(std::string("setban 127.0.0.0:8334")), std::runtime_error); //portnumber for setban not allowed
    BOOST_CHECK_NO_THROW(r = CallRPC(std::string("listbanned")));
    UniValue ar = r.get_array();
    UniValue o1 = ar[0].get_obj();
    UniValue adr = find_value(o1, "address");
    BOOST_CHECK_EQUAL(adr.get_str(), "127.0.0.0/32");
    BOOST_CHECK_NO_THROW(CallRPC(std::string("setban 127.0.0.0 remove")));
    BOOST_CHECK_NO_THROW(r = CallRPC(std::string("listbanned")));
    ar = r.get_array();
    BOOST_CHECK_EQUAL(ar.size(), 0U);

    BOOST_CHECK_NO_THROW(r = CallRPC(std::string("setban 127.0.0.0/24 add 9907731200 true")));
    BOOST_CHECK_NO_THROW(r = CallRPC(std::string("listbanned")));
    ar = r.get_array();
    o1 = ar[0].get_obj();
    adr = find_value(o1, "address");
    UniValue banned_until = find_value(o1, "banned_until");
    BOOST_CHECK_EQUAL(adr.get_str(), "127.0.0.0/24");
    BOOST_CHECK_EQUAL(banned_until.get_int64(), 9907731200); // absolute time check

    BOOST_CHECK_NO_THROW(CallRPC(std::string("clearbanned")));

    BOOST_CHECK_NO_THROW(r = CallRPC(std::string("setban 127.0.0.0/24 add 200")));
    BOOST_CHECK_NO_THROW(r = CallRPC(std::string("listbanned")));
    ar = r.get_array();
    o1 = ar[0].get_obj();
    adr = find_value(o1, "address");
    banned_until = find_value(o1, "banned_until");
    BOOST_CHECK_EQUAL(adr.get_str(), "127.0.0.0/24");
    int64_t now = GetTime();
    BOOST_CHECK(banned_until.get_int64() > now);
    BOOST_CHECK(banned_until.get_int64()-now <= 200);

    // must throw an exception because 127.0.0.1 is in already banned subnet range
    BOOST_CHECK_THROW(r = CallRPC(std::string("setban 127.0.0.1 add")), std::runtime_error);

    BOOST_CHECK_NO_THROW(CallRPC(std::string("setban 127.0.0.0/24 remove")));
    BOOST_CHECK_NO_THROW(r = CallRPC(std::string("listbanned")));
    ar = r.get_array();
    BOOST_CHECK_EQUAL(ar.size(), 0U);

    BOOST_CHECK_NO_THROW(r = CallRPC(std::string("setban 127.0.0.0/255.255.0.0 add")));
    BOOST_CHECK_THROW(r = CallRPC(std::string("setban 127.0.1.1 add")), std::runtime_error);

    BOOST_CHECK_NO_THROW(CallRPC(std::string("clearbanned")));
    BOOST_CHECK_NO_THROW(r = CallRPC(std::string("listbanned")));
    ar = r.get_array();
    BOOST_CHECK_EQUAL(ar.size(), 0U);


    BOOST_CHECK_THROW(r = CallRPC(std::string("setban test add")), std::runtime_error); //invalid IP

    //IPv6 tests
    BOOST_CHECK_NO_THROW(r = CallRPC(std::string("setban FE80:0000:0000:0000:0202:B3FF:FE1E:8329 add")));
    BOOST_CHECK_NO_THROW(r = CallRPC(std::string("listbanned")));
    ar = r.get_array();
    o1 = ar[0].get_obj();
    adr = find_value(o1, "address");
    BOOST_CHECK_EQUAL(adr.get_str(), "fe80::202:b3ff:fe1e:8329/128");

    BOOST_CHECK_NO_THROW(CallRPC(std::string("clearbanned")));
    BOOST_CHECK_NO_THROW(r = CallRPC(std::string("setban 2001:db8::/ffff:fffc:0:0:0:0:0:0 add")));
    BOOST_CHECK_NO_THROW(r = CallRPC(std::string("listbanned")));
    ar = r.get_array();
    o1 = ar[0].get_obj();
    adr = find_value(o1, "address");
    BOOST_CHECK_EQUAL(adr.get_str(), "2001:db8::/30");

    BOOST_CHECK_NO_THROW(CallRPC(std::string("clearbanned")));
    BOOST_CHECK_NO_THROW(r = CallRPC(std::string("setban 2001:4d48:ac57:400:cacf:e9ff:fe1d:9c63/128 add")));
    BOOST_CHECK_NO_THROW(r = CallRPC(std::string("listbanned")));
    ar = r.get_array();
    o1 = ar[0].get_obj();
    adr = find_value(o1, "address");
    BOOST_CHECK_EQUAL(adr.get_str(), "2001:4d48:ac57:400:cacf:e9ff:fe1d:9c63/128");
}

BOOST_AUTO_TEST_CASE(rpc_convert_values_generatetoaddress)
{
    UniValue result;

    BOOST_CHECK_NO_THROW(result = RPCConvertValues("generatetoaddress", {"101", "mkESjLZW66TmHhiFX8MCaBjrhZ543PPh9a"}));
    BOOST_CHECK_EQUAL(result[0].get_int(), 101);
    BOOST_CHECK_EQUAL(result[1].get_str(), "mkESjLZW66TmHhiFX8MCaBjrhZ543PPh9a");

    BOOST_CHECK_NO_THROW(result = RPCConvertValues("generatetoaddress", {"101", "mhMbmE2tE9xzJYCV9aNC8jKWN31vtGrguU"}));
    BOOST_CHECK_EQUAL(result[0].get_int(), 101);
    BOOST_CHECK_EQUAL(result[1].get_str(), "mhMbmE2tE9xzJYCV9aNC8jKWN31vtGrguU");

    BOOST_CHECK_NO_THROW(result = RPCConvertValues("generatetoaddress", {"1", "mkESjLZW66TmHhiFX8MCaBjrhZ543PPh9a", "9"}));
    BOOST_CHECK_EQUAL(result[0].get_int(), 1);
    BOOST_CHECK_EQUAL(result[1].get_str(), "mkESjLZW66TmHhiFX8MCaBjrhZ543PPh9a");
    BOOST_CHECK_EQUAL(result[2].get_int(), 9);

    BOOST_CHECK_NO_THROW(result = RPCConvertValues("generatetoaddress", {"1", "mhMbmE2tE9xzJYCV9aNC8jKWN31vtGrguU", "9"}));
    BOOST_CHECK_EQUAL(result[0].get_int(), 1);
    BOOST_CHECK_EQUAL(result[1].get_str(), "mhMbmE2tE9xzJYCV9aNC8jKWN31vtGrguU");
    BOOST_CHECK_EQUAL(result[2].get_int(), 9);
}

BOOST_AUTO_TEST_CASE(rpc_getblockstats_calculate_percentiles_by_weight)
{
    int64_t total_weight = 200;
    std::vector<std::pair<CAmount, int64_t>> feerates;
    CAmount result[NUM_GETBLOCKSTATS_PERCENTILES] = { 0 };

    for (int64_t i = 0; i < 100; i++) {
        feerates.emplace_back(std::make_pair(1 ,1));
    }

    for (int64_t i = 0; i < 100; i++) {
        feerates.emplace_back(std::make_pair(2 ,1));
    }

    CalculatePercentilesByWeight(result, feerates, total_weight);
    BOOST_CHECK_EQUAL(result[0], 1);
    BOOST_CHECK_EQUAL(result[1], 1);
    BOOST_CHECK_EQUAL(result[2], 1);
    BOOST_CHECK_EQUAL(result[3], 2);
    BOOST_CHECK_EQUAL(result[4], 2);

    // Test with more pairs, and two pairs overlapping 2 percentiles.
    total_weight = 100;
    CAmount result2[NUM_GETBLOCKSTATS_PERCENTILES] = { 0 };
    feerates.clear();

    feerates.emplace_back(std::make_pair(1, 9));
    feerates.emplace_back(std::make_pair(2 , 16)); //10th + 25th percentile
    feerates.emplace_back(std::make_pair(4 ,50)); //50th + 75th percentile
    feerates.emplace_back(std::make_pair(5 ,10));
    feerates.emplace_back(std::make_pair(9 ,15));  // 90th percentile

    CalculatePercentilesByWeight(result2, feerates, total_weight);

    BOOST_CHECK_EQUAL(result2[0], 2);
    BOOST_CHECK_EQUAL(result2[1], 2);
    BOOST_CHECK_EQUAL(result2[2], 4);
    BOOST_CHECK_EQUAL(result2[3], 4);
    BOOST_CHECK_EQUAL(result2[4], 9);

    // Same test as above, but one of the percentile-overlapping pairs is split in 2.
    total_weight = 100;
    CAmount result3[NUM_GETBLOCKSTATS_PERCENTILES] = { 0 };
    feerates.clear();

    feerates.emplace_back(std::make_pair(1, 9));
    feerates.emplace_back(std::make_pair(2 , 11)); // 10th percentile
    feerates.emplace_back(std::make_pair(2 , 5)); // 25th percentile
    feerates.emplace_back(std::make_pair(4 ,50)); //50th + 75th percentile
    feerates.emplace_back(std::make_pair(5 ,10));
    feerates.emplace_back(std::make_pair(9 ,15)); // 90th percentile

    CalculatePercentilesByWeight(result3, feerates, total_weight);

    BOOST_CHECK_EQUAL(result3[0], 2);
    BOOST_CHECK_EQUAL(result3[1], 2);
    BOOST_CHECK_EQUAL(result3[2], 4);
    BOOST_CHECK_EQUAL(result3[3], 4);
    BOOST_CHECK_EQUAL(result3[4], 9);

    // Test with one transaction spanning all percentiles.
    total_weight = 104;
    CAmount result4[NUM_GETBLOCKSTATS_PERCENTILES] = { 0 };
    feerates.clear();

    feerates.emplace_back(std::make_pair(1, 100));
    feerates.emplace_back(std::make_pair(2, 1));
    feerates.emplace_back(std::make_pair(3, 1));
    feerates.emplace_back(std::make_pair(3, 1));
    feerates.emplace_back(std::make_pair(999999, 1));

    CalculatePercentilesByWeight(result4, feerates, total_weight);

    for (int64_t i = 0; i < NUM_GETBLOCKSTATS_PERCENTILES; i++) {
        BOOST_CHECK_EQUAL(result4[i], 1);
    }
}

//! A reply written by building the reply object and writing it with UniValue::write().
static std::string ReplyByObject(const UniValue& result, const UniValue& error, const UniValue& id)
{
    return JSONRPCReplyObj(result, error, id).write() + "\n";
}

static std::string RandomString(FastRandomContext& rng)
{
    // Mostly the bytes that are escaped, the bytes around them and bytes of 0x80 and above.
    static const std::string special("\x00\x01\x08\t\n\x0b\x0c\r\x1f \"/\\\x7f\x80\xc3\xa9\xff", 18);
    std::string s;
    for (uint64_t n = rng.randrange(12); n > 0; --n) {
        s += rng.randbool() ? special[rng.randrange(special.size())] : char(rng.randbits(8));
    }
    return s;
}

static UniValue RandomValue(FastRandomContext& rng, int depth)
{
    switch (rng.randrange(depth > 0 ? 8 : 6)) {
    case 0: return UniValue{};
    case 1: return UniValue{rng.randbool()};
    case 2: return UniValue{int64_t(rng.rand64() >> rng.randrange(64))};
    case 3: return UniValue{double(int64_t(rng.rand64())) / (1.0 + rng.rand32())};
    case 4: {
        UniValue num;
        num.setNumStr(strprintf("%d.%010ue%d", int64_t(rng.rand64()), rng.rand32(), int(rng.randrange(600)) - 300));
        return num;
    }
    case 5: return UniValue{RandomString(rng)};
    case 6: {
        UniValue arr(UniValue::VARR);
        for (uint64_t n = rng.randrange(5); n > 0; --n) arr.push_back(RandomValue(rng, depth - 1));
        return arr;
    }
    default: {
        UniValue obj(UniValue::VOBJ);
        for (uint64_t n = rng.randrange(5); n > 0; --n) {
            // __pushKV keeps duplicate keys, which the writers must write as they are.
            obj.__pushKV(rng.randbool() ? RandomString(rng) : "k", RandomValue(rng, depth - 1));
        }
        return obj;
    }
    }
}

BOOST_AUTO_TEST_CASE(rpc_reply_write)
{
    std::vector<UniValue> values;
    values.emplace_back();
    values.emplace_back(true);
    values.emplace_back(false);
    values.emplace_back(UniValue::VBOOL, "x"); // written as false
    for (const int64_t n : {int64_t{0}, int64_t{1}, int64_t{-1}, std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max()}) {
        values.emplace_back(n);
    }
    values.emplace_back(std::numeric_limits<uint64_t>::max());
    values.emplace_back(std::numeric_limits<int>::min());
    for (const double d : {0.0, -0.0, 0.1, -1.5, 1.0 / 3, 1e300, -1e-300, 5e-324, 123456789012345678.0, 1e15, 1e16,
                           std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        values.emplace_back(d); // the last two have no number text and are written as nothing
    }
    // ValueFromAmount() negates a negative amount, so the lowest amount it takes is min() + 1.
    for (const CAmount amount : {CAmount{0}, CAmount{1}, CAmount{-1}, -COIN / 10, MAX_MONEY, std::numeric_limits<CAmount>::min() + 1}) {
        values.push_back(ValueFromAmount(amount));
    }
    for (const char* num : {"1e5", "-0", "1E+5", "0.000", "-1.5e-7", "123456789012345678901234567890"}) {
        UniValue value;
        BOOST_CHECK(value.setNumStr(num));
        values.push_back(value);
    }
    values.emplace_back(UniValue::VNUM, "not a number"); // written as it is
    std::string all_bytes;
    for (int c = 0; c < 256; ++c) {
        values.emplace_back(std::string(1, char(c)));
        values.emplace_back(std::string(1, char(c)) + "a" + std::string(2, char(c)));
        all_bytes += char(c);
    }
    values.emplace_back(all_bytes);
    values.emplace_back(std::string(all_bytes.rbegin(), all_bytes.rend()));
    values.emplace_back("");
    values.emplace_back("\"\\\"\\"); // escapes next to each other
    values.emplace_back("caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80"); // UTF-8
    values.emplace_back("\x80\xc3\xff\xfe"); // not UTF-8
    values.emplace_back(std::string(1000000, 'f') + "\n" + std::string(1000000, '0')); // long, as a block's hex
    values.emplace_back(UniValue::VARR);
    values.emplace_back(UniValue::VOBJ);
    UniValue keyed(UniValue::VOBJ);
    for (int c = 0; c < 256; ++c) keyed.pushKV(std::string(1, char(c)), c);
    keyed.pushKV("", all_bytes);
    keyed.__pushKV("same", 1);
    keyed.__pushKV("same", UniValue::VOBJ);
    values.push_back(keyed);
    UniValue array_with_keys(UniValue::VARR); // an array writes its values only
    array_with_keys.__pushKV("key", "value");
    array_with_keys.push_back(UniValue(UniValue::VOBJ));
    values.push_back(array_with_keys);
    UniValue failed_float(UniValue::VARR); // a failed setFloat() keeps the old values but is a number
    failed_float.push_back(1);
    failed_float.setFloat(std::numeric_limits<double>::quiet_NaN());
    values.push_back(failed_float);
    UniValue parsed;
    BOOST_CHECK(parsed.read("{\"a\":[1,-2.5e-10,true,null,\"\\u0000\\ud83d\\ude00\\/\"],\"b\":{},\"c\":[[]],\"\":\"\"}"));
    values.push_back(parsed);
    UniValue nested(UniValue::VARR);
    for (int i = 0; i < 200; ++i) {
        UniValue outer(UniValue::VOBJ);
        outer.pushKV("level", i);
        outer.pushKV("inner", nested);
        nested = UniValue(UniValue::VARR);
        nested.push_back(outer);
        nested.push_back(UniValue(UniValue::VARR));
    }
    values.push_back(nested);
    FastRandomContext rng{/* fDeterministic */ true};
    for (int i = 0; i < 1000; ++i) values.push_back(RandomValue(rng, 4));
    UniValue all_values(UniValue::VARR);
    UniValue all_keys(UniValue::VOBJ);
    for (const UniValue& v : values) {
        all_values.push_back(v);
        all_keys.__pushKV(v.write(), v);
    }
    values.push_back(all_values);
    values.push_back(all_keys);

    const std::vector<UniValue> errors{NullUniValue, JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter"),
                                       JSONRPCError(RPC_PARSE_ERROR, all_bytes), UniValue{false}, UniValue{0}, UniValue{""}, UniValue{UniValue::VOBJ}};
    for (const UniValue& value : values) {
        // The value as result, as error (the result is then written as null) and as id.
        for (const UniValue& error : errors) {
            BOOST_CHECK_EQUAL(JSONRPCReply(value, error, NullUniValue), ReplyByObject(value, error, NullUniValue));
            BOOST_CHECK_EQUAL(JSONRPCReply(value, error, 1), ReplyByObject(value, error, 1));
        }
        BOOST_CHECK_EQUAL(JSONRPCReply(NullUniValue, value, "id"), ReplyByObject(NullUniValue, value, "id"));
        BOOST_CHECK_EQUAL(JSONRPCReply(all_bytes, value, value), ReplyByObject(all_bytes, value, value));
        BOOST_CHECK_EQUAL(JSONRPCReply(true, NullUniValue, value), ReplyByObject(true, NullUniValue, value));
    }
    BOOST_CHECK_EQUAL(JSONRPCReply(NullUniValue, NullUniValue, NullUniValue), "{\"result\":null,\"error\":null,\"id\":null}\n");

    // JSONRPCAppendReply() appends to what is there.
    std::string out{"[1,"};
    JSONRPCAppendReply(out, all_values, NullUniValue, all_keys);
    BOOST_CHECK_EQUAL(out, "[1," + JSONRPCReplyObj(all_values, NullUniValue, all_keys).write());

    // An object left incomplete by a failed parse has a key without a value. Writing it fails in the same way.
    UniValue incomplete;
    BOOST_CHECK(!incomplete.read("[{\"a\":1,\"b\":"));
    std::string by_object, by_writer;
    try {
        ReplyByObject(incomplete, NullUniValue, 1);
    } catch (const std::out_of_range& e) {
        by_object = e.what();
    }
    try {
        JSONRPCReply(incomplete, NullUniValue, 1);
    } catch (const std::out_of_range& e) {
        by_writer = e.what();
    }
    BOOST_CHECK(!by_object.empty());
    BOOST_CHECK_EQUAL(by_writer, by_object);
}

BOOST_AUTO_TEST_CASE(rpc_reply_batch)
{
    if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
    util::Ref context{m_node};
    JSONRPCRequest jreq(context);
    UniValue batch;
    BOOST_CHECK(batch.read("[{\"method\":\"echo\",\"params\":[\"\\u0000\\\"\\u00ff\",{\"k\":[]},-0.5],\"id\":1},"
                           "{\"method\":\"getblockcount\",\"id\":\"\\n\"},{\"method\":\"getblockhash\",\"params\":[-1],\"id\":[]},"
                           "{\"method\":\"getblockhash\",\"params\":[\"x\"],\"id\":{\"a\":null}},{\"method\":\"nosuchmethod\",\"id\":null},"
                           "{\"method\":5,\"id\":2.5},{\"id\":true},[],1,{\"method\":\"echo\"},{\"method\":\"echo\",\"params\":3}]"));
    // The batch reply as it was written before: an array of reply objects, written by UniValue::write().
    UniValue replies(UniValue::VARR);
    for (const UniValue& req : batch.getValues()) {
        JSONRPCRequest one(jreq);
        try {
            one.parse(req);
            replies.push_back(JSONRPCReplyObj(tableRPC.execute(one), NullUniValue, one.id));
        } catch (const UniValue& error) {
            replies.push_back(JSONRPCReplyObj(NullUniValue, error, one.id));
        } catch (const std::exception& e) {
            replies.push_back(JSONRPCReplyObj(NullUniValue, JSONRPCError(RPC_PARSE_ERROR, e.what()), one.id));
        }
    }
    BOOST_CHECK_EQUAL(JSONRPCExecBatch(jreq, batch), replies.write() + "\n");
    BOOST_CHECK_EQUAL(JSONRPCExecBatch(jreq, UniValue{UniValue::VARR}), "[]\n");
}

BOOST_AUTO_TEST_SUITE_END()
