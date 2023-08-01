Added checkpoint at height 149,000
Adjusted pruning height to 500,000

[Updates from Litecoin 0.21.2.2](https://github.com/litecoin-project/litecoin/releases/tag/v0.21.2.2)

Important Security Updates

This release contains fixes that harden node and network security. These fixes are important for every node operator and wallet user.
Limit and tightly manage memory usage in events of high network traffic or when connected to extremely slow peers. This protects nodes on lower end hardware to not run out of memory in the face of increased network activity.

RPC API Changes

Added addconnection for use by functional tests.
getpeerinfo provides 2 new fields per peer, addr_processed and addr_rate_limited, that track addr message processing.

## What's Changed
* v3.1.0 by @koh-gt in https://github.com/koh-gt/ferrite-core/pull/50
* Ferrite v3.1.1 by @koh-gt in https://github.com/koh-gt/ferrite-core/pull/52

**Full Changelog**: https://github.com/koh-gt/ferrite-core/compare/v3.1.0...v3.1.1r1