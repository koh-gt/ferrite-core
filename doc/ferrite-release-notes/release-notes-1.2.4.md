## [**Download Ferrite Wallet**](https://github.com/koh-gt/ferrite-core/releases/download/v1.2.4/ferrite-qt.exe) - Ferrite Core Qt for Windows 64-bit
_[Windows 32-bit](https://github.com/koh-gt/ferrite-core/releases/download/v1.2.4/ferrite-qt-x86.exe) / [Linux and Mac OS 64-bit](https://github.com/koh-gt/ferrite-core/releases/download/v1.2.4/ferrite-qt-unix.7z)_

---

This is a revision of Ferrite Core v1.2.3 based on Ferrite Core 1.2.4r1 and 1.2.4r2 source code changes.

### Wallet + other executables installers
[64-bit Windows Installer](https://github.com/koh-gt/ferrite-core/releases/download/v1.2.4/ferrite-1.2.4-win64-setup.exe) - Ferrite Core Installer for Windows 64-bit
[32-bit Windows Installer](https://github.com/koh-gt/ferrite-core/releases/download/v1.2.4/ferrite-1.2.4-win32-setup.exe) - Ferrite Core Installer for Windows 32-bit

### Other executables / OS
[ferrite-core-1.2.4-win64.7z](https://github.com/koh-gt/ferrite-core/releases/download/v1.2.4/ferrite-core-1.2.4-win64.7z) - 7zip file containing ferrite executables for Windows 64-bit.  
[ferrite-core-1.2.4-win32.7z](https://github.com/koh-gt/ferrite-core/releases/download/v1.2.4/ferrite-core-1.2.4-win32.7z) - 7zip file containing ferrite executables for Windows 32-bit.  
[ferrite-core-1.2.4-unix.7z](https://github.com/koh-gt/ferrite-core/releases/download/v1.2.4/ferrite-core-1.2.4-unix.7z) - 7zipfile containing ferrite static built executables for both 64-bit Linux and Mac OS X.  

## Mining
### [List of pools and settings](https://github.com/koh-gt/ferrite-core/wiki/Mining-Pools-List)
[ferrite-pool-miner.zip](https://github.com/koh-gt/ferrite-core/releases/download/v1.2.3/ferrite-pool-miner.7z) - Ferrite ASIC/GPU pool miner for Windows. You can choose to mine in any Ferrite stratum pool using a GPU or a Scrypt ASIC.
[ferrite-cpu-miner.zip](https://github.com/koh-gt/ferrite-core/releases/download/v1.2.0/ferrite-cpu-miner.zip) - Contains simple-to-use powershell and configuration scripts for solo CPU mining.  (Not recommended)

## Changes
Added new kFEC unit for more human-meaningful wallet balances. ($0.20/kFEC vs $0.0002/FEC) Easier to send larger amounts. 12 decimal point precision for kFEC unit.
Added new documentation and about page changes.
Added built-in addnodes for the following pool:
### [Zeus Mining Pool](https://zeusminingpool.net/)

Newly added pools will be added to addnodes on version 1.3.0.
Minor stability changes.

Included in this release is a stratum-pool miner based on [CCMiner v2.3.1 by @tpruvot](https://github.com/tpruvot/ccminer/releases/tag/2.3.1-tpruvot) with new pool configurations.
ASIC mining is recommended over GPU and CPU mining. GPU mining is still feasible for more efficient cards when undervolted. You can opt to rent an Scrypt ASIC.

Everyone is invited to pull ferrite-core to suggest feature updates/chain parameters.

* Compiled on Ubuntu Linux 22.04 by @koh-gt
Uploaded on 21 Jan 2023.

**Full Changelog**: https://github.com/koh-gt/ferrite-core/compare/v1.2.3...v1.2.4