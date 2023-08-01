## [**Download Ferrite Wallet**](https://github.com/koh-gt/ferrite-core/releases/download/v1.2.1/ferrite-qt.exe) - Ferrite Core Qt for Windows 64-bit
_[Windows 32-bit](https://github.com/koh-gt/ferrite-core/releases/download/v1.2.1/ferrite-qt-x86.exe) / [Linux and Mac OS 64-bit](https://github.com/koh-gt/ferrite-core/releases/download/v1.2.1/ferrite-qt-linux-osx.xz)_

---

This is a revision of Ferrite Core v1.2.0 based on Ferrite Core 1.2.1r and 1.2.1r2 source code changes.

### Wallet + other executables installers
[64-bit Windows Installer](https://github.com/koh-gt/ferrite-core/releases/download/v1.2.1/ferrite-1.2.1-win64-setup.exe) - Ferrite Core Installer for Windows 64-bit
[32-bit Windows Installer](https://github.com/koh-gt/ferrite-core/releases/download/v1.2.1/ferrite-1.2.1-win32-setup.exe) - Ferrite Core Installer for Windows 32-bit

### Other executables / OS
[ferrite-core-1.2.1-win64.zip](https://github.com/koh-gt/ferrite-core/releases/download/v1.2.1/ferrite-core-1.2.1-win64.7z) - zip file containing ferrite executables for Windows 64-bit.  
[ferrite-core-1.2.1-win32.zip](https://github.com/koh-gt/ferrite-core/releases/download/v1.2.1/ferrite-core-1.2.1-win32.7z) - zip file containing ferrite executables for Windows 32-bit.  
[ferrite-core-1.2.1-linux-macos-amd64.tar.xz](https://github.com/koh-gt/ferrite-core/releases/download/v1.2.1/ferrite-core-1.2.1-linux-macos-amd64.tar.xz) - tar.xz file containing ferrite static built executables for both 64-bit Linux and Mac OS X.  

## Mining
Quick mining: `generate 1` in Qt console
[ferrite-gpu-miner.zip](https://github.com/koh-gt/ferrite-core/releases/download/v1.2.0/ferrite-gpu-miner.zip) - Ferrite GPU Miner for Windows (Faster)
[ferrite-cpu-miner.zip](https://github.com/koh-gt/ferrite-core/releases/download/v1.2.0/ferrite-cpu-miner.zip) - Contains simple-to-use powershell and configuration scripts for solo CPU mining. 

### Changes
This release allows you to send 1 atom/vB transactions. The default fee is 1 atom/vB
Roboto Mono monospace font is now used for displaying Ferrite balance.
Mandarin translation fixes.

Included in this release is a GPU pool miner based on [CCMiner v2.2.5 by @tpruvot](https://github.com/tpruvot/ccminer/releases/tag/2.2.5-tpruvot) created by @TylerAnder. GPU mining is oftentimes much faster than CPU mining.

Everyone is invited to pull ferrite-core to suggest feature updates/chain parameters.

* Compiled on Ubuntu Linux 22.04 by @koh-gt
Uploaded on 3 Dec 2022

## What's Changed
* Reduce default fallback fee by @koh-gt in https://github.com/koh-gt/ferrite-core/pull/5
* v1.2.2 - reduce qt mintxfee to 1 atom per vB by @koh-gt in https://github.com/koh-gt/ferrite-core/pull/8
* fonts 1 by @koh-gt in https://github.com/koh-gt/ferrite-core/pull/9
* fonts-qrc by @koh-gt in https://github.com/koh-gt/ferrite-core/pull/10
* fonts by @koh-gt in https://github.com/koh-gt/ferrite-core/pull/11
* use embedded font roboto mono by @koh-gt in https://github.com/koh-gt/ferrite-core/pull/12

**Full Changelog**: https://github.com/koh-gt/ferrite-core/compare/v1.2.0...v1.2.1