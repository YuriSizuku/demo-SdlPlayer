if ! [ -d $PROJECT_PATH/thirdparty/port ]; then mkdir -p $PROJECT_PATH/thirdparty/port; fi
if ! [ -d $PROJECT_PATH/thirdparty/build/arch_mingw64 ]; then mkdir -p $PROJECT_PATH/thirdparty/build/arch_mingw64; fi
if ! [ -d $PROJECT_PATH/thirdparty/build/arch_linux64 ]; then mkdir -p $PROJECT_PATH/thirdparty/build/arch_linux64; fi

# fetch by wget
function fetch_port()
{
    if ! [ -d "$PROJECT_PATH/thirdparty/port/$2" ]; then
        echo "## fetch_port $1 $2"
        wget $1/$2.tar.gz -O $PROJECT_PATH/thirdparty/port/$2.tar.gz 
        tar zxf $PROJECT_PATH/thirdparty/port/$2.tar.gz -C $PROJECT_PATH/thirdparty/port
    fi
}

function fetch_sdl2()
{
    SDL2_NAME=SDL2-2.26.4
    SDL2_SRC=$PROJECT_PATH/thirdparty/port/$SDL2_NAME
    fetch_port https://www.libsdl.org/release $SDL2_NAME
}

function fetch_ffmpeg()
{
    FFMPEG_NAME=ffmpeg-4.4.3
    FFMPEG_SRC=$PROJECT_PATH/thirdparty/port/$FFMPEG_NAME
    fetch_port https://ffmpeg.org/releases $FFMPEG_NAME
}