### This is a revision of Ferrite Core v3.1.1 based on v3.1.2r1 and v3.1.2r2 changes.
First post-MWEB wallet - MWEB full activation height 150,120 


# v3.1.2 wallets
| <img alt=download-logo-1 src=https://github.com/koh-gt/ferrite-core/assets/101822992/ce12f90e-286d-4229-b5db-db4c462e6256 height=32> <br> `-        Windows        -` | SHA256 Hash |
|-|-|
| [<img alt=download-1 src=https://github.com/koh-gt/ferrite-core/assets/101822992/70ae208a-caaa-40ff-856c-42e87745e8c3 height=32>](https://github.com/koh-gt/ferrite-core/releases/download/v3.1.2/ferrite-3.1.2-win64-setup.exe) | `4dc048a946b406b1b39b2b803108d893ef7785e9b58f0a6dd19281bd6b8a7d20` <br> **New install** _ferrite-3.1.2-win64-setup.exe_ |
| [<img alt=download-2 src=https://github.com/koh-gt/ferrite-core/assets/101822992/70ae208a-caaa-40ff-856c-42e87745e8c3 height=32>](https://github.com/koh-gt/ferrite-core/releases/download/v3.1.2/ferrite-qt.exe) |  `0c19f0cbd935f02ca6127ccf8c0da9c7f62410366a557066d7c67e19407f7bb8` <br> **Wallet Update** _ferrite-qt.exe_ |
| [<img alt=download-3 src=https://github.com/koh-gt/ferrite-core/assets/101822992/70ae208a-caaa-40ff-856c-42e87745e8c3 height=32>](https://github.com/koh-gt/ferrite-core/releases/download/v3.1.2/ferrite-win.7z) | `258b342e63bcea1b24966270b8e49889c8c045d6b8ed9e65764f3578cd75f0ac` <br> **Server** |

| <img alt=download-logo-1 src=https://github.com/koh-gt/ferrite-core/assets/101822992/c07963c6-718e-4ee3-a9d5-a9a96010f99a height=32> <br> `-         MacOS         -` | SHA256 Hash |
|-|-|
|  [<img alt=download-1 src=https://github.com/koh-gt/ferrite-core/assets/101822992/70ae208a-caaa-40ff-856c-42e87745e8c3 height=32>](https://github.com/koh-gt/ferrite-core/releases/download/v3.1.2/Ferrite-Qt.dmg)               | `19edeb13e35a2232dea8ec95bfd4e337470a8bf806197e4972a90441014116a2` <br> **Wallet** _Ferrite-Qt.dmg_      | 
|  [<img alt=download-1 src=https://github.com/koh-gt/ferrite-core/assets/101822992/70ae208a-caaa-40ff-856c-42e87745e8c3 height=32>](https://github.com/koh-gt/ferrite-core/releases/download/v3.1.2/Ferrite-Mac.7z)               | `7c8ee5075cbccbb47887f10d4cf2a181deb4b30c4c57be2cfce98114ad2fe93b` <br> **Server**  |

| <img alt=download-logo-1 src=https://github.com/koh-gt/ferrite-core/assets/101822992/1c13b30a-95de-43bc-82c7-864a86e4658c height=32> <br> `-         Linux         -` | SHA256 Hash |
|-|-|
|  [<img alt=download-1 src=https://github.com/koh-gt/ferrite-core/assets/101822992/70ae208a-caaa-40ff-856c-42e87745e8c3 height=32>](https://github.com/koh-gt/ferrite-core/releases/download/v3.1.2/ferrite-qt.7z)           | `d288b9fdd7233ad358d76fa5bea337fb17484c28aaee5f31836c92b4e4fc5c50` <br> **Wallet** _ferrite-qt_   |
|  [<img alt=download-1 src=https://github.com/koh-gt/ferrite-core/assets/101822992/70ae208a-caaa-40ff-856c-42e87745e8c3 height=32>](https://github.com/koh-gt/ferrite-core/releases/download/v3.1.2/ferrite-linux.7z)               | `a5067d04dd9e0780a4b50ce59a514962f9d54d01c595a738e12c54a24d107e69` <br> **Server**  |


## Changes
Changes in v3.1.2
### Mac OS post-DGW and MWEB compatibility update
> Mac OS boost build fix
> Remove `GUARDED_BY(NetEventsInterface::g_msgproc_mutex)` to avoid gitian build failures
### Documentation and copyright holders update
> Mac OS build notes updated
> Added Dogecoin and Dash copyright acknowledgement
### Update of default Windows installation path to `C:\Program Files\Ferrite\` 
> previously `C:\Program Files\_Ferrite_Core\`
### Maximum feerate increased to 1,000 FEC
> Allows for urgent transactions to boost multipool profitability.

## Future changes
Checkpoint just after MWEB activation, new nodes.
Checkpoints for block 200,000 and 250,000 (important).
MacOS compatibility testing on Ventura and Sonoma.
Bitcoin/Litecoin 24.x updates


## Mining
### [List of pools and settings](https://github.com/koh-gt/ferrite-core/wiki/Mining-Pools-List)
### [MiningPoolStats](https://miningpoolstats.stream/ferrite)

### Ferrite runs on Scrypt algorithm - exact compatibility with Litecoin/Dogecoin
**Scrypt is not an ASIC-resistant algorithm.**

ASIC mining is recommended. You can opt to [rent an Scrypt ASIC](https://github.com/koh-gt/ferrite-core/wiki/Rent-an-ASIC-miner).

## Other notes

### All versions of Ferrite Core **wallets** will maintain backwards compatibility
> At height 250,000 all Ferrite will be split into 1 Ferrite (FEC) and 1 Ferrite Classic (FECC). v1.x.x and v2.x.x will only be able to spend the FECC portion, while v3+ will only be able to spend the FEC portion. 
Do not be alarmed if coins sent through v1.x.x and v2.x.x (Ferrite Classic) do not appear in your v3+ (Ferrite) balance - your Ferrite balance is unchanged.
Coins sent to Ferrite Classic through Ferrite will not show up, but are not lost. Ferrite Classic users need to copy their wallet to a v3+ Ferrite Core client to be able to access the coins. 
One can attempt to run both Ferrite Classic and Ferrite after Ferrite Classic is launched. Ferrite Classic may encounter hashrate instability.


* Windows and Linux builds compiled on Ubuntu Linux 22.10 Jammy Jellyfish by @tansander and ktoki
* Mac OS builds compiled on Mac OS 10.15.5 Catalina by @koh-gt 
Uploaded on 7 Aug 2023.

## What's Changed
* v3.1.2 by @koh-gt in https://github.com/koh-gt/ferrite-core/pull/53
* v3.1.2-2 - Boost build fix by @koh-gt in https://github.com/koh-gt/ferrite-core/pull/54

**Full Changelog**: https://github.com/koh-gt/ferrite-core/compare/v3.1.1...v3.1.2