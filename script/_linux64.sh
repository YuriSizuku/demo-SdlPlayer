# must use after _fetch.sh from local_mingw64.sh

function build_sdl2()
{
    if ! [ -d "${SDL2_SRC}/build_${PLATFORM}" ]; then mkdir -p "${SDL2_SRC}/build_${PLATFORM}"; fi
    echo "## SDL2_SRC=$SDL2_SRC"

    pushd "${SDL2_SRC}/build_${PLATFORM}"
    ../configure --host=x86_64-linux-gnu \
        "CFLAGS=-m64" "CXXFLAGS=-m64" "LDFLAGS=-m64" \
        --disable-3dnow --disable-sse --disable-sse3 \
        --disable-video-wayland --disable-video-offscreen \
        --enable-video-x11  --enable-x11-shared  \
        --prefix=$PORTBUILD_PATH
    make -j$CORE_NUM && make install 
    popd
}

function build_ffmpeg()
{
    if ! [ -d $FFMPEG_SRC/build_$PLATFORM ]; then mkdir -p $FFMPEG_SRC/build_$PLATFORM ;fi

    pushd $FFMPEG_SRC/build_$PLATFORM
    export DLLTOOL=dlltool
    ../configure  --prefix=$PORTBUILD_PATH --enable-pic\
        --arch=x86_64 --target-os=linux --cross-prefix=x86_64-linux-gnu- \
        --enable-static --enable-shared --enable-small --enable-stripping \
        --enable-swscale --enable-avformat --enable-avcodec  \
        --disable-asm --disable-avdevice --disable-doc
    make -j$CORE_NUM && make install
    popd
}