Changes in v3.0.1
Built in addnodes, small fixes. 
Testnet 4 launched with DGWv3, Taproot and MWEB support enabled. 

From version 3.0.0r1
> ### Exchanges and miners are encouraged to upgrade to v3.0.0 or v3.0.1 when possible
> Given the average block time has slowed from 70-80s to 300-600s in the past 10,000 blocks, this would provide about 400 days of time for the difficulty algorithm hard fork.
> 
> **No coin holdings will be lost for late upgrading.** 
> However, coins mined using v1 and v2 will not be recognised after block 250,000 by v3, as such, some transactions made on old wallets may not be valid. Simply download ferrite-qt and your funds will be reflected. As the block emission schedule is unchanged, all funds stored or mined before this height will be safely compatible with v3.0.0 even in the case of late upgrading.
> 
> > Alongside this, the DAA aims to make sudden difficulty drops and spikes avoidable. For instance, the network will adjust difficulty rapidly when the hashrate changes exponentially, while also avoiding feedback oscillations. -[bitcoin.com 20171113](https://news.bitcoin.com/bitcoin-cash-network-completes-a-successful-hard-fork/)

## What's Changed
* v3.0.1 by @koh-gt in https://github.com/koh-gt/ferrite-core/pull/48
* V3.0.1 #2 by @koh-gt in https://github.com/koh-gt/ferrite-core/pull/49


**Full Changelog**: https://github.com/koh-gt/ferrite-core/compare/v3.0.0...v3.0.1r1