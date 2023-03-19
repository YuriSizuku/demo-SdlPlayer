# must use after _fetch.sh from local_mingw64.sh

function build_sdl2()
{
    if ! [ -d "${SDL2_SRC}/build_${PLATFORM}" ]; then mkdir -p "${SDL2_SRC}/build_${PLATFORM}"; fi
    echo "## SDL2_SRC=$SDL2_SRC"

    export CFLAGS="-Os"
    export CXXFLAGS="-Os"
    pushd "${SDL2_SRC}/build_${PLATFORM}"
    ../configure --host=x86_64-w64-mingw32 \
        --disable-3dnow --disable-sse --disable-sse3 \
        --disable-video-vulkan --disable-video-offscreen \
        --prefix=$PORTBUILD_PATH
    make -j$CORE_NUM && make install 
    popd
}

function build_ffmpeg()
{
    if ! [ -d $FFMPEG_SRC/build_$PLATFORM ]; then mkdir -p $FFMPEG_SRC/build_$PLATFORM ;fi
    
    # the ffmpeg might be some error using linux cross mingw64
    CROSS_CONFIG=
    if [ -z "$(uname -a) | grep Msys" ]; then
        CROSS_CONFIG=--enable-cross-compile --cross-prefix=x86_64-w64-mingw32-
    fi

    pushd $FFMPEG_SRC/build_$PLATFORM
    export DLLTOOL=dlltool
    ../configure  --prefix=$PORTBUILD_PATH --enable-pic \
        --arch=x86_64 --target-os=mingw64 $CROSS_CONFIG \
        --cc=x86_64-w64-mingw32-gcc --strip=strip --enable-stripping \
        --ar=x86_64-w64-mingw32-gcc-ar --nm=x86_64-w64-mingw32-gcc-nm \
        --extra-cflags="-static" --extra-ldflags="-static" \
        --pkg-config-flags="--static" \
        --enable-static --enable-shared --enable-small \
        --enable-swscale --enable-avformat --enable-avcodec  \
        --disable-asm --disable-avdevice --disable-doc

    # use sh directory is not available in windows (absolute path), must use msys2 shell
    make -j$CORE_NUM && make install
    popd
}