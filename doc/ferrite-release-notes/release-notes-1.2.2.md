## [**Download Ferrite Wallet**](https://github.com/koh-gt/ferrite-core/releases/download/v1.2.2/ferrite-qt.exe) - Ferrite Core Qt for Windows 64-bit
_[Windows 32-bit](https://github.com/koh-gt/ferrite-core/releases/download/v1.2.2/ferrite-qt-x86.exe) / [Linux and Mac OS 64-bit](https://github.com/koh-gt/ferrite-core/releases/download/v1.2.2/ferrite-qt-linux-osx.7z)_

---

This is a revision of Ferrite Core v1.2.1 based on Ferrite Core 1.2.2r source code changes.

### Wallet + other executables installers
[64-bit Windows Installer](https://github.com/koh-gt/ferrite-core/releases/download/v1.2.2/ferrite-1.2.2-win64-setup.exe) - Ferrite Core Installer for Windows 64-bit
[32-bit Windows Installer](https://github.com/koh-gt/ferrite-core/releases/download/v1.2.2/ferrite-1.2.2-win32-setup.exe) - Ferrite Core Installer for Windows 32-bit

### Other executables / OS
[ferrite-core-1.2.2-win64.7z](https://github.com/koh-gt/ferrite-core/releases/download/v1.2.2/ferrite-core-1.2.2-win64.7z) - zip file containing ferrite executables for Windows 64-bit.  
[ferrite-core-1.2.2-win32.7z](https://github.com/koh-gt/ferrite-core/releases/download/v1.2.2/ferrite-core-1.2.2-win32.7z) - zip file containing ferrite executables for Windows 32-bit.  
[ferrite-core-1.2.2-linux-osx.7z](https://github.com/koh-gt/ferrite-core/releases/download/v1.2.2/ferrite-core-1.2.2-linux-osx.7z) - tar.xz file containing ferrite static built executables for both 64-bit Linux and Mac OS X.  

## Mining
[ferrite-pool-miner.zip](https://github.com/koh-gt/ferrite-core/releases/download/v1.2.2/ferrite-pool-miner.7z) - Ferrite ASIC/GPU pool miner for Windows. You can choose to mine in any Ferrite stratum pool using a GPU or a Scrypt ASIC.
[ferrite-cpu-miner.zip](https://github.com/koh-gt/ferrite-core/releases/download/v1.2.0/ferrite-cpu-miner.zip) - Contains simple-to-use powershell and configuration scripts for solo CPU mining.  (Not recommended)

### Changes
Minor stability changes.

Included in this release is a stratum-pool miner based on [CCMiner v2.3.1 by @tpruvot](https://github.com/tpruvot/ccminer/releases/tag/2.3.1-tpruvot)
Due to @TylerAnder pool being temporarily down, the miner default config now points to [Mining Coins](mining-coins.com) pool at stratum+tcp://161.97.83.204:3181 as suggested by user "CM C" on Telegram.
ASIC mining is recommended over GPU and CPU mining. GPU mining is still feasible for more efficient cards when undervolted. You can opt to rent an Scrypt ASIC.

Everyone is invited to pull ferrite-core to suggest feature updates/chain parameters.

* Compiled on Ubuntu Linux 22.04 by @koh-gt
Uploaded on 25 Dec 2022. Merry Christmas.

## What's Changed
* Revert changes by @koh-gt in https://github.com/koh-gt/ferrite-core/pull/27
* pool miner changes by @artucuxi in https://github.com/koh-gt/ferrite-core/pull/28

## New Contributors
* @artucuxi made their first contribution in https://github.com/koh-gt/ferrite-core/pull/28

**Full Changelog**: https://github.com/koh-gt/ferrite-core/compare/v1.2.1...v1.2.2