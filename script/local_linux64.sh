# sh -c "export SKIP_PORTS=yes && ./local_linux64.sh"

PLATFORM=linux64
PROJECT_PATH=$(pwd)/..
PORTBUILD_PATH=$PROJECT_PATH/thirdparty/build/arch_$PLATFORM
CORE_NUM=$(cat /proc/cpuinfo | grep -c ^processor)
CC="gcc -m64"

if ! [ -d $PROJECT_PATH/build_$PLATFORM ]; then mkdir -p $PROJECT_PATH/build_$PLATFORM; fi

source ./_fetch.sh
source ./_$PLATFORM.sh
if [ -z "$SKIP_PORTS" ]; then
    # fetch_sdl2 && build_sdl2
    fetch_ffmpeg && build_ffmpeg
fi

# $(pkg-config sdl2 --static --libs)
export PKG_CONFIG_PATH=$PORTBUILD_PATH/lib/pkgconfig
$CC $PROJECT_PATH/src/sdl_player.c -g \
    -o $PROJECT_PATH/build_$PLATFORM/sdl_player \
    -static-libgcc -static-libstdc++ \
    -L$PORTBUILD_PATH/lib -I$PORTBUILD_PATH/include \
    -Wno-discarded-qualifiers \
    -Wl,-Bstatic -lSDL2 -lavformat -lavcodec -lswscale -lswresample -lavutil \
        -lz -lbz2 \
    -Wl,-Bdynamic -ldl -lpthread -lm