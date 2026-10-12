# ATTENSION

ATTENSION is community-developed firmware for the Bastl Instruments Kastle 2. It
turns the instrument into a generative musical collaborator: one that moves
between a recognisable HOME, variation, tension, and resolution rather than
simply producing uncontrolled randomness.

The idea is to let musical material evolve while staying connected to a
starting motif. You can steer its energy and direction, then listen as it
finds different ways to leave, develop, and return.

## Have a Kastle 2 and want to try ATTENSION?

Download [ATTENSION_1.0.uf2](ATTENSION_1.0.uf2), then:

1. Put your Kastle 2 into its RP2040 BOOTSEL USB mass-storage mode.
2. Copy `ATTENSION_1.0.uf2` to the boot volume.
3. Let the device reboot.
4. Open the complete [ATTENSION_User_Manual_1.0.pdf](ATTENSION_User_Manual_1.0.pdf)
   to get started.

## What ATTENSION does

ATTENSION begins at HOME: a musical identity shaped by its pitch range,
rhythm, and melodic contour. From there, Wander can carry the music further
from HOME, while Tension shapes the drama of the journey—building expectation
before allowing it to resolve and find its way back.

Voice A carries the primary line; Voice B can answer, support, or add a
contrasting harmonic presence. Percussion and fills give phrases momentum,
while Atmosphere shapes their colour and character, and Presence brings that
surrounding world closer or pushes it further away.

You guide the journey, shaping the movement, activity, voices, and atmosphere
while ATTENSION generates musical possibilities along the way.

## HOME Preview

HOME Preview lets you audition and shape a prospective new HOME before you
commit it. It is a way to listen to the next musical starting point rather
than choosing blind. The [User Manual](ATTENSION_User_Manual_1.0.pdf)
explains the complete workflow.

## Design philosophy

ATTENSION is built around constrained variation. A phrase can change, wander,
and surprise, but its history still matters. Tension and release should feel
like musical movement; variation should retain enough identity to make a
return meaningful. The player and the generative system share the work.

## User manual

The complete guide is
[ATTENSION_User_Manual_1.0.pdf](ATTENSION_User_Manual_1.0.pdf).
It covers controls, workflows, and the full instrument in detail.

## Faceplate

The faceplate artwork is available in two intended forms:

- [ATTENSION_Kastle2_Faceplate.svg](docs/ATTENSION_Kastle2_Faceplate.svg) —
  editable vector artwork.
- [ATTENSION_Kastle2_Faceplate.png](docs/ATTENSION_Kastle2_Faceplate.png) —
  a convenient raster reference and preview.

These files are references for the ATTENSION layout; they do not imply that a
physical faceplate replacement is required.

Their origin and separate non-commercial permissions are described in
[ASSET_PROVENANCE.md](ASSET_PROVENANCE.md).

## Building and modifying

The repository includes the source and build information needed to build
ATTENSION and create a replacement UF2. See [BUILDING.md](BUILDING.md) for
the required external Pico SDK/toolchain dependencies, reference environment,
build process, flashing notes, and information on modifying and rebuilding the
inherited I2S component.

## Open source and provenance

ATTENSION-authored code and modifications identified in source as
GPL-3.0-or-later are distributed under those terms. ATTENSION is derived from
the [open-source Bastl Instruments Kastle 2 firmware](https://github.com/bastl-instruments/kastle2); inherited and
third-party components retain their own licences and notices.

See [LICENSE](LICENSE), [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md),
[ASSET_PROVENANCE.md](ASSET_PROVENANCE.md), and [BUILDING.md](BUILDING.md) for
the relevant details. ATTENSION is an
independent project and does not imply endorsement by Bastl Instruments or
other contributors.

## Feedback

Feedback, questions, bug reports, or found something interesting? I'd be
glad to hear from you.

Chris Beattie  
chrisbeattie994 (at) gmail.com
