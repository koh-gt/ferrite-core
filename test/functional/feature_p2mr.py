#!/usr/bin/env python3
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the P2MR rules (BIP 360) in blocks and in the mempool.

Node 0 has the P2MR rules active from the start (-vbparams=p2mr:-1:0). Node 1 does not have them
and skips standardness (-acceptnonstdtxn=1); node 2 does not have them either and keeps the
default policy. All three nodes check scripts without the script check threads (-par=1), so that a
refused block or proposal names the script error.

The test checks:
- the project's spend vector (src/test/data/p2mr_spend.json), recomputed with the framework;
- the output type witness_v2_p2mr in the RPC results and REST replies that describe an output
  OP_2 <32 bytes>, and witness_unknown for other witness version 2 programs;
- spends that follow the rules: a leaf with a key and OP_CHECKSIG (also as the only witness
  spend of a block), a hash lock, a tree whose only leaf is its root, other leaf versions at
  depth one and at depth zero, and a spend with an annex; each one in a block on node 0 and in
  the mempool of each node, with the result the policy of that node gives;
- every way a spend can break the rules, refused in block proposals and through submitblock on
  node 0, and refused in the mempool of every node with its own reason;
- 120 random witnesses, chosen with a fixed seed, whose block proposals on node 0 give what an
  implementation of the rules written here predicts;
