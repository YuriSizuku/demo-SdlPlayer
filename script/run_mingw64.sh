# sh -c "./run_mingw64.sh test.mkv"

PLATFORM=mingw64
PROJECT_PATH=$(pwd)/..
PORTBUILD_PATH=$PROJECT_PATH/thirdparty/build/arch_$PLATFORM
ARGS=$@

if [ -n "$(uname -a) | grep Msys" ]; then
    if [ -z "$MSYS2" ]; then MSYS2=/d/Software/env/msys2; fi
    PATH=$MSYS2/mingw64/bin:$PATH
fi
PATH=$PORTBUILD_PATH/bin:$PATH

pushd $PROJECT_PATH/build_$PLATFORM
./sdl_player $ARGS
popd