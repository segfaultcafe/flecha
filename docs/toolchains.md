# Supported toolchains

Flecha 0.1.1 uses explicit major-version ranges for source builds and pinned
major-version baselines for release production. Patch releases within a listed
major version are accepted. Versions outside these ranges are not supported
until they pass the complete CI, sanitizer, packaging, installed-layout, and
clean-install matrix.

## Source-build policy

| Component | Supported versions |
| --- | --- |
| CMake | 3.20 or newer |
| LLVM development package | 17 through 23 |
| Clang C/C++ compiler | 17 through 23 |
| GCC C/C++ compiler | 13 through 16 |
| Apple Clang | 16 through 17 |
| Microsoft Visual C++ | Not supported |

Microsoft Visual C++ is not supported by the current release line. Building
Flecha and generated programs with the MSVC ABI and native MSVC drivers is a
required [0.3.0 release gate](release_0_3.md#release-gates), not a claim about
the toolchains supported today.

The C and C++ compilers must come from the same toolchain and support C++23.
CMake rejects unsupported compiler and LLVM versions during configuration.
`LLVM_DIR` must identify the matching LLVM CMake package when it is not on the
default search path.

## Release baselines

Release archives are built and consumed on these pinned GitHub-hosted runner
families:

| Archive | Build toolchain | Clean-install linker and SDK |
| --- | --- | --- |
| Linux x86-64 | Ubuntu 24.04, Clang 18, LLVM 18 | Clang 18, GNU binutils and Ubuntu 24.04 system development/startup files |
| Windows x86-64 | Windows Server 2025, MSYS2 UCRT64, Clang 23, LLVM 23 | MSYS2 UCRT64 Clang 23 and the UCRT64 runtime/startup packages |
| macOS x86-64 | macOS 15, Homebrew Clang 18 and LLVM 18 | Xcode 16.4, Apple Clang 17, macOS SDK 15 |
| macOS arm64 | macOS 15, Homebrew Clang 18 and LLVM 18 | Xcode 16.4, Apple Clang 17, macOS SDK 15 |

The supported binary-package hosts are Ubuntu 24.04 x86-64, Windows Server
2025 x86-64 under MSYS2 UCRT64, and macOS 15.x on x86-64 or arm64 with Xcode
16.4 and a macOS 15.x SDK. Other host OS and SDK combinations are untested.

The Linux test suite also builds with GCC 14. Sanitizer jobs use Clang 18.
Every CI and release job prints the detected compiler, LLVM, linker, target,
and platform runtime or SDK version, then fails if it differs from its matrix
baseline.

## Installation packages

### Ubuntu 24.04

Install the pinned release-build toolchain with:

```sh
sudo apt-get install clang-18 llvm-18-dev ninja-build cmake
```

GCC validation additionally uses `gcc-14` and `g++-14`. A binary archive user
installs the host linker and system startup files with:

```sh
sudo apt-get install binutils clang-18 libc6-dev
```

LLVM development files are not required.

### Windows Server 2025 and MSYS2

Flecha supports the MSYS2 `UCRT64` environment. Release builds use the shared
LLVM and Clang 23 packages:

```sh
pacman -S --needed \
    mingw-w64-ucrt-x86_64-clang \
    mingw-w64-ucrt-x86_64-cmake \
    mingw-w64-ucrt-x86_64-ninja \
    mingw-w64-ucrt-x86_64-llvm
```

The workflow verifies LLVM and Clang 23 before configuring, so an MSYS2 rolling
upgrade cannot silently change the release toolchain. Binary archive users
install only `mingw-w64-ucrt-x86_64-clang`, which supplies the native driver,
UCRT libraries, and startup files without requiring the LLVM development
package.

### macOS 15

Source builds use Homebrew `llvm@18`. Native linking uses Xcode 16.4 and the
macOS 15 SDK:

```sh
brew install llvm@18 cmake ninja
export LLVM_DIR="$(brew --prefix llvm@18)/lib/cmake/llvm"
export CC="$(brew --prefix llvm@18)/bin/clang"
export CXX="$(brew --prefix llvm@18)/bin/clang++"
```

Select Xcode 16.4 through `DEVELOPER_DIR` or `xcode-select`. Binary archive
users need only the corresponding Xcode command-line tools and SDK, not the
Homebrew LLVM development package.

## Updating the matrix

Changing a supported range or release baseline requires updating
`cmake/toolchain_policy.cmake`, both GitHub Actions workflows, and this page in
the same change. New versions become supported only after all protected CI
jobs pass on the affected release platforms.
