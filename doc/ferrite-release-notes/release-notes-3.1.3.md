### This is a revision of Ferrite Core v3.1.2 based on v3.1.3r1 changes.
Refinement of v3.1.2 - minor improvements.

# v3.1.3 wallets
| <img alt=download-logo-1 src=https://github.com/koh-gt/ferrite-core/assets/101822992/ce12f90e-286d-4229-b5db-db4c462e6256 height=32> <br> `-        Windows        -` | SHA256 Hash |
|-|-|
| [<img alt=download-1 src=https://github.com/koh-gt/ferrite-core/assets/101822992/70ae208a-caaa-40ff-856c-42e87745e8c3 height=32>](https://github.com/koh-gt/ferrite-core/releases/download/v3.1.3/ferrite-3.1.3-win64-setup.exe) | `e02f6c70952947004ad95b0bac7f5316a4a7f1d74af0a4a216f9d3fc73502117` <br> **New install** _ferrite-3.1.3-win64-setup.exe_ |
| [<img alt=download-2 src=https://github.com/koh-gt/ferrite-core/assets/101822992/70ae208a-caaa-40ff-856c-42e87745e8c3 height=32>](https://github.com/koh-gt/ferrite-core/releases/download/v3.1.3/ferrite-qt.exe) |  `b5c7300572229fa63bee1784dbf94c7826f8dc99a9b8996be9f60db8e6b62447` <br> **Wallet Update** _ferrite-qt.exe_ |
| [<img alt=download-3 src=https://github.com/koh-gt/ferrite-core/assets/101822992/70ae208a-caaa-40ff-856c-42e87745e8c3 height=32>](https://github.com/koh-gt/ferrite-core/releases/download/v3.1.3/ferrite-win.7z) | `df7bea12739b57a038e172634fb87808983ed2591a8f553c67d0f277f96cacdf` <br> **Server** |

| <img alt=download-logo-1 src=https://github.com/koh-gt/ferrite-core/assets/101822992/c07963c6-718e-4ee3-a9d5-a9a96010f99a height=32> <br> `-         MacOS         -` | SHA256 Hash |
|-|-|
|  [<img alt=download-1 src=https://github.com/koh-gt/ferrite-core/assets/101822992/70ae208a-caaa-40ff-856c-42e87745e8c3 height=32>](https://github.com/koh-gt/ferrite-core/releases/download/v3.1.3/Ferrite-Qt.dmg)               | `e076cfd1515066708fc2b9417d13311ea1cbcf16102962b6b8bf9dfadcda6003` <br> **Wallet** _Ferrite-Qt.dmg_      | 
|  [<img alt=download-1 src=https://github.com/koh-gt/ferrite-core/assets/101822992/70ae208a-caaa-40ff-856c-42e87745e8c3 height=32>](https://github.com/koh-gt/ferrite-core/releases/download/v3.1.3/Ferrite-Mac.7z)               | `335a9d3f41682f67ac2ac82f5832b4ea26ad0e8c215eb10db3b9f1f66cb918d6` <br> **Server**  |

| <img alt=download-logo-1 src=https://github.com/koh-gt/ferrite-core/assets/101822992/1c13b30a-95de-43bc-82c7-864a86e4658c height=32> <br> `-         Linux         -` | SHA256 Hash |
|-|-|
|  [<img alt=download-1 src=https://github.com/koh-gt/ferrite-core/assets/101822992/70ae208a-caaa-40ff-856c-42e87745e8c3 height=32>](https://github.com/koh-gt/ferrite-core/releases/download/v3.1.3/ferrite-qt-linux.7z)           | `862d806b2e6ee3fd4a89ec365de46357b2f1b389db46fc0478c3b312ec1c2268` <br> **Wallet** _ferrite-qt_   |
|  [<img alt=download-1 src=https://github.com/koh-gt/ferrite-core/assets/101822992/70ae208a-caaa-40ff-856c-42e87745e8c3 height=32>](https://github.com/koh-gt/ferrite-core/releases/download/v3.1.3/ferrite-linux.7z)               | `c23cbe7b379db9b902aa3d20128b810496fe1b8c7ed8cf9711d7b106618e5115` <br> **Server**  |


## Changes in v3.1.3
### Nodes update
> Latest post-MWEB checkpoint at height 154,000 for efficient transaction verification.
> Added new active nodes.
### Documentation and links update
> Branding and code cleanup deep within repository
> Added Ferrite Forum links and updated new links of website and block explorer.

## Future changes
v4.0.0a FEXT Ferrite Core implementation
Checkpoints for block 200,000 and 250,000 (important).
Bitcoin 24.x updates

## Mining
[**List of pools and settings**](https://github.com/koh-gt/ferrite-core/wiki/Mining-Pools-List)
Ferrite runs on Scrypt algorithm -compatible with Litecoin/Dogecoin miners
**Scrypt is not an ASIC-resistant algorithm.**
Scrypt ASIC mining is recommended. You can opt to [rent an Scrypt ASIC](https://github.com/koh-gt/ferrite-core/wiki/Rent-an-ASIC-miner).

## Other notes (on v1, v2 wallets before _13 Apr 2022_).
### All versions of Ferrite Core **wallets** will maintain backwards compatibility
> At height 250,000 all Ferrite will be split into 1 Ferrite (FEC) and 1 Ferrite Classic (FECC). v1.x.x and v2.x.x will only be able to spend the FECC portion, while v3+ will only be able to spend the FEC portion. 
Do not be alarmed if coins sent through v1.x.x and v2.x.x (Ferrite Classic) do not appear in your v3+ (Ferrite) balance - your Ferrite balance is unchanged.
Coins sent to Ferrite Classic through Ferrite will not show up, but are not lost. Ferrite Classic users need to copy their wallet to a v3+ Ferrite Core client to be able to access the coins. 
One can attempt to run both Ferrite Classic and Ferrite after Ferrite Classic is launched. Ferrite Classic may encounter hashrate instability.


* Windows and Linux builds compiled on Ubuntu Linux 22.10 Jammy Jellyfish by kohgt
* Mac OS builds compiled on Mac OS by otonadev
Uploaded on 30 Nov 2023.

## What's Changed
* V3.1.3 r by @koh-gt in https://github.com/koh-gt/ferrite-core/pull/55
* Fix typos by @omahs in https://github.com/koh-gt/ferrite-core/pull/56
* Ferrite v3.1.3 by @koh-gt in https://github.com/koh-gt/ferrite-core/pull/57

## New Contributors
* @omahs made their first contribution in https://github.com/koh-gt/ferrite-core/pull/56

**Full Changelog**: https://github.com/koh-gt/ferrite-core/compare/ferrite-backup-3.1.2...ferrite-backup-3.1.3