# macOS Build Instructions and Notes

The commands in this guide should be executed in a Terminal application.
The built-in one is located in
```
/Applications/Utilities/Terminal.app
```

## Preparation
Install the macOS command line tools:

```shell
xcode-select --install
```

When the popup appears, click `Install`.

Then install [Homebrew](https://brew.sh).
``` shell
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
```

## Dependencies
Qt requires [Xcode](https://developer.apple.com/download/all/)
Tested on MacOS 10.15.7 - [Xcode 12.4](https://download.developer.apple.com/Developer_Tools/Xcode_12.4/Xcode_12.4.xip)  
Extract and run Xcode - will require about 30 GB of storage space.  
```shell
brew install automake libtool boost miniupnpc pkg-config python libevent libnatpmp qrencode fmt openssl

# Move xcode to applications folder, xcode necessary for qt
sudo xcode-select --switch /Applications/Xcode.app
brew install qt@5   # requires full xcode

# If you need to have qt@5 first in your PATH, run:
echo 'export PATH="/usr/local/opt/qt@5/bin:$PATH"' >> ~/.zshrc
# For compilers to find qt@5 you may need to set:
export LDFLAGS="-L/usr/local/opt/qt@5/lib"
export CPPFLAGS="-I/usr/local/opt/qt@5/include"
# For pkg-config to find qt@5 you may need to set:
export PKG_CONFIG_PATH="/usr/local/opt/qt@5/lib/pkgconfig"

# get latest c++17
brew install gcc --HEAD

#
```
qt@5 only requires C++11, while latest qt 6 will require C++17 which may not be available on older MacOS make versions.
Common qt error - 
```error: "Qt requires a C++17 compiler"```
```shell
brew remove qt6
```

The requirements are only qt > 5.5.1. Current version is v5.15.8 (20230511)

If you run into issues, check [Homebrew's troubleshooting page](https://docs.brew.sh/Troubleshooting).
See [dependencies.md](dependencies.md) for a complete overview.

If you want to build the disk image with `make deploy` (.dmg / optional), you need RSVG:
```shell
brew install librsvg
```

The wallet support requires one or both of the dependencies ([*SQLite*](#sqlite) and [*Berkeley DB*](#berkeley-db)) in the sections below.
To build Bitcoin Core without wallet, see [*Disable-wallet mode*](#disable-wallet-mode).

#### SQLite

Usually, macOS installation already has a suitable SQLite installation.
Also, the Homebrew package could be installed:

```shell
brew install sqlite
```

In that case the Homebrew package will prevail.

#### Berkeley DB

It is recommended to use Berkeley DB 4.8. If you have to build it yourself,
you can use [this](/contrib/install_db4.sh) script to install it
like so:

```shell
./contrib/install_db4.sh .
```

from the root of the repository.

Also, the Homebrew package could be installed:

```shell
brew install berkeley-db4
brew link berkeley-db@4 --force
```

## Build Ferrite Core

1. Clone the Ferrite Core source code:
    ```shell
    git clone https://github.com/koh-gt /ferrite-core/ferrite-main
    cd ferrite-main
    ```

2.  Build Ferrite Core:

    Configure and build the headless Ferrite Core binaries as well as the GUI (if Qt is found).

    You can disable the GUI build by passing `--without-gui` to configure.
    ```shell
    chmod +x autogen.sh
    chmod +x share/genbuild.sh
    ./autogen.sh
    ./configure --with-miniupnpc --enable-upnp-default --with-natpmp --disable-tests
    make # make -j4 if you have 4 threads, make -j8 for 8 threads
    ```

3.  It is recommended to build and run the unit tests:
    ```shell
    make check
    ```

4.  You can also create a  `.dmg` that contains the `.app` bundle (optional):
    ```shell
    make deploy
    ```

## Disable-wallet mode
When the intention is to run only a P2P hout a wallet, Ferrite Core may be
compiled in disable-wallet mode with:
```shell
./configure --disable-wallet
```

In this case there is no dependency on [*Berkeley DB*](#berkeley-db) and [*SQLite*](#sqlite).

Mining is also possible in disable-wallet mode using the `getblocktemplate` RPC call.

## Running
Ferrite Core is now available at `./src/ferrited`

Before running, you may create an empty configuration file:
```shell
mkdir -p "/Users/${USER}/Library/Application Support/Ferrite"

touch "/Users/${USER}/Library/Application Support/Ferrite/ferrite.conf"

chmod 600 "/Users/${USER}/Library/Application Support/Ferrite/ferrite.conf"
```

The first time you run ferrited, it will start downloading the blockchain. This process could
take many hours, or even days on slower than average systems.

You can monitor the download process by looking at the debug.log file:
```shell
tail -f $HOME/Library/Application\ Support/Ferrite/debug.log
```

## Other commands:
```shell
./src/ferrited -daemon      # Starts the ferrite daemon.
./src/ferrite-cli --help    # Outputs a list of command-line options.
./src/ferrite-cli help      # Outputs a list of RPC commands when the daemon is running.
```

## Notes
* Tested on OS X 10.14 Mojave through macOS 11 Big Sur on 64-bit Intel
processors only.
* Building with downloaded Qt binaries is not officially supported. See the notes in [#7714](https://github.com/bitcoin/bitcoin/issues/7714).
