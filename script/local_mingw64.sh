# sh -c "export SKIP_PORTS=yes && ./local_mingw64.sh"

PLATFORM=mingw64
PROJECT_PATH=$(pwd)/..
PORTBUILD_PATH=$PROJECT_PATH/thirdparty/build/arch_$PLATFORM
CORE_NUM=$(cat /proc/cpuinfo | grep -c ^processor)
CC=x86_64-w64-mingw32-gcc

if [ -n "$(uname -a | grep Msys) " ]; then 
    if [ -z "$MSYS2" ]; then MSYS2=/d/Software/env/msys2; fi
    PATH=$MSYS2/mingw64/bin:$PATH
fi

if ! [ -d $PROJECT_PATH/build_$PLATFORM ]; then mkdir -p $PROJECT_PATH/build_$PLATFORM; fi

source ./_fetch.sh
source ./_$PLATFORM.sh
if [ -z "$SKIP_PORTS" ]; then
    fetch_sdl2 && build_sdl2
    fetch_ffmpeg && build_ffmpeg
fi

# $(pkg-config sdl2 --static --libs)
export PKG_CONFIG_PATH=$PORTBUILD_PATH/lib/pkgconfig
$CC $PROJECT_PATH/src/sdl_player.c \
    -o $PROJECT_PATH/build_$PLATFORM/sdl_player.exe \
    -static-libgcc -static-libstdc++ -g \
    -L$PORTBUILD_PATH/lib -I$PORTBUILD_PATH/include \
    -Wl,--subsystem,console -Wno-discarded-qualifiers \
    -Wl,-Bstatic,--whole-archive -lwinpthread -Wl,--no-whole-archive \
    -Wl,-Bstatic -lmingw32 -mwindows -lSDL2main -lSDL2 -lmingw32 -mwindows -lSDL2main -lSDL2\
    -Wl,-Bdynamic -lavformat -lavcodec -lswscale -lswresample -lavutil \
    -lm -luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 -lversion -luuid -ladvapi32 -lsetupapi -lshell32 -ldinput8