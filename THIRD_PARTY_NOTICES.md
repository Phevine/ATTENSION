# Third-party and inherited notices

ATTENSION is a modified firmware derived from the Kastle 2 codebase. It is
not the original Bastl Instruments firmware, and this project does not imply
endorsement by Bastl Instruments or any other named contributor.

## Kastle 2 inherited code

The inherited Kastle 2 source carries MIT licence notices naming, as
applicable, Vaclav Mach and Marek Mach of Bastl Instruments. The original
Kastle 2 root MIT licence, including `Copyright (c) 2023-2025 Bastl
Instruments`, is preserved in `LICENSES/Kastle2-MIT.txt`. File-level notices
remain in the affected source files. ATTENSION-authored modifications are
identified in source by their own GPL-3.0-or-later notices; they do not
relicense inherited MIT material or other third-party material.

## I2S implementation

`lib/I2S.cpp`, `lib/I2S.hpp`, and `lib/I2S.pio` are inherited I2S material
under LGPL-2.1-or-later. Their source headers identify Vaclav Mach (Bastl
Instruments) and state that portions derive from the Arduino-Pico I2S
implementation by Earle F. Philhower, III (copyright 2022). The headers also
identify inspiration from Daniel Collins' `rp2040_i2s_example` and state that
no code from that project was copied. The I2S licence and notices are retained
in those files; `LICENSES/LGPL-2.1.txt` supplies the LGPL v2.1 text.

## Raspberry Pi Pico SDK and WS2812 PIO

ATTENSION builds against the external Raspberry Pi Pico SDK. The SDK is not
vendored wholesale in this repository; the build imports it through
`PICO_SDK_PATH`. Pico SDK source used by this build is BSD-3-Clause licensed.

`src/common/peripherals/WS2812.pio` is adapted from the Raspberry Pi Pico
examples and carries a Raspberry Pi (Trading) Ltd. BSD-3-Clause notice. The
associated local files retain their Bastl MIT and Raspberry Pi BSD notices.

## TinyUSB

The project uses the TinyUSB device stack supplied through the external Pico
SDK, including USB MIDI and the inherited CDC interface. The linked FIFO
implementation (`lib/tinyusb/src/common/tusb_fifo.c` in the SDK used for the
reference build) carries the MIT notice:

- Copyright (c) 2019 Ha Thach (tinyusb.org)
- Copyright (c) 2020 Reinhard Panhuber - rework to unmasked pointers

Local `src/tusb_config.h` and `src/usb_descriptors.c` retain TinyUSB MIT
notices; the descriptor file also identifies Vaclav Mach (Bastl Instruments).
This acknowledgement records verified contributors to the linked FIFO source;
it is not represented as a complete TinyUSB contributor inventory.

## Newlib runtime (OPEN inventory)

The reference build uses Newlib supplied by Arm GNU Toolchain 15.2.Rel1. The
release-verification map records retained members of its `libg.a` runtime.
Newlib is assembled from multiple source components with file-specific notices;
the canonical upstream notice is at
<https://sourceware.org/newlib/COPYING.NEWLIB>.

The exact object-to-source notice inventory for the retained runtime members
has not yet been completed from the matching Arm GNU Toolchain source package.
Accordingly, this document does not claim that the generic Newlib notice or
URL is a complete notice inventory. That matching source archive and its
file-level licence evidence are required before making a final binary-runtime
notice claim.

## EarLevel ADSR provenance (OPEN licence review)

`src/common/dsp/control/AdsrEnv.cpp` and `AdsrEnv.hpp` retain their existing
acknowledgement of the ADSR envelope work by Nigel Redmon / EarLevel
Engineering: <https://www.earlevel.com/main/2013/06/03/envelope-generators-adsr-code/>.
The acknowledgement records inspiration and provenance only. This repository
does not assert that EarLevel source was copied or that a particular EarLevel
licence applies; the original downloadable source and terms remain to be
reviewed.

## WS2812 C++ provenance (OPEN permission)

`src/common/peripherals/WS2812.cpp` and `WS2812.hpp` identify the original
ForsakenNGS Pico_WS2812 C++ library. The upstream repository did not provide a
verified explicit redistribution licence during this release review:
<https://github.com/ForsakenNGS/Pico_WS2812>. The existing attribution is
preserved, but a formal upstream grant, authoritative historic licence, or a
future replacement based directly on suitably licensed material is needed
before treating that C++ provenance as resolved.

`src/common/peripherals/WS2812.pio` separately carries Raspberry Pi (Trading)
Ltd. BSD-3-Clause provenance from the Pico examples; that PIO provenance does
not establish a licence for the ForsakenNGS C++ wrapper.

## DSP provenance

`Fm2` identifies MIT-licensed DaisySP/Electrosmith source by Ben Sergentanis
as its original code provenance. `AdvancedDynamicDelayLine` identifies
MIT-licensed DaisySP `DelayLine` material as a loose basis. Their source
headers preserve the applicable copyright and MIT notices.

## Artwork and documentation

Artwork and manual provenance and their separate non-commercial permissions
are documented in `ASSET_PROVENANCE.md`. They are distinct from firmware and
component licences.

## Licence texts

Canonical texts supplied with this source package are:

- `LICENSE` — GNU GPL version 3 licence text, applicable to
  ATTENSION-authored code and modifications identified as GPL-3.0-or-later.
- `LICENSES/Kastle2-MIT.txt` — original Bastl Instruments root MIT licence for
  inherited Kastle 2 material.
- `LICENSES/LGPL-2.1.txt` — GNU Lesser GPL v2.1.
- `LICENSES/MIT.txt` — MIT licence text.
- `LICENSES/BSD-3-Clause.txt` — BSD 3-Clause licence text.

File-level notices remain authoritative for component-specific copyright and
licence scope.
