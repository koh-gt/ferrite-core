Ferrite Core
=============

Setup
---------------------
Ferrite Core is the original Ferrite client and it builds the backbone of the network. It downloads and, by default, stores the entire history of Ferrite transactions, which requires approximately 22 gigabytes of disk space. Depending on the speed of your computer and network connection, the synchronization process can take anywhere from a few hours to a day or more.

To download Ferrite Core, visit [ferrite.org](https://ferrite.org/).

Running
---------------------
The following are some helpful notes on how to run Ferrite Core on your native platform.

### Unix

Unpack the files into a directory and run:

- `bin/ferrite-qt` (GUI) or
- `bin/ferrited` (headless)

### Windows

Unpack the files into a directory, and then run ferrite-qt.exe.

### macOS

Drag Ferrite Core to your applications folder, and then run Ferrite Core.

### Need Help?

* See the documentation at the [Ferrite Wiki](https://ferrite.info/) for help and more information.
* Ask for help on [#ferrite](https://webchat.freenode.net/#ferrite) on Freenode. If you don't have an IRC client, use [webchat here](https://webchat.freenode.net/#ferrite).
* Ask for help on the [FerriteTalk](https://ferritetalk.io/) forums, in the [Technical Support board](https://ferritetalk.io/c/technical-support).

Building
---------------------
The following are developer notes on how to build Ferrite Core on your native platform. They are not complete guides, but include notes on the necessary libraries, compile flags, etc.

- [Dependencies](dependencies.md)
- [macOS Build Notes](build-osx.md)
- [Unix Build Notes](build-unix.md)
- [Windows Build Notes](build-windows.md)
- [FreeBSD Build Notes](build-freebsd.md)
- [OpenBSD Build Notes](build-openbsd.md)
- [NetBSD Build Notes](build-netbsd.md)
- [Gitian Building Guide (External Link)](https://github.com/bitcoin-core/docs/blob/master/gitian-building.md)

Development
---------------------
The Ferrite repo's [root README](/README.md) contains relevant information on the development process and automated testing.

- [Developer Notes](developer-notes.md)
- [Productivity Notes](productivity.md)
- [Release Notes](release-notes.md)
- [Release Process](release-process.md)
- [Source Code Documentation (External Link)](https://doxygen.bitcoincore.org/)
- [Translation Process](translation_process.md)
- [Translation Strings Policy](translation_strings_policy.md)
- [JSON-RPC Interface](JSON-RPC-interface.md)
- [Unauthenticated REST Interface](REST-interface.md)
- [Shared Libraries](shared-libraries.md)
- [BIPS](bips.md)
- [Dnsseed Policy](dnsseed-policy.md)
- [Benchmarking](benchmarking.md)

### Resources
* Discuss on the [FerriteTalk](https://ferritetalk.io/) forums.
* Discuss general Ferrite development on #ferrite-dev on Freenode. If you don't have an IRC client, use [webchat here](https://webchat.freenode.net/#ferrite-dev).

### Miscellaneous
- [Assets Attribution](assets-attribution.md)
- [bitcoin.conf Configuration File](bitcoin-conf.md)
- [Files](files.md)
- [Fuzz-testing](fuzzing.md)
- [Reduce Memory](reduce-memory.md)
- [Reduce Traffic](reduce-traffic.md)
- [Tor Support](tor.md)
- [Init Scripts (systemd/upstart/openrc)](init.md)
- [ZMQ](zmq.md)
- [PSBT support](psbt.md)

License
---------------------
Distributed under the [MIT software license](/COPYING).
