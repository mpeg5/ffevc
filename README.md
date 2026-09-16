# ffevc — FFmpeg plus up-to-date EVC integration

This project delivers FFmpeg plus the latest EVC (MPEG-5 Essential Video
Coding) integration work. Its purpose is fast source distribution:
improvements and fixes for the EVC codec wrappers are developed, reviewed
and released here, so that users can pick them up quickly without waiting
for the next FFmpeg release cycle.

The maintained codec integration is:

- **EVC** — encoding through [xeve](https://github.com/mpeg5/xeve), decoding
  through [xevd](https://github.com/mpeg5/xevd)

The FFmpeg code base itself is updated regularly from upstream FFmpeg, so
the tree stays close to current FFmpeg while carrying the newest EVC
patches on top. The current base is FFmpeg 9.0.1: the `release/9.0` branch
at commit `3a7c002718` (2026-09-15), including the `n9.0.1` maintenance
release and the fixes landed on the branch after it. Everything outside
the EVC integration works the same as in pure FFmpeg.

## Branches

| Branch | Content |
|---|---|
| `main` | The distribution branch: current FFmpeg base plus the latest EVC patches. |
| `upstream` | Pristine FFmpeg, no local patches. Updated regularly from upstream FFmpeg and then merged into `main`. |
| feature branches | Work in progress, opened as pull requests against `main`. |

# EVC (xeve / xevd)

EVC (MPEG-5 Essential Video Coding) support uses FFmpeg's `libxeve` encoder
and `libxevd` decoder wrappers. They are enabled at configure time with
`--enable-libxeve` and `--enable-libxevd` (the xeve and xevd libraries must
be installed and visible to `pkg-config`).

The encoder is selected with `-c:v libxeve`. Supported input pixel formats:
`yuv420p`, `yuv420p10`. The main options are `-profile` (`baseline`, `main`),
`-preset` (`fast`, `medium`, `slow`, `placebo`), `-rc_mode` (`CQP`, `ABR`,
`CRF`) with `-qp` (0–51) or `-crf` (10–49), and `-xeve-params` for passing
`key=value` pairs directly to the xeve library.

HDR metadata is carried through EVC as well: HDR10 mastering display /
content light level and dynamic HDR10+ (SMPTE ST 2094-40) are written as SEI
messages on encoding and restored as frame side data on decoding (see
[HDR metadata over EVC](#hdr-metadata-over-evc)). xeve and xevd 0.7.0 or
newer are required (the versions that provide the per-picture SEI API).

## Examples

Encode to EVC with the main profile and a target bitrate:

    ffmpeg -i input.mov -c:v libxeve -profile main -b:v 5M output.mp4

Constant-QP encoding:

    ffmpeg -i input.mov -c:v libxeve -rc_mode CQP -qp 30 output.mp4

Decode EVC:

    ffmpeg -i input.mp4 output.yuv

## HDR metadata over EVC

HDR metadata carried by the input is written into the EVC bitstream as SEI
messages (ISO/IEC 23094-1 Annex D) on encoding and restored as frame side
data on decoding:

- static HDR10: mastering display colour volume and content light level SEI
- dynamic HDR10+ (SMPTE ST 2094-40), carried in a user-data-registered
  ITU-T T.35 SEI message

No extra options are needed; a plain transcode keeps the metadata:

    ffmpeg -i hdr10plus_input.mp4 -c:v libxeve -profile main output.mp4

To check the metadata, decode and look for the side data entries:

    ffprobe -show_frames -show_entries frame=side_data_list output.mp4

Static metadata can also be attached explicitly when the source has none:

    ffmpeg -f lavfi \
        -mastering_display "G(8500,39850)B(6550,2300)R(35400,14600)WP(15635,16450)L(10000000,1)" \
        -content_light "1000,200" \
        -i testsrc2=size=1920x1080:rate=30 \
        -c:v libxeve -profile main -pix_fmt yuv420p10 -frames:v 30 output.mp4

# Building

Two components are built in order: the xeve encoder and xevd decoder
libraries (https://github.com/mpeg5/xeve and https://github.com/mpeg5/xevd,
CMake), then FFmpeg itself, which finds them through `pkg-config` (`xeve.pc`
and `xevd.pc` are installed by their builds). xeve builds its main profile
by default (`-DSET_PROF=MAIN`; use `-DSET_PROF=BASE` for the baseline
profile).

Common prerequisites: a C compiler, `make`, `cmake`, `pkg-config`, `git`,
and `nasm` for the x86 assembly optimizations (or configure FFmpeg with
`--disable-x86asm` to build without them).

## Linux

Install the build dependencies (Debian/Ubuntu):

    sudo apt-get install build-essential cmake git pkg-config nasm

1. Build and install xeve and xevd (0.7.0 or newer):

       git clone https://github.com/mpeg5/xeve.git
       cmake -S xeve -B xeve/build -DCMAKE_BUILD_TYPE=Release
       cmake --build xeve/build -j$(nproc)
       sudo cmake --install xeve/build

       git clone https://github.com/mpeg5/xevd.git
       cmake -S xevd -B xevd/build -DCMAKE_BUILD_TYPE=Release
       cmake --build xevd/build -j$(nproc)
       sudo cmake --install xevd/build
       sudo ldconfig

2. Configure and build FFmpeg with the EVC wrappers enabled:

       git clone https://github.com/mpeg5/ffevc.git
       cd ffevc
       ./configure --enable-libxeve --enable-libxevd
       make -j$(nproc)

   If xeve/xevd are installed in a non-default prefix, point pkg-config at
   it:

       PKG_CONFIG_PATH=/path/to/prefix/lib/pkgconfig ./configure --enable-libxeve --enable-libxevd

3. Verify the wrappers are available:

       ./ffmpeg -h encoder=libxeve
       ./ffmpeg -h decoder=libxevd

## Windows (MinGW-w64)

Windows builds use the MinGW-w64 toolchain. FFmpeg's build system needs a
POSIX shell, which [MSYS2](https://www.msys2.org/) provides. Install MSYS2,
open the **MSYS2 MINGW64** shell and install the tools:

    pacman -S --needed base-devel git \
        mingw-w64-x86_64-toolchain \
        mingw-w64-x86_64-cmake \
        mingw-w64-x86_64-ninja \
        mingw-w64-x86_64-nasm \
        mingw-w64-x86_64-pkgconf

1. Build and install xeve and xevd into the MinGW prefix:

       git clone https://github.com/mpeg5/xeve.git
       cmake -S xeve -B xeve/build -G Ninja \
           -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/mingw64
       cmake --build xeve/build
       cmake --install xeve/build

       git clone https://github.com/mpeg5/xevd.git
       cmake -S xevd -B xevd/build -G Ninja \
           -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/mingw64
       cmake --build xevd/build
       cmake --install xevd/build

2. Configure and build FFmpeg in the same shell:

       git clone https://github.com/mpeg5/ffevc.git
       cd ffevc
       ./configure --enable-libxeve --enable-libxevd
       make -j$(nproc)

3. The resulting `ffmpeg.exe` depends on the xeve/xevd DLLs (in
   `/mingw64/bin`) and the MinGW runtime DLLs; keep them next to the
   executable or in `PATH` when running outside the MSYS2 shell.

# About FFmpeg itself

This tree is a full FFmpeg distribution; apart from the EVC integration
described above, everything — libraries, tools, documentation, licensing —
is plain FFmpeg. For general FFmpeg information see the upstream
[FFmpeg README](https://github.com/FFmpeg/FFmpeg/blob/master/README.md), the
[documentation](https://ffmpeg.org/documentation.html), and [LICENSE.md](LICENSE.md)
(mainly LGPL, with optional GPL components).

# How to contribute

Contributions to upstream FFmpeg follow [its own process](https://ffmpeg.org/developer.html#Contributing).
**This repository is different: contributions are made through GitHub pull
requests.**

Pull requests are accepted **only for the EVC integration maintained
here**: the xeve/xevd wrappers (encoder, decoder, parser) and related code
(muxing/demuxing). Changes to any other part of the FFmpeg code base are
out of scope — please submit those to upstream FFmpeg following its
[contribution process](https://ffmpeg.org/developer.html#Contributing); this
tree merges upstream FFmpeg regularly, so fixes accepted there arrive here
as well.

1. Fork the repository and create a feature branch from `main`.
2. Build and test your change (see the Building section above).
3. Keep commit subjects to a single line and sign off your commits
   (`git commit -s`).
4. Open a pull request against `main` with a short description of what the
   change does and how it was tested.

When opening a PR from a fork, please **enable "Allow edits from
maintainers"**. It lets maintainers rebase your branch or apply small review
fixes directly instead of going through another request/response round trip,
which can shorten the review cycle considerably.
