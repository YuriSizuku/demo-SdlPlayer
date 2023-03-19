# bash -c "./run_linux64.sh ../asset/test.mkv"

PLATFORM=mingw64
PROJECT_PATH=$(pwd)/..
PORTBUILD_PATH=$PROJECT_PATH/thirdparty/build/arch_$PLATFORM
ARGS=$@

PATH=$PORTBUILD_PATH/bin:$PATH
$PROJECT_PATH/build_$PLATFORM/sdl_player $ARGS