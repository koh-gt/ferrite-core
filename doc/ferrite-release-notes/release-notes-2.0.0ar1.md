Experimental!
Pre-release build of Ferrite Core v2.0.0 built on updated Litecoin 21 with MWEB and Taproot enabled.

New features:
Taproot and MWEB can start as early after block 200,000, if 38 or more blocks of every 40 block interval (95%) are mined with consensus bits of 2 and 4 for Taproot and MWEB respectively. 
This means that the network is ready for Taproot/MWEB earlier than expected. 
This feature will start no earlier than May 2023.

Taproot timeout height is block 700,000. After this date Taproot will be automatically enabled. 
This will happen no earlier than Apr 2024.

MWEB timeout height is block 1,200,000. After this date MWEB will be automatically enabled. 
This will happen no earlier than Mar 2025.

Full multi wallet support
Multi-signature transactions support
Privacy mode to hide recent transactions and current balance


## What's Changed
* ferrite by @koh-gt in https://github.com/koh-gt/ferrite-core/pull/35
**Full Changelog**: https://github.com/koh-gt/ferrite-core/commits/v2.0.0-ar1