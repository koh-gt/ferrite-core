### This is a revision of Ferrite Core v3.1.3 based on v3.1.4r1 changes.
Refinement of v3.1.3 - minor improvements.

### Notice for Ubuntu LTS versions
In the event you are running an old version of Ubuntu (16 Xenial or older), you may encounter these issues with the missing dynamically linked library missing from your install of Ubuntu.
```bash
# error
Error while loading shared libraries: libzmq.so.5: cannot open shared object file: No such file or directory
# in this case you should install the missing ZeroMQ library 
apt-get install libzmq3-dev
```


# v3.1.4 wallets
| <img alt=download-logo-1 src=https://github.com/koh-gt/ferrite-core/assets/101822992/ce12f90e-286d-4229-b5db-db4c462e6256 height=32> <br> `-        Windows        -` | SHA256 Hash |
|-|-|
| [<img alt=download-1 src=https://github.com/koh-gt/ferrite-core/assets/101822992/70ae208a-caaa-40ff-856c-42e87745e8c3 height=32>](https://github.com/koh-gt/ferrite-core/releases/download/v3.1.4/ferrite-3.1.4-win64-setup.exe) | `c2df373192bbbb726f33fdc1562f9dfea07cfb737d42325843f7c4eab5e7137b` <br> **New install** _ferrite-3.1.4-win64-setup.exe_ |
| [<img alt=download-2 src=https://github.com/koh-gt/ferrite-core/assets/101822992/70ae208a-caaa-40ff-856c-42e87745e8c3 height=32>](https://github.com/koh-gt/ferrite-core/releases/download/v3.1.4/ferrite-qt.exe) |  `d7a47117b831036f17370626aadd40074f59d4335312ff89591185ad8cfc65e0` <br> **Wallet Update** _ferrite-qt.exe_ |
| [<img alt=download-3 src=https://github.com/koh-gt/ferrite-core/assets/101822992/70ae208a-caaa-40ff-856c-42e87745e8c3 height=32>](https://github.com/koh-gt/ferrite-core/releases/download/v3.1.4/ferrite-win.zip) | `f3a959e61e64253415d653a009dc95f55241fe99ac0fd85f85db56e88e8890f2` <br> **Server** |

| <img alt=download-logo-1 src=https://github.com/koh-gt/ferrite-core/assets/101822992/c07963c6-718e-4ee3-a9d5-a9a96010f99a height=32> <br> `-         MacOS         -` | SHA256 Hash |
|-|-|
|  [<img alt=download-1 src=https://github.com/koh-gt/ferrite-core/assets/101822992/70ae208a-caaa-40ff-856c-42e87745e8c3 height=32>](https://github.com/koh-gt/ferrite-core/releases/download/v3.1.3/Ferrite-Qt.dmg)               | `e076cfd1515066708fc2b9417d13311ea1cbcf16102962b6b8bf9dfadcda6003` <br> **Wallet (v3.1.3)** _Ferrite-Qt.dmg_      | 
|  [<img alt=download-1 src=https://github.com/koh-gt/ferrite-core/assets/101822992/70ae208a-caaa-40ff-856c-42e87745e8c3 height=32>](https://github.com/koh-gt/ferrite-core/releases/download/v3.1.3/Ferrite-Mac.7z)               | `335a9d3f41682f67ac2ac82f5832b4ea26ad0e8c215eb10db3b9f1f66cb918d6` <br> **Server (v3.1.3)**  |

| <img alt=download-logo-1 src=https://github.com/koh-gt/ferrite-core/assets/101822992/1c13b30a-95de-43bc-82c7-864a86e4658c height=32> <br> `-         Linux         -` | SHA256 Hash |
|-|-|
|  [<img alt=download-1 src=https://github.com/koh-gt/ferrite-core/assets/101822992/70ae208a-caaa-40ff-856c-42e87745e8c3 height=32>](https://github.com/koh-gt/ferrite-core/releases/download/v3.1.4/ferrite-qt-linux.zip)           | `754583a759ae089dfbf4a802f6a990ed6d04c8135fe69dc891bd0c6c1884e782` <br> **Wallet** _ferrite-qt_   |
|  [<img alt=download-1 src=https://github.com/koh-gt/ferrite-core/assets/101822992/70ae208a-caaa-40ff-856c-42e87745e8c3 height=32>](https://github.com/koh-gt/ferrite-core/releases/download/v3.1.4/ferrite-linux.zip)               | `4ce1b5595b3f563919c15731783666819cef9b6fd548e312c3ab29219fa7175d` <br> **Server**  |

## Changes in v3.1.4
Added missing includes
fontconfig changes
zlib update
### Wallet labels update
Labels are now shortened if they are too long. - Prevents excessively long label names from going out of screen, preventing copying of address or sending of funds.
### Other changes
Send delay has also been reduced from 3s to 2s.
For more details see https://github.com/koh-gt/ferrite-core/pull/60

## Future changes
MWEB updates
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
Uploaded on 29 Feb 2024.
Last Updated on 11 Aug 2024.

## What's Changed
* v-3.1.4 by @koh-gt in https://github.com/koh-gt/ferrite-core/pull/60
* fecv-3.1.5 by @koh-gt in https://github.com/koh-gt/ferrite-core/pull/61 (future release)


**Full Changelog**: https://github.com/koh-gt/ferrite-core/compare/v3.1.3...v3.1.4