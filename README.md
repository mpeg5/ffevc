# Functionality from this repository is merged to official FFmpeg repository at https://git.ffmpeg.org/ffmpeg.git
# Use main FFmpeg for latest fixes and features.
# This repository is Archived and for reference only, it won't be maintained anymore!


# ffevc
ffmpeg supporting EVC codec and file formats. 

MPEG-5 Essential Video Coding (EVC)  integration with FFmpeg project.  

It is supported under Linux and Windows.  

## Prequisities

1. Install build dependencies

    ```bash
    sudo apt-get update -qq && sudo apt-get -y install \
    autoconf \
    automake \
    build-essential \
    cmake \
    pkg-config \
    libsdl2-dev \ 
    nasm
    ```
2. Install in system xeve encoder (dev package, main profile) 

    Detailed and latest info is in [xeve].

3. Install in system xevd decoder (dev package)

    Detailed and latest info is in [xevd].

## Build 

To enable EVC in FFmpeg it need to be configured with flags  `--enable-libxeve` and `--enable-libxevd`


### Sample static configuration
```bash
PATH="$HOME/bin:$PATH" PKG_CONFIG_PATH="$HOME/ffmpeg_build/lib/pkgconfig" ./configure \
--prefix="$HOME/ffmpeg_build" \
--pkg-config-flags="--static" \
--extra-cflags="-I$HOME/ffmpeg_build/include" \
--extra-ldflags="-L$HOME/ffmpeg_build/lib" \
--extra-libs="-lpthread -lm" \
--bindir="$HOME/bin" \
--enable-ffplay \
--enable-libxeve \
--enable-libxevd
```
### Shared configuration

For shred build, FFmpeg need to be configured with flag `--enable-shared` and removed `--pkg-config-flags="--static"`.

### Compilation & installation

```bash
PATH="$HOME/bin:$PATH" make -j $(nproc)

make install
```


## Examples

### Encode

```
ffmpeg -f rawvideo -pix_fmt yuv420p -s:v 1920x1080 -r 30 -i input_file.yuv -c:v libxeve -f rawvideo output_file.evc
```

Pass encoder specify parameters by using `-xeve-params "<param=value>:<param=value>"`
```
ffmpeg -f rawvideo -pix_fmt yuv420p -s:v 1920x1080 -r 30 -i input_file.yuv -c:v libxeve -xeve-params "rc-type=0:q=37:profile=baseline:preset=medium" -f rawvideo output.evc
```

Encoder parameters help

`ffmpeg -help encoder=libxeve`

Optional parameters:

 - `-threads <num>`

 ### Decode

 ```
 ffmpeg -i input_file.evc -pix_fmt yuv420p output_file.yuv
 ```

 ### MP4 container

YUV -> MP4
```
ffmpeg -f rawvideo -pix_fmt yuv420p -s:v 1920x1080 -r 30 -i input_file.yuv -c:v libxeve -f rawvideo output_file.mp4
```

MP4 -> YUV
```
ffmpeg -i input_file.mp4 -f rawvideo -pix_fmt yuv420p output_file.yuv
```

EVC -> MP4
```
ffmpeg -i input_file.evc -c:v copy output_file.mp4
```

MP4 -> EVC
```
ffmpeg -i input_file.mp4 -vcodec copy -an -f rawvideo output_file.evc
```

## References

[xeve][xeve]  
[xevd][xevd]  
[FFmpeg][ffmpeg]  


[xeve]: https://github.com/mpeg5/xeve "xeve repository"
[xevd]: https://github.com/mpeg5/xevd "xevd repository"
[ffmpeg]: https://trac.ffmpeg.org/wiki/CompilationGuide/Ubuntu "FFmpeg compile"
