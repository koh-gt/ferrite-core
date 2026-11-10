WINDOWS BUILD NOTES
====================

Below are some notes on how to build Ferrite Core for Windows.

The options known to work for building Ferrite Core on Windows are:

* On Linux, using the [Mingw-w64](https://mingw-w64.org/doku.php) cross compiler tool chain. Ubuntu Jammy Jellyfish 22.04 is required
and is the platform used to build the Ferrite Core Windows release binaries.
* On Windows, using [Windows
Subsystem for Linux (WSL)](https://docs.microsoft.com/windows/wsl/about) and the Mingw-w64 cross compiler tool chain.
* On Windows, using a native compiler tool chain such as [Visual Studio](https://www.visualstudio.com). See [README.md](/build_msvc/README.md).

Other options which may work, but which have not been extensively tested are (please contribute instructions):

* On Windows, using a POSIX compatibility layer application such as [cygwin](https://www.cygwin.com/) or [msys2](https://www.msys2.org/).

Installing Windows Subsystem for Linux
---------------------------------------

With Windows 10, Microsoft has released a new feature named the [Windows
Subsystem for Linux (WSL)](https://docs.microsoft.com/windows/wsl/about). This
feature allows you to run a bash shell directly on Windows in an Ubuntu-based
environment. Within this environment you can cross compile for Windows without
the need for a separate Linux VM or server. Note that while WSL can be installed with
other Linux variants, such as OpenSUSE, the following instructions have only been
tested with Ubuntu.

This feature is not supported in versions of Windows prior to Windows 10 or on
Windows Server SKUs. In addition, it is available [only for 64-bit versions of
Windows](https://docs.microsoft.com/windows/wsl/install-win10).

Full instructions to install WSL are available on the above link.
To install WSL on Windows 10 with Fall Creators Update installed (version >= 16215.0) do the following:

1. Enable the Windows Subsystem for Linux feature
  * Open the Windows Features dialog (`OptionalFeatures.exe`)
  * Enable 'Windows Subsystem for Linux'
  * Click 'OK' and restart if necessary
2. Install Ubuntu
  * Open Microsoft Store and search for "Ubuntu 22.04 LTS"
  * Click Install
  * On Windows 10 version 2004 and later, typing `wsl --install -d Ubuntu-22.04` in a cmd prompt does steps 1 and 2
3. Complete Installation
  * Open a cmd prompt and type `wsl -d Ubuntu-22.04`
  * Create a new UNIX user account (this is a separate account from your Windows account)

After the bash shell is active, you can follow the instructions below, starting
with the "Cross-compilation" section. Compiling the 64-bit version is
recommended, but it is possible to compile the 32-bit version.

Cross-compilation for Ubuntu and Windows Subsystem for Linux
------------------------------------------------------------

The steps below can be performed on Ubuntu (including in a VM) or WSL. The depends system
will also work on other Linux distributions, however the commands for
installing the toolchain will be different.

First, install the general dependencies. The depends system builds every library the Windows
binaries use, so the libraries listed in [build-unix.md](build-unix.md) are not needed:

    sudo apt update
    sudo apt upgrade
    sudo apt install build-essential libtool autotools-dev automake pkg-config bsdmainutils curl git python3 nsis

A host toolchain (`build-essential`) is necessary because some dependency
packages need to build host utilities that are used in the build process.
`python3` runs `make -C src check-security` and `make -C src check-symbols`.

See [dependencies.md](dependencies.md) for a complete overview.

`make deploy` builds the Windows installer with [NSIS](https://nsis.sourceforge.io/Main_Page) (`nsis`).
Without it, configure warns "makensis not found. Cannot create installer." and `make deploy` prints
"error: could not build ..." and then "built ...", and exits without an error, but makes no installer.

Acquire the source in the usual way:

    git clone https://github.com/koh-gt/ferrite-core.git
    cd ferrite-core

## Building for 64-bit Windows

The first step is to install the mingw-w64 cross-compilation tool chain, in its posix threads variant <sup>[1](#footnote1)</sup>:

    sudo apt install g++-mingw-w64-x86-64-posix

If you installed `g++-mingw-w64-x86-64` instead, it brings both variants and makes the win32 one the default,
also on Ubuntu 22.04, so set the default to posix:

    sudo update-alternatives --config x86_64-w64-mingw32-g++ # Set the default mingw32 g++ compiler option to posix.

Once the toolchain is installed the build steps are common:

Note that for WSL the Ferrite Core source path MUST be somewhere in the default mount file system, for
example /usr/src/ferrite, AND not under /mnt/d/. If this is not the case the dependency autoconf scripts will fail.
This means you cannot use a directory that is located directly on the host Windows file system to perform the build.

Additional WSL Note: WSL support for [launching Win32 applications](https://docs.microsoft.com/en-us/archive/blogs/wsl/windows-and-ubuntu-interoperability#launching-win32-applications-from-within-wsl)
results in `Autoconf` configure scripts being able to execute Windows Portable Executable files. This can cause
unexpected behaviour during the build, such as Win32 error dialogs for missing libraries. The recommended approach
is to temporarily disable WSL support for Win32 applications.

The depends build supplies fmt (pinned at 7.1.3) for the Windows binaries; the build machine's own fmt is not used.

Build using, from the `ferrite-core` directory (on Ubuntu, leave out the three lines marked WSL only):

    PATH=$(echo "$PATH" | sed -e 's/:\/mnt.*//g') # WSL only: strip out problematic Windows %PATH% imported var
    sudo bash -c "echo 0 > /proc/sys/fs/binfmt_misc/status" # WSL only: disable WSL support for Win32 applications.
    cd depends
    make HOST=x86_64-w64-mingw32 -j$(nproc)
    cd ..
    ./autogen.sh
    CONFIG_SITE=$PWD/depends/x86_64-w64-mingw32/share/config.site ./configure --prefix=/ --with-miniupnpc --enable-upnp-default --disable-tests
    make -j$(nproc)
    sudo bash -c "echo 1 > /proc/sys/fs/binfmt_misc/status" # WSL only: enable WSL support for Win32 applications again.
    make deploy

`make deploy` writes the installer, `ferrite-<version>-win64-setup.exe`, to the `ferrite-core` directory.
`make -C src check-security` and `make -C src check-symbols` check the binaries, as the release build does.
    
No more 32-bit builds available.
    
The Qt build in depends applies `fix_limits_header.patch`, so GCC 11 and later no longer stop it with
"numeric_limits is not a member of std" in qbytearraymatcher.h; no manual edit is needed.

## Depends system

For further documentation on the depends system see [README.md](../depends/README.md) in the depends directory.

Installation
-------------

After building using the Windows subsystem it can be useful to copy the compiled
executables to a directory on the Windows drive in the same directory structure
as they appear in the release `.zip` archive. This can be done in the following
way. This will install to `c:\workspace\ferrite`, for example:

    make install DESTDIR=/mnt/c/workspace/ferrite

You can also create an installer using:

    make deploy

Footnotes
---------

<a name="footnote1">1</a>: Starting from Ubuntu Xenial 16.04, both the 32 and 64 bit Mingw-w64 packages install two different
compiler options to allow a choice between either posix or win32 threads. The default option is win32 threads which is the more
efficient since it will result in binary code that links directly with the Windows kernel32.lib. Unfortunately, the headers
required to support win32 threads conflict with some of the classes in the C++11 standard library, in particular std::mutex.
It's not possible to build the Ferrite Core code using the win32 version of the Mingw-w64 cross compilers (at least not without
modifying headers in the Ferrite Core source code).
