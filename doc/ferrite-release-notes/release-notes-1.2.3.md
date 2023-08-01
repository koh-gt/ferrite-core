## [**Download Ferrite Wallet**](https://github.com/koh-gt/ferrite-core/releases/download/v1.2.3/ferrite-qt.exe) - Ferrite Core Qt for Windows 64-bit
_[Windows 32-bit](https://github.com/koh-gt/ferrite-core/releases/download/v1.2.3/ferrite-qt-x86.exe) / [Linux and Mac OS 64-bit](https://github.com/koh-gt/ferrite-core/releases/download/v1.2.3/ferrite-qt-unix.7z)_

---

This is a revision of Ferrite Core v1.2.2 based on Ferrite Core 1.2.3r1 and 1.2.3r2 source code changes.

### Wallet + other executables installers
[64-bit Windows Installer](https://github.com/koh-gt/ferrite-core/releases/download/v1.2.3/ferrite-1.2.3-win64-setup.exe) - Ferrite Core Installer for Windows 64-bit
[32-bit Windows Installer](https://github.com/koh-gt/ferrite-core/releases/download/v1.2.3/ferrite-1.2.3-win32-setup.exe) - Ferrite Core Installer for Windows 32-bit

### Other executables / OS
[ferrite-core-1.2.3-win64.7z](https://github.com/koh-gt/ferrite-core/releases/download/v1.2.3/ferrite-core-1.2.3-win64.7z) - zip file containing ferrite executables for Windows 64-bit.  
[ferrite-core-1.2.3-win32.7z](https://github.com/koh-gt/ferrite-core/releases/download/v1.2.3/ferrite-core-1.2.3-win32.7z) - zip file containing ferrite executables for Windows 32-bit.  
[ferrite-core-1.2.3-linux-osx.7z](https://github.com/koh-gt/ferrite-core/releases/download/v1.2.3/ferrite-core-1.2.3-unix.7z) - tar.xz file containing ferrite static built executables for both 64-bit Linux and Mac OS X.  

## Mining
[ferrite-pool-miner.zip](https://github.com/koh-gt/ferrite-core/releases/download/v1.2.3/ferrite-pool-miner.7z) - Ferrite ASIC/GPU pool miner for Windows. You can choose to mine in any Ferrite stratum pool using a GPU or a Scrypt ASIC.
[ferrite-cpu-miner.zip](https://github.com/koh-gt/ferrite-core/releases/download/v1.2.0/ferrite-cpu-miner.zip) - Contains simple-to-use powershell and configuration scripts for solo CPU mining.  (Not recommended)

## Changes
Added checkpoints for block 10,000 and 30,000.  
About page updated to year 2022-2023. Source code and wiki page is accessible from the about page.
Added built-in addnodes for the following pools:

### [Findblocks Pool](https://findblocks.net/) - Mark Richards
### [Lucky Dog Pool](https://luckydogpool.com/) - LuckySmurf
### [Spools Pool](https://spools.online/) - Alexan

>Pool mining gives a more stable payout unlike solo mining. 

Minor stability changes.

Included in this release is a stratum-pool miner based on [CCMiner v2.3.1 by @tpruvot](https://github.com/tpruvot/ccminer/releases/tag/2.3.1-tpruvot) with new pool configurations.
ASIC mining is recommended over GPU and CPU mining. GPU mining is still feasible for more efficient cards when undervolted. You can opt to rent an Scrypt ASIC.

Everyone is invited to pull ferrite-core to suggest feature updates/chain parameters.

* Compiled on Ubuntu Linux 22.04 by @koh-gt
Uploaded on 31 Dec 2022. Happy 2023.

## What's Changed
* 20221226 by @koh-gt in https://github.com/koh-gt/ferrite-core/pull/29
* v1.3.x by @koh-gt in https://github.com/koh-gt/ferrite-core/pull/31

**Full Changelog**: https://github.com/koh-gt/ferrite-core/compare/v1.2.2...v1.2.3