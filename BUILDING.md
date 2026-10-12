# Building ATTENSION

This guide describes the ATTENSION 1.0 reference build and how to make a
replacement firmware image from source. The documented versions are the
development/reference environment, not a promise that they are strict minimum
versions for every host.

## Reference environment

- Raspberry Pi Pico SDK: 2.3.0
- Platform: RP2040
- Board: `kastle2_board`
- ARM GNU Toolchain: 15_2_Rel1
- CMake: 4.3.4
- Ninja: 1.13.2
- Python: 3.14.7
- picotool: compatible 2.3.0 CMake package

Install the ARM GNU Toolchain 15_2_Rel1, CMake, Ninja, and Python. Obtain the
Pico SDK 2.3.0 from the official Raspberry Pi source and include its
submodules. For example:

```powershell
git clone --branch 2.3.0 --recurse-submodules https://github.com/raspberrypi/pico-sdk.git
```

The SDK's own `pico_sdk_version.cmake` identifies this release as 2.3.0. Set
`PICO_SDK_PATH` to that SDK checkout. The project imports it through
`pico_sdk_import.cmake`.

The post-build UF2 conversion uses picotool. If CMake cannot find a local
picotool package, it may attempt to acquire one. To use an already installed
compatible package without a network fetch, set `picotool_DIR` to the folder
containing `picotoolConfig.cmake`.

## Fresh build

From the repository root, configure a new build directory. For PowerShell:

```powershell
$env:PICO_SDK_PATH = 'C:\path\to\pico-sdk'
$picotool_DIR = 'C:\path\to\picotool'
cmake -S . -B build -G Ninja -Dpicotool_DIR=$picotool_DIR
cmake --build build --target template
```

The target is `template`; its generated firmware name is `ATTENSION`.
Expected outputs include:

- `build/output/ATTENSION.elf`
- `build/output/ATTENSION.uf2`
- `build/output/ATTENSION.bin`
- `build/output/ATTENSION.hex`

The same CMake/Ninja build may also work on other hosts with equivalent tools
and SDK configuration, but the reference build documented here was verified on
Windows.

The Pico SDK, TinyUSB, and the Newlib runtime are acquired external build
dependencies rather than vendored copies in this repository. Their provenance
and the remaining Newlib notice-inventory work are described in
`THIRD_PARTY_NOTICES.md`.

## Modifying the LGPL I2S component

The inherited I2S implementation is in:

- `lib/I2S.cpp`
- `lib/I2S.hpp`
- `lib/I2S.pio`

These files carry LGPL-2.1-or-later provenance and licence notices. A normal
source build recompiles modified I2S source, regenerates PIO material when
`I2S.pio` changes, relinks ATTENSION, and produces a replacement UF2. No
proprietary relinking tool or secret binary is required.

The complete source and build files are supplied so a technically competent
recipient can rebuild ATTENSION with a modified I2S implementation. Consult
the I2S source headers and `LICENSES/LGPL-2.1.txt` for its licence terms.

## Output chain

```text
source -> objects/libraries -> ATTENSION.elf -> ATTENSION.uf2
```

The CMake-generated post-build step uses `picotool uf2 convert` for the RP2040
family. Normally there is no need to invoke picotool manually: building the
`template` target creates `build/output/ATTENSION.uf2`.

## Flashing a rebuilt UF2

1. Put the Kastle 2 RP2040 into BOOTSEL USB mass-storage mode.
2. Copy `build/output/ATTENSION.uf2` to the boot volume.
3. Allow the device to reboot into the new firmware.

The project contains no ATTENSION signing, authentication, or firmware-lock
requirement. A rebuilt UF2 uses the ordinary RP2040 installation path.

## Reproducibility

A build is expected to be functionally reproducible when it uses the matching
source and documented environment. Bit-for-bit reproduction is not
established: host paths, debug metadata, picotool details, and tool versions
can alter exact binary hashes without changing firmware function.