- a block with MWEB data that spends a P2MR output, refused when the spend breaks the rules.
"""

import copy
import http.client
import json
import os
import random
import urllib.parse

from test_framework.blocktools import add_witness_commitment, create_block
from test_framework.key import TaggedHash, compute_xonly_pubkey, sign_schnorr, verify_schnorr
from test_framework.messages import (
    COIN,
    CBlock,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxInWitness,
    CTxOut,
    FromHex,
    ToHex,
    ser_string,
    sha256,
)
from test_framework.script import (
    CScript,
    OP_0,
    OP_2,
    OP_CHECKSIG,
    OP_EQUAL,
    OP_FALSE,
    OP_RETURN,
    OP_SHA256,
    OP_SUBSTR,
    OP_TRUE,
    SIGHASH_DEFAULT,
    TaprootSignatureHash,
    p2mr_construct,
)
from test_framework.segwit_addr import encode_segwit_address
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_greater_than

OUTPUT_AMOUNT = COIN // 10
FEE = 10000
LEAF_TAPSCRIPT = 0xc0
FIRST_MWEB_HEIGHT = 432
RANDOM_SEED = 360
RANDOM_CASES = 120

# The key of the leaf that checks a signature, and the preimages of the hash locks. They are used by this test only.
LEAF_KEY = sha256(b"feature_p2mr.py leaf key")
PREIMAGE = sha256(b"feature_p2mr.py preimage")
LARGE_PREIMAGE = PREIMAGE * 2 + bytes(17)

# The script errors of the P2MR rules, as the node names them.
EMPTY_WITNESS = "Witness program was passed an empty witness"
WITNESS_SIZE = "P2MR witness must contain a script and a control block"
CONTROL_SIZE = "Invalid P2MR control block size"
CONTROL_PARITY = "P2MR control block must have its lowest bit set"
MISMATCH = "Witness program hash mismatch"
LEAF_VERSION_DISCOURAGED = "Taproot version reserved for soft-fork upgrades"
WITNESS_VERSION_DISCOURAGED = "Witness version reserved for soft-fork upgrades"
STACK_SIZE = "Stack size limit exceeded"
PUSH_SIZE = "Push value size limit exceeded"
OP_RETURN_ERROR = "OP_RETURN was encountered"
CLEANSTACK = "Stack size must be exactly one after execution"
EVAL_FALSE = "Script evaluated without error but finished with a false/empty top stack element"
SCHNORR_SIG = "Invalid Schnorr signature"

# The leaf scripts of the random witnesses, and what each one does when it is run as a tapscript.
LEAF_SCRIPTS = {
    "true": CScript([OP_TRUE]),
    "false": CScript([OP_FALSE]),
    "return": CScript([OP_RETURN]),
    "success": CScript([OP_RETURN, OP_SUBSTR]),  # OP_SUBSTR is one of the OP_SUCCESSx opcodes of BIP 342
}
LEAF_VERSIONS = [0xc0, 0xc2, 0x66, 0xfe, 0x00, 0x50]


def reason(error):
    return "non-mandatory-script-verify-flag ({})".format(error)


def leaf_hash(version, script):
    return TaggedHash("TapLeaf", bytes([version]) + ser_string(bytes(script)))


def branch_hash(a, b):
    return TaggedHash("TapBranch", a + b if a < b else b + a)


def tapscript_result(kind, stack):
    """The result of running one of LEAF_SCRIPTS as a tapscript on the initial stack (BIP 342)."""
    if kind == "success":
        return None
    if len(stack) > 1000:
        return STACK_SIZE
    if any(len(element) > 520 for element in stack):
        return PUSH_SIZE
    if kind == "return":
        return OP_RETURN_ERROR
    final = stack + [b"\x01" if kind == "true" else b""]
    if len(final) != 1:
        return CLEANSTACK
    if not any(final[0]):
        return EVAL_FALSE
    return None


def expected_result(program, witness, script_kinds):
    """What a block reports for a spend of OP_2 <program> with this witness: None if the spend follows the P2MR rules,
    otherwise the script error. script_kinds maps the bytes of each leaf script that may be run to its kind."""
    stack = list(witness)
    if len(stack) == 0:
        return EMPTY_WITNESS
    if len(stack) == 1:
        return WITNESS_SIZE
    if len(stack[-1]) > 0 and stack[-1][0] == 0x50:
        # The last element is an annex; with two elements there is no room for one.
        if len(stack) == 2:
            return WITNESS_SIZE
        stack.pop()
    control = stack.pop()
    script = stack.pop()
    if len(control) < 1 or len(control) > 1 + 32 * 128 or (len(control) - 1) % 32 != 0:
        return CONTROL_SIZE
    if control[0] & 1 != 1:
        return CONTROL_PARITY
    version = control[0] & 0xfe
    node = leaf_hash(version, script)
    for i in range(1, len(control), 32):
        node = branch_hash(node, control[i:i + 32])
    if node != program:
        return MISMATCH
    if len(control) == 1 or version != LEAF_TAPSCRIPT:
        # Not run: a leaf at depth zero, or a leaf version without rules yet.
        return None
    return tapscript_result(script_kinds[bytes(script)], stack)


class P2MRTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        self.extra_args = [
            ["-vbparams=p2mr:-1:0", "-par=1", "-rest"],
            ["-acceptnonstdtxn=1", "-par=1"],
            ["-par=1"],
        ]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        self.setup_nodes()
        self.connect_all()

    def connect_all(self):
        self.connect_nodes(0, 1)
        self.connect_nodes(0, 2)
        self.connect_nodes(1, 2)

    def disconnect_node_0(self):
        """Keep the blocks that node 0 refuses away from the nodes without the rules, which would accept them."""
        self.disconnect_nodes(0, 1)
        self.disconnect_nodes(0, 2)

    def fund(self, script_pubkeys, mine=True):
        """Pay OUTPUT_AMOUNT to each scriptPubKey in one transaction from the wallet of node 0, mine it unless mine is
        False, and return the outputs as (outpoint, CTxOut)."""
        node = self.nodes[0]
        needed = len(script_pubkeys) * OUTPUT_AMOUNT + COIN // 100
        coin = next(u for u in node.listunspent() if u['amount'] * COIN > needed)
        tx = CTransaction()
        tx.vin = [CTxIn(COutPoint(int(coin['txid'], 16), coin['vout']))]
        tx.vout = [CTxOut(OUTPUT_AMOUNT, script_pubkey) for script_pubkey in script_pubkeys]
        tx.vout.append(CTxOut(int(coin['amount'] * COIN) - needed, bytes.fromhex(node.getaddressinfo(node.getrawchangeaddress())['scriptPubKey'])))
        signed = node.signrawtransactionwithwallet(ToHex(tx))
        assert signed['complete']
        txid = int(node.sendrawtransaction(signed['hex']), 16)
        if mine:
            node.generate(1)
            self.sync_blocks()
        return [(COutPoint(txid, n), tx.vout[n]) for n in range(len(script_pubkeys))]

    def spend(self, utxo, witness):
        """A transaction that spends the output with this witness, or with a witness that make_witness(tx, spent output)
        gives when witness is a function."""
        outpoint, spent = utxo
        tx = CTransaction()
        tx.nVersion = 2
        tx.vin = [CTxIn(outpoint, b"", 0xfffffffd)]
        tx.vout = [CTxOut(spent.nValue - FEE, self.payout_script)]
        stack = witness(tx, spent) if callable(witness) else witness
        if stack:
            tx.wit.vtxinwit = [CTxInWitness()]
            tx.wit.vtxinwit[0].scriptWitness.stack = list(stack)
        tx.rehash()
        return tx

    def key_leaf_witness(self, tx, spent):
        leaf = self.tree.leaves["key"]
        sighash = TaprootSignatureHash(tx, [spent], SIGHASH_DEFAULT, 0, scriptpath=True, script=leaf.script)
        return [sign_schnorr(LEAF_KEY, sighash), leaf.script, leaf.control_block]

    def block(self, txs):
        """A block on the tip of node 0 with these transactions."""
        tmpl = self.nodes[0].getblocktemplate({'rules': ['segwit']})
        block = create_block(tmpl=tmpl, txlist=txs)
        add_witness_commitment(block)
        block.solve()
        return block

    def propose(self, block):
        return self.nodes[0].getblocktemplate({'data': block.serialize().hex(), 'mode': 'proposal', 'rules': ['segwit']})

    def mempool_results(self, tx):
        """The reject reason of each node's mempool for the transaction, or None where it is accepted."""
        results = []
        for node in self.nodes:
            result = node.testmempoolaccept([ToHex(tx)])[0]
            results.append(None if result['allowed'] else result['reject-reason'])
        return results

    def mine(self, txs):
        """Mine a block with these transactions on node 0 and have the other nodes follow."""
        block = self.block(txs)
        assert_equal(self.propose(block), None)
        assert_equal(self.nodes[0].submitblock(block.serialize().hex()), None)
        assert_equal(self.nodes[0].getbestblockhash(), block.hash)
        self.sync_blocks()
        return block

    def rest(self, path):
        url = urllib.parse.urlparse(self.nodes[0].url)
        conn = http.client.HTTPConnection(url.hostname, url.port)
        conn.request('GET', path)
        response = conn.getresponse()
        assert_equal(response.status, 200)
        return json.loads(response.read().decode('utf-8'))

    def run_test(self):
        node = self.nodes[0]
        self.payout_script = CScript([OP_0, sha256(b"feature_p2mr.py payout")])
        self.test_spend_vector()

        key_script = CScript([compute_xonly_pubkey(LEAF_KEY)[0], OP_CHECKSIG])
        hash_script = CScript([OP_SHA256, sha256(PREIMAGE), OP_EQUAL])
        large_hash_script = CScript([OP_SHA256, sha256(LARGE_PREIMAGE), OP_EQUAL])
        # A tree with leaves at depth 2 and 3: a key and OP_CHECKSIG, two hash locks, OP_TRUE and OP_FALSE. A tree with
        # two leaves at depth 1: OP_TRUE with leaf version 0xc2, and OP_TRUE. Trees whose only leaf is the root:
        # OP_RETURN (not run) and OP_TRUE with leaf version 0xc2.
        self.tree = p2mr_construct([("key", key_script), ("hash", hash_script), ("large hash", large_hash_script),
                                    ("true", CScript([OP_TRUE])), ("false", CScript([OP_FALSE]))])
        self.other_tree = p2mr_construct([("other", CScript([OP_TRUE]), 0xc2), ("true", CScript([OP_TRUE]))])
        self.root_tree = p2mr_construct([("only", CScript([OP_RETURN]))])
        self.root_tree_other = p2mr_construct([("only", CScript([OP_TRUE]), 0xc2)])
        # (A program of zero bytes would be false as a stack element, and its spends invalid for that reason.)
        self.v2_31 = CScript([OP_2, b"\x5a" * 31])

        self.log.info("Fund P2MR outputs and other witness version 2 outputs")
        node.generate(30)
        self.sync_blocks()
        funding = self.fund([self.tree.scriptPubKey] * 28 + [self.root_tree.scriptPubKey] * 4 + [self.root_tree_other.scriptPubKey] * 2 +
                            [self.other_tree.scriptPubKey] * 2 + [self.v2_31] * 2, mine=False)
        self.tree_utxos = funding[:28]
        self.root_utxos = funding[28:32]
        self.root_other_utxos = funding[32:34]
        self.other_utxos = funding[34:36]
        self.v2_31_utxos = funding[36:38]
        self.test_output_type(funding[0][0].hash, funding[36][0])

        self.test_spends_by_the_rules()
        self.test_spends_against_the_rules()
        self.test_random_witnesses()
        self.test_mweb_block()

    def test_spend_vector(self):
        self.log.info("The spend vector of the unit tests, recomputed with the framework")
        path = os.path.join(self.config["environment"]["SRCDIR"], "src", "test", "data", "p2mr_spend.json")
        with open(path, encoding="utf8") as f:
            vector = json.load(f)
        leaves = vector["given"]["scriptTree"]
        scripts = [bytes.fromhex(leaf["script"]) for leaf in leaves]
        assert all(leaf["leafVersion"] == LEAF_TAPSCRIPT for leaf in leaves)
        hashes = [leaf_hash(LEAF_TAPSCRIPT, script) for script in scripts]
        assert_equal([h.hex() for h in hashes], vector["intermediary"]["leafHashes"])
        tree = p2mr_construct([("0", CScript(scripts[0])), ("1", CScript(scripts[1]))])
        assert_equal(tree.merkle_root, branch_hash(hashes[0], hashes[1]))
        assert_equal(tree.merkle_root.hex(), vector["intermediary"]["merkleRoot"])
        assert_equal(bytes(tree.scriptPubKey).hex(), vector["expected"]["scriptPubKey"])
        assert_equal([tree.leaves["0"].control_block.hex(), tree.leaves["1"].control_block.hex()], vector["expected"]["scriptPathControlBlocks"])

        tx = FromHex(CTransaction(), vector["given"]["rawUnsignedTx"])
        spent = [CTxOut(utxo["amountSats"], bytes.fromhex(utxo["scriptPubKey"])) for utxo in vector["given"]["utxosSpent"]]
        assert_equal(spent[0].scriptPubKey, bytes(tree.scriptPubKey))
        pubkey = scripts[0][1:33]
        for spending in vector["inputSpending"]:
            given = spending["given"]
            assert_equal((given["txinIndex"], given["leafId"], given["hashType"]), (0, 0, 0))
            annex = bytes.fromhex(given["annex"]) if given["annex"] is not None else None
            sighash = TaprootSignatureHash(tx, spent, SIGHASH_DEFAULT, 0, scriptpath=True, script=CScript(scripts[0]), annex=annex)
            assert_equal(sighash.hex(), spending["intermediary"]["sigHash"])
            assert_equal(TaggedHash("TapSighash", bytes.fromhex(spending["intermediary"]["sigMsg"])), sighash)
            signature = bytes.fromhex(spending["expected"]["signature"])
            assert verify_schnorr(pubkey, signature, sighash)
            assert not verify_schnorr(pubkey, signature, sha256(sighash))
            witness = [signature, scripts[0], tree.leaves["0"].control_block] + ([annex] if annex is not None else [])
            assert_equal([w.hex() for w in witness], spending["expected"]["witness"])
        signed = FromHex(CTransaction(), vector["expected"]["rawSignedTx"])
        assert_equal([w.hex() for w in signed.wit.vtxinwit[0].scriptWitness.stack], vector["inputSpending"][0]["expected"]["witness"])

    def test_output_type(self, funding_txid, v2_31_outpoint):
        self.log.info("OP_2 <32 bytes> has the output type witness_v2_p2mr wherever an output is described")
        node = self.nodes[0]
        spk = bytes(self.tree.scriptPubKey).hex()
        address = encode_segwit_address("rfec", 2, self.tree.merkle_root)
        assert address.startswith("rfec1z")
        decoded = node.decodescript(spk)
        assert_equal(decoded['type'], 'witness_v2_p2mr')
        assert_equal(decoded['asm'], '2 ' + self.tree.merkle_root.hex())
        assert_equal(decoded['addresses'], [address])
        assert_equal(decoded['reqSigs'], 1)
        assert 'segwit' not in decoded
        assert_equal(node.decodescript(self.v2_31.hex())['type'], 'witness_unknown')
        assert_equal(node.decodescript(CScript([OP_2, bytes(33)]).hex())['type'], 'witness_unknown')
        assert 'witness_v1_taproot, witness_v2_p2mr, witness_mweb_pegin' in node.help('decodescript')
        # validateaddress is unchanged: a witness program of version 2.
        info = node.validateaddress(address)
        assert_equal((info['isvalid'], info['iswitness'], info['witness_version'], info['witness_program']), (True, True, 2, self.tree.merkle_root.hex()))

        txid = '{:064x}'.format(funding_txid)
        raw = node.getrawtransaction(txid)
        types = [out['scriptPubKey']['type'] for out in node.decoderawtransaction(raw)['vout']]
        assert_equal(types[:38], ['witness_v2_p2mr'] * 36 + ['witness_unknown'] * 2)
        assert_equal(self.rest('/rest/tx/{}.json'.format(txid))['vout'][0]['scriptPubKey']['type'], 'witness_v2_p2mr')
        block_hash = node.generate(1)[0]
        self.sync_blocks()
        assert_equal(node.getrawtransaction(txid, True, block_hash)['vout'][0]['scriptPubKey']['type'], 'witness_v2_p2mr')
        block_tx = next(tx for tx in node.getblock(block_hash, 2)['tx'] if tx['txid'] == txid)
        assert_equal(block_tx['vout'][0]['scriptPubKey']['type'], 'witness_v2_p2mr')
        assert_equal(block_tx['vout'][36]['scriptPubKey']['type'], 'witness_unknown')
        rest_tx = next(tx for tx in self.rest('/rest/block/{}.json'.format(block_hash))['tx'] if tx['txid'] == txid)
        assert_equal(rest_tx['vout'][0]['scriptPubKey']['type'], 'witness_v2_p2mr')
        assert_equal(node.gettxout(txid, 0)['scriptPubKey']['type'], 'witness_v2_p2mr')
        assert_equal(node.gettxout(txid, v2_31_outpoint.n)['scriptPubKey']['type'], 'witness_unknown')
        utxos = self.rest('/rest/getutxos/{}-0.json'.format(txid))['utxos']
        assert_equal(utxos[0]['scriptPubKey']['type'], 'witness_v2_p2mr')
        psbt = node.createpsbt([{'txid': txid, 'vout': 0}], [{address: 0.05}])
        decoded = node.decodepsbt(psbt)
        assert_equal(decoded['tx']['vout'][0]['scriptPubKey']['type'], 'witness_v2_p2mr')
        decoded = node.decodepsbt(node.utxoupdatepsbt(psbt))
        assert_equal(decoded['inputs'][0]['witness_utxo']['scriptPubKey']['type'], 'witness_v2_p2mr')

    def test_spends_by_the_rules(self):
        node, nonstandard, default = self.nodes
        not_standard = "bad-txns-nonstandard-inputs"
        tree = self.tree.leaves
        root = self.root_tree.leaves["only"]
        root_other = self.root_tree_other.leaves["only"]
        other_leaf = self.other_tree.leaves["other"]

        self.log.info("Spends of a leaf with a key and a signature, of a hash lock and of a tree whose only leaf is its root")
        spends = [
            self.spend(self.tree_utxos.pop(), self.key_leaf_witness),
            self.spend(self.tree_utxos.pop(), [PREIMAGE, tree["hash"].script, tree["hash"].control_block]),
            self.spend(self.root_utxos.pop(), [root.script, root.control_block]),
        ]
        for tx in spends:
            # Relayed where the rules apply and where standardness is not required; not standard elsewhere.
            assert_equal(self.mempool_results(tx), [None, None, not_standard])
        # A block whose only spend with a witness is a P2MR spend that checks a signature.
        block = self.mine([spends[0]])
        assert_equal(len(block.vtx), 2)
        # The node that skips standardness relays such a spend to the node with the rules.
        nonstandard.sendrawtransaction(ToHex(spends[1]))
        self.wait_until(lambda: spends[1].hash in node.getrawmempool())
        assert spends[1].hash not in default.getrawmempool()
        self.mine([spends[1], spends[2]])
        for n in self.nodes:
            assert_equal(n.getrawmempool(), [])

        self.log.info("Other leaf versions are valid in blocks and not relayed, at depth one and at depth zero")
        other = [
            self.spend(self.other_utxos.pop(), [other_leaf.script, other_leaf.control_block]),
            self.spend(self.root_other_utxos.pop(), [root_other.script, root_other.control_block]),
        ]
        assert_equal((len(other_leaf.control_block), len(root_other.control_block)), (33, 1))
        for tx in other:
            assert_equal(self.mempool_results(tx), [reason(LEAF_VERSION_DISCOURAGED)] * 2 + [not_standard])
        self.mine(other)

        self.log.info("An annex is valid in blocks and not standard")
        annexed = [
            self.spend(self.tree_utxos.pop(), [tree["true"].script, tree["true"].control_block, b"\x50"]),
            self.spend(self.tree_utxos.pop(), [tree["true"].script, tree["true"].control_block, b"\x50annex"]),
        ]
        for tx in annexed:
            assert_equal(self.mempool_results(tx), ["bad-witness-nonstandard", None, not_standard])
        self.mine(annexed)

        self.log.info("Initial stack elements of more than 80 bytes are valid in blocks and not standard, also at depth zero")
        assert_equal(len(LARGE_PREIMAGE), 81)
        leaf = tree["large hash"]
        large = [
            self.spend(self.tree_utxos.pop(), [LARGE_PREIMAGE, leaf.script, leaf.control_block]),
            # The leaf at depth zero is not run, but the limit is the same.
            self.spend(self.root_utxos.pop(), [bytes(81), root.script, root.control_block]),
        ]
        for tx in large:
            assert_equal(self.mempool_results(tx), ["bad-witness-nonstandard", None, not_standard])
        self.mine(large)

        self.log.info("Spends of other witness version 2 programs are not touched by the rules")
        tx = self.spend(self.v2_31_utxos.pop(), [b"\x01"])
        assert_equal(self.mempool_results(tx), [not_standard, reason(WITNESS_VERSION_DISCOURAGED), not_standard])
        self.mine([tx])

    def test_spends_against_the_rules(self):
        node = self.nodes[0]
        not_standard = "bad-txns-nonstandard-inputs"
        tree = self.tree.leaves
        true_leaf = tree["true"]
        root = self.root_tree.leaves["only"]

        def wrong_signature(tx, spent):
            witness = self.key_leaf_witness(tx, spent)
            witness[0] = witness[0][:-1] + bytes([witness[0][-1] ^ 1])
            return witness

        cleared = bytes([true_leaf.control_block[0] & 0xfe]) + true_leaf.control_block[1:]
        deep = bytes([LEAF_TAPSCRIPT | 1]) + bytes(32 * 129)
        # (script error, reason in the mempool of node 0 where its witness checks come first, utxos, witness)
        cases = [
            (EMPTY_WITNESS, None, self.tree_utxos, []),
            (WITNESS_SIZE, "bad-witness-nonstandard", self.tree_utxos, [true_leaf.script]),
            (WITNESS_SIZE, "bad-witness-nonstandard", self.tree_utxos, [true_leaf.script, b"\x50"]),
            (CONTROL_SIZE, None, self.tree_utxos, [true_leaf.script, true_leaf.control_block + b"\x00"]),
            (CONTROL_SIZE, None, self.tree_utxos, [true_leaf.script, true_leaf.control_block[:-1]]),
            (CONTROL_SIZE, "bad-witness-nonstandard", self.tree_utxos, [true_leaf.script, b""]),
            (CONTROL_SIZE, None, self.tree_utxos, [true_leaf.script, deep]),
            (CONTROL_PARITY, None, self.tree_utxos, [true_leaf.script, cleared]),
            (CONTROL_PARITY, "bad-witness-nonstandard", self.tree_utxos, [true_leaf.script, cleared, b"\x50"]),
            (CONTROL_PARITY, None, self.root_utxos, [root.script, b"\xc0"]),
            (MISMATCH, None, self.tree_utxos, [tree["key"].script, true_leaf.control_block]),
            (MISMATCH, None, self.tree_utxos, [true_leaf.script, true_leaf.control_block[:-1] + bytes([true_leaf.control_block[-1] ^ 1])]),
            (MISMATCH, None, self.root_utxos, [CScript([OP_TRUE]), root.control_block]),
            (SCHNORR_SIG, None, self.tree_utxos, wrong_signature),
            (EVAL_FALSE, None, self.tree_utxos, [tree["false"].script, tree["false"].control_block]),
            (CLEANSTACK, None, self.tree_utxos, [b"\x01", true_leaf.script, true_leaf.control_block]),
            (PUSH_SIZE, "bad-witness-nonstandard", self.tree_utxos, [bytes(521), true_leaf.script, true_leaf.control_block]),
        ]
        self.log.info("Spends against the rules are refused in blocks, with the script error, and in every mempool")
        self.disconnect_node_0()
        tip = node.getbestblockhash()
        for error, node_0_reason, utxos, witness in cases:
            tx = self.spend(utxos.pop(), witness)
            self.log.debug("{}: {}".format(error, [w.hex()[:20] for w in tx.wit.vtxinwit[0].scriptWitness.stack] if tx.wit.vtxinwit else []))
            assert_equal(self.mempool_results(tx), [node_0_reason or reason(error), reason(error), not_standard])
            block = self.block([tx])
            assert_equal(self.propose(block), reason(error))
            assert_equal(node.submitblock(block.serialize().hex()), reason(error))
            assert_equal(node.getbestblockhash(), tip)
            assert_equal([t['status'] for t in node.getchaintips() if t['hash'] == block.hash], ['invalid'])
        self.connect_all()
        self.sync_blocks()

    def test_random_witnesses(self):
        self.log.info("{} random witnesses (seed {}) against an implementation of the rules here, through block proposals".format(RANDOM_CASES, RANDOM_SEED))
        rng = random.Random(RANDOM_SEED)

        def random_bytes(n):
            return bytes(rng.getrandbits(8) for _ in range(n))

        # Trees with one leaf at the given depth: (kind of script, leaf version, depth).
        shapes = [("true", 0xc0, 0), ("true", 0xc0, 1), ("true", 0xc0, 2), ("true", 0xc0, 7), ("true", 0xc0, 128),
                  ("false", 0xc0, 1), ("return", 0xc0, 0), ("return", 0xc0, 2), ("success", 0xc0, 1), ("success", 0xc0, 0),
                  ("true", 0xc2, 1), ("return", 0x66, 0), ("true", 0xfe, 2), ("false", 0x00, 1), ("true", 0x50, 1),
                  ("return", 0x50, 0)]
        trees = []
        for kind, version, depth in shapes:
            path = [random_bytes(32) for _ in range(depth)]
            node = leaf_hash(version, LEAF_SCRIPTS[kind])
            for sibling in path:
                node = branch_hash(node, sibling)
            trees.append({"kind": kind, "script": bytes(LEAF_SCRIPTS[kind]), "control": bytes([version | 1]) + b"".join(path),
                          "program": node, "script_pubkey": CScript([OP_2, node])})
        utxos = self.fund([tree["script_pubkey"] for tree in trees])
        script_kinds = {bytes(script): kind for kind, script in LEAF_SCRIPTS.items()}

        def random_element():
            return random_bytes(rng.choice([0, 1, 32, 80, 81, 520, 521]))

        def random_witness(tree):
            script, control = tree["script"], tree["control"]
            stack = [random_element() for _ in range(rng.choice([0, 0, 0, 1, 2, 3]))]
            change = rng.choice(["none", "none", "none", "parity", "path", "script", "version", "size", "empty", "deep",
                                 "annex", "annex", "fifty", "one", "nothing"])
            if change == "parity":
                control = bytes([control[0] & 0xfe]) + control[1:]
            elif change == "path":
                if len(control) > 1:
                    i = rng.randrange(1, len(control))
                    control = control[:i] + bytes([control[i] ^ (1 << rng.randrange(8))]) + control[i + 1:]
                else:
                    script = script + b"\x51"
            elif change == "script":
                script = bytes(LEAF_SCRIPTS[rng.choice(sorted(LEAF_SCRIPTS))])
            elif change == "version":
                control = bytes([rng.choice(LEAF_VERSIONS) | 1]) + control[1:]
            elif change == "size":
                control = control + random_bytes(rng.randrange(1, 32)) if rng.random() < 0.5 or len(control) == 1 else control[:-rng.randrange(1, min(32, len(control)))]
            elif change == "empty":
                control = b""
            elif change == "deep":
                control = bytes([control[0]]) + random_bytes(32 * 129)
            if change == "one":
                return [rng.choice([script, control, b"\x50", random_element()])]
            if change == "nothing":
                return []
            if change == "fifty":
                return [script, b"\x50" + random_bytes(rng.randrange(0, 4))]
            witness = stack + [script, control]
            if change == "annex":
                witness.append(b"\x50" + random_bytes(rng.randrange(0, 40)))
            return witness

        outcomes = {}
        for i in range(RANDOM_CASES):
            n = rng.randrange(len(trees))
            witness = random_witness(trees[n])
            expected = expected_result(trees[n]["program"], witness, script_kinds)
            result = self.propose(self.block([self.spend(utxos[n], witness)]))
            assert_equal(result, None if expected is None else reason(expected))
            outcomes[expected] = outcomes.get(expected, 0) + 1
        self.log.info("Outcomes: {}".format(sorted(outcomes.items(), key=lambda item: str(item[0]))))
        # The seed covers every kind of result.
        for expected in [None, EMPTY_WITNESS, WITNESS_SIZE, CONTROL_SIZE, CONTROL_PARITY, MISMATCH, PUSH_SIZE, CLEANSTACK, OP_RETURN_ERROR]:
            assert_greater_than(outcomes.get(expected, 0), 0)

    def test_mweb_block(self):
        self.log.info("With MWEB active, a block that spends a P2MR output is checked under the rules")
        node, other_pool, _ = self.nodes
        node.generate(FIRST_MWEB_HEIGHT - 1 - node.getblockcount())
        node.sendtoaddress(node.getnewaddress(address_type='mweb'), 1)
        node.generate(2)
        self.sync_blocks()
        assert_equal(node.getblockcount(), FIRST_MWEB_HEIGHT + 1)
        assert_equal(len(node.getblock(node.getbestblockhash())['tx']), 2)  # the coinbase and the HogEx

        true_leaf = self.tree.leaves["true"]
        cleared = bytes([true_leaf.control_block[0] & 0xfe]) + true_leaf.control_block[1:]
        bad = self.spend(self.tree_utxos.pop(), [true_leaf.script, cleared])
        good = self.spend(self.tree_utxos.pop(), self.key_leaf_witness)
        # A block that a node without the rules mines, with each spend put in before the HogEx.
        self.disconnect_node_0()
        mined = FromHex(CBlock(), other_pool.getblock(other_pool.generate(1)[0], 0))
        assert mined.mweb_block is not None
        tip = node.getbestblockhash()
        for tx, result in [(bad, reason(CONTROL_PARITY)), (good, None)]:
            block = copy.deepcopy(mined)
            block.vtx.insert(len(block.vtx) - 1, tx)
            add_witness_commitment(block)
            block.solve()
            assert_equal(node.submitblock(block.serialize().hex()), result)
        assert_equal(node.getblock(node.getbestblockhash())['tx'][1], good.hash)
        assert tip != node.getbestblockhash()
        self.connect_all()
        node.generate(1)
        self.sync_blocks()


if __name__ == '__main__':
    P2MRTest().main()
