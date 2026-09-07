# libsilk
This filter decodes standalone SILK speech files using the SILK SDK.

## Requirements

[CMake](https://cmake.org/) is used as a build system. To install it, follow
[Debian build instructions](developing_in_debian.md).

[Emscripten SDK](https://emscripten.org/) is required for building
WebAssembly artifacts. To install it, follow the
[Download and Install](https://emscripten.org/docs/getting_started/downloads.html)
guide:

```bash
cd $OPT

# Get the emsdk repo.
git clone https://github.com/emscripten-core/emsdk.git

# Enter that directory.
cd emsdk

# Download and install the latest SDK tools.
./emsdk install latest

# Make the "latest" SDK "active" for the current user. (writes ~/.emscripten file)
./emsdk activate latest
```

## Building the accessor

```bash
# Setup EMSDK and other environment variables. In practice EMSDK is set to be
# $OPT/emsdk.
source $OPT/emsdk/emsdk_env.sh

# Assuming you are in the root level of the cloned repo :
emcmake cmake .
emmake make
```

Once built, you can use and distribute libsilk_1.wasm with your universal tags.

## Why the SILK SDK and not libopus

SILK is the speech half of Opus, and libopus does carry a SILK decoder — but
not this bitstream. When SILK was folded into Opus its frame header moved into
the Opus TOC byte, so libopus's `silk_Decode` expects the caller to supply the
internal sampling rate and the number of frames in the packet, and no longer
reads them from the stream. A standalone `.silk` file still has them in the
frame, where the SDK left them.

The two are therefore not interchangeable, and not by a little: decoding a real
`.silk` file with libopus's SILK measures **-20 dB SNR** against the SDK's own
decode of the same file.

## The file format

Nine bytes `#!SILK_V3`, then per packet a 16-bit little-endian length and that
many bytes; a length of -1 ends the stream. Some writers — WeChat is the common
case — put one extra byte in front of the magic, so it is looked for at offset
0 and offset 1.

The format carries no sampling rate, which is why `srate` is an option
defaulting to 24 kHz, the rate the SDK's own test decoder uses. It carries no
packet duration either, but that needs no option: the decoder is called until
it says it has no more internal frames for the packet.

## Rebuilding the library

```bash
cd $SILK_SDK/silk
emmake make lib CC=emcc AR=emar RANLIB=emranlib \
    CFLAGS="-fPIC -O3 -DNDEBUG -Iinterface -Isrc -Itest"
```

The include paths have to be repeated: overriding `CFLAGS` drops the ones the
SDK's Makefile sets.

## Documentation

For more details, please visit our documentation at https://bevara.com/documentation/develop/.
