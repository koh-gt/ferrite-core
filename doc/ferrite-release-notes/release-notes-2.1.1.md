### This is a revision of Ferrite Core v2.1.0 based on v2.1.1r1 and v2.1.1r2 changes. 

Checkpoint for block 100,000 - All transactions before height 100,000 are automatically deemed valid with consensus of over 10,000 block confirmations. 
Pruning option can now be enabled - Faster synchronisation and less data storage required from only storing the blocks from a few days back instead of the entire block history. Not suitable for hosting block explorers, mining pools or for local mining. May be more susceptible to forking and consensus attacks. Disabled by default.

Default configuration file - Creates a default configuration file containing addnodes and settings to reduce block propagation latency, allowing for reduced rate of detached blocks when mining. This also allows for faster initial synchronisation speeds.

**Forwarding your external WAN port 9574 at your router allows your node to broadcast block information to miners faster, reducing the risk of another miner finding a valid block at the same block height during the block propagation process.** 
> Forwarding port 9573 on your router can reduce propagation time by 500-2000 milliseconds, providing a 1-3% increase in mining output.


32 bit binaries will be released soon. For now the latest 32 bit release is v1.3.1 which is the same as in release v2.1.0.

### v2.1.1 wallets
| OS                        | Bit      | Name                   | Link                              | SHA256 Checksum                         |
|--------------------|-------|---------------------|--------------------------|---------------------------------------|
| Windows             | 64     | Setup                     | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v2.1.1/ferrite-2.1.1-win64-setup.exe) | `57193a922cb8fdc1cd03d5a759a2e8de7e307e27b566eba43dcb2330ebef7215` |
| Windows             | 64     | Qt Wallet               | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v2.1.1/ferrite-qt.exe) |  `507f520cd6e726dff458ae86b57c5c36abdc7870b8dc2bd8522f2a74d99d8998` |
| Windows             | 64     | Qt + CLI               | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v2.1.1/ferrite-2.1.1.7z) | `8ba2a7bf468eca76876a8d87406a5a1b549357961238a3ac3e058edb13a4f553` |
| Unix                     | 64     | Qt Wallet                | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v2.1.1/ferrite-qt.7z) | `9dd14260698c8d1b045ff9fc3107bd082128c1987c5176b811ae07b36232ccbc` |
| Unix                     |  64     | Qt + CLI                | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v2.1.1/ferrite-2.1.1-unix.7z)  | `8422a6c52e1d856c74e1f7ef09feb06b0e09d3482c060d44a8bba40e70b0927e` |

### v1.3.1 wallet for 32-bit (old release)
| OS                        | Bit      | Name                   | Link                              | SHA256 Checksum                         |
|--------------------|-------|---------------------|--------------------------|---------------------------------------|
| Windows             | 32     | Setup                     | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v2.0.0/ferrite-1.3.1-win32-setup.exe) | `fef28d405e7ec2c2061ec7771c7e72323a3de8c6f2709595fe76c4699e038477` |
| Windows             | 32     | Qt Wallet                | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v2.0.0/ferrite-qt-x86.exe) | `f8055d287b89fa22987a0d886e0ac18953d6b7363ddfa10f2a9b6542acaa9ee2` |
| Windows             | 32     | Qt + CLI               | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v2.0.0/ferrite-1.3.1.7z) | `f60353297a737d906e8582fab7565b265aaafce99f4bbf238db597369f2e9af3` |


## Mining
### [List of pools and settings](https://github.com/koh-gt/ferrite-core/wiki/Mining-Pools-List)
| Miner                  | Type                     | Link                             | SHA256 Checksum                                                                |
|-------------------|---------------------|-------------------------|-----------------------------------------------------------------|
| CCMiner             | ASIC / GPU / CPU | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v1.3.0/ferrite-pool-miner-1.3.0.7z) | `6799b0d090e3007d5cebb8485744c0d07a68c5dd0f4df84c17198ba94dac2ffd` |

## Changes
Optional to mine with MWEB support.
Small changes.

### Future changes
Checkpoints for block 150,000.
New tab for wallet management?

Included in this release is a stratum-pool miner based on [CCMiner v2.3.1 by @tpruvot](https://github.com/tpruvot/ccminer/releases/tag/2.3.1-tpruvot) with new pool configurations.
ASIC mining is recommended over GPU and CPU mining. You can opt to rent an Scrypt ASIC.

Everyone is invited to pull ferrite-core to suggest feature updates/chain parameters.

* Compiled on Ubuntu Linux 22.10 by @koh-gt
Uploaded on 11 Mar 2023.

## What's Changed
* v2.1.1 - Default config file, checkpoint by @koh-gt in https://github.com/koh-gt/ferrite-core/pull/44
* v2.1.1 #2 by @koh-gt in https://github.com/koh-gt/ferrite-core/pull/45

**Full Changelog**: https://github.com/koh-gt/ferrite-core/compare/v2.1.0...v2.1.1