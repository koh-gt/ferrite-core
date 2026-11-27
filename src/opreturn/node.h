// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_OPRETURN_NODE_H
#define BITCOIN_OPRETURN_NODE_H

#include <opreturn/options.h>

class ArgsManager;
struct NodeContext;

/**
 * How a program keeps the OP_RETURN record: its options, and the chain client through which init verifies, loads,
 * starts, flushes and stops it. Only a program that shows the record in a GUI tab calls these; the node programs
 * keep no record.
 */
namespace opreturn {

/**
 * Registers the record's options with their help texts, and the second name of each, which works as the first and
 * which -help does not show. For a program that shows the record in a GUI tab; called once, before the arguments
 * are parsed.
 */
void AddArgs(ArgsManager& args);

/**
 * Called once, before init, by a program that shows the record in a GUI tab: the record is then kept by default,
 * except with -disablewallet, the tab's stored choices count, and Construct makes the chain client also while the
 * record is off, so that the tab can switch it on. Tests may set it back.
 */
void SetGuiHost(bool gui_host = true);

/**
 * The facts of the node that the record's defaults depend on: -disablewallet from `args`, and the prune mode and
 * target as the node's base initialization has set them from -prune.
 */
NodeFacts ReadNodeFacts(const ArgsManager& args);

/**
 * Resolves the record's options (ResolveOptions, with ReadNodeFacts and the tab's stored choices) and makes its
 * chain client, which it appends to node.chain_clients and whose record GetRecord then returns. Called after the
 * node's base initialization has set the prune mode and before init calls the clients' verify(). For a GUI host the
 * client is always made; otherwise only while the record is on. A value of an option that may not be set is
 * reported by the client's verify(), which returns false after InitError, so that start-up stops as for any bad
 * option; such a client has no record. Never throws, and takes no node lock; returns false, appending nothing, only
 * when the client could not be made.
 *
 * Init calls the client's verify(), load(), start(), flush() and stop() in that order (flush() and stop() also
 * after a start-up that failed), and lets go of it in Shutdown after stop(), outside cs_main. stop() waits for the
 * record's worker, which may itself be waiting for cs_main, so it is never called under cs_main. Letting go of the
 * client neither stops the record nor waits for its worker. Until init has called load(), the record ignores a
 * switch or a rebuild that the tab asks for.
 */
bool Construct(NodeContext& node, const StoredChoices& stored);

} // namespace opreturn

#endif // BITCOIN_OPRETURN_NODE_H
