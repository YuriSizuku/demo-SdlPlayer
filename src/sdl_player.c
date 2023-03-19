#include <stdio.h>

#define WINDOW_W 1280
#define WINDOW_H 720
#define FPS 60
#define SAMPLE_RATE 44100
#define CHANNELS 2
#define SAMPLES 4096
#define AUDIO_FRAME_SIZE SAMPLES*CHANNELS*2
#define AUDIO_FRAME_NUM 10
#define TRUE 1
#define FALSE 0
typedef int bool;

// sdl
#include <SDL2/SDL.h>

static SDL_Window* g_window;
static SDL_Renderer* g_renderer;
static SDL_Texture* g_videotex;
static SDL_mutex* g_audiomutex;
static SDL_AudioDeviceID g_audio;
static SDL_AudioSpec g_audiospec;
static char g_videopath[260] = {0};

// ffmpeg
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/audio_fifo.h>

// ffmpeg context
static AVFormatContext *g_avformatctx;
static AVCodecContext *g_vcodecctx, *g_acodecctx;
static int g_vindex, g_aindex;

// ffmpeg avpacket, avframe, fifo, status
static AVPacket *g_avpacket;
static AVFrame *g_vframe, *g_aframe;
static struct AVAudioFifo *g_audiofifo;
static double g_audiotime = 0; // ms
static enum PLAYER_STATUS{
    STATUS_INIT,
    STATUS_RUNNING,
    STATUS_PAUSE,
} g_playerstatus = STATUS_INIT;

// sws, swr
static struct SwsContext *g_swsctx = NULL;
static struct SwrContext *g_swrctx = NULL;
static uint8_t* g_yuvdata[4] = {NULL};
static int g_yuvlinesize[4] = {0};

// util
static AVFrame* make_target_aframe()
{
    AVFrame *tmpaframe = av_frame_alloc();
    tmpaframe->nb_samples = SAMPLES;
    tmpaframe->channel_layout = AV_CH_LAYOUT_STEREO;
    tmpaframe->format = AV_SAMPLE_FMT_S16;
    tmpaframe->sample_rate = SAMPLE_RATE;
    av_frame_get_buffer(tmpaframe, 0);
    return tmpaframe;
}

// callback
static void audio_callback(void *userdata, Uint8 *stream, int len)
{
    memset(stream, 0, len); // make silence
    if(!g_audiofifo) return;
    if(g_playerstatus == STATUS_PAUSE) return;

    int desired_samples = len / CHANNELS / 2;
    int read_samples = 0;
    AVFrame *tmpaframe = make_target_aframe();
    SDL_LockMutex(g_audiomutex);
    read_samples = av_audio_fifo_read(g_audiofifo, (void**)tmpaframe->data, desired_samples);
    SDL_UnlockMutex(g_audiomutex);
    
    if(read_samples>0)
    {
        // can not use MixAudio here
        SDL_MixAudioFormat(stream, tmpaframe->data[0], 
            g_audiospec.format, read_samples*CHANNELS*2, SDL_MIX_MAXVOLUME);
        g_audiotime += ((double)read_samples / SAMPLE_RATE)*1000; 
    }
    av_frame_unref(tmpaframe);
    av_frame_free(&tmpaframe);
}

// event loop
static int init_sdl()
{
    int res;

    // init sdl video
    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS))
    {
         SDL_Log("SDL_Init failed, %s", SDL_GetError());
         return -1;
    }
    
    g_window = SDL_CreateWindow("sdl_player", 
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 
        WINDOW_W, WINDOW_H, SDL_WINDOW_OPENGL);
    g_renderer = SDL_CreateRenderer(g_window, -1, 
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);
    g_videotex = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_YV12, 
        SDL_TEXTUREACCESS_STREAMING, WINDOW_W, WINDOW_H);
    SDL_SetRenderDrawColor(g_renderer, 0xff, 0xc0, 0xcb, 0xff);
    SDL_RenderClear(g_renderer);

    // init sdl audio
    g_audiomutex = SDL_CreateMutex();
    SDL_AudioSpec desired;
	SDL_zero(desired);
	desired.freq = SAMPLE_RATE;
	desired.format = AUDIO_S16;
	desired.channels = CHANNELS;
	desired.samples = SAMPLES;
	desired.callback = audio_callback;

	g_audio = SDL_OpenAudioDevice(NULL, 0, &desired, &g_audiospec, 0);
    SDL_PauseAudioDevice(g_audio, 0);
}

static int init_ffmpeg()
{
    int res;
    
    // init avformat, avcodec
    avformat_open_input(&g_avformatctx, g_videopath, NULL, NULL);
    av_dump_format(g_avformatctx, 0, g_videopath, 0);
    avformat_find_stream_info(g_avformatctx, NULL); // for some stream without header
    for(int i=0;i<g_avformatctx->nb_streams;i++)
    {
        AVStream* avstream = g_avformatctx->streams[i];
        if(avstream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            g_vindex = i;
            AVCodec* vcodec = avcodec_find_decoder(avstream->codecpar->codec_id);
            g_vcodecctx = avcodec_alloc_context3(vcodec);
            res = avcodec_parameters_to_context(g_vcodecctx, avstream->codecpar);
            res = avcodec_open2(g_vcodecctx, NULL, NULL);
            
            SDL_Log("video fmt=%d %dX%d", avstream->codecpar->format, 
                g_vcodecctx->width, g_vcodecctx->height);
        }
        else if(avstream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            g_aindex = i;
            AVCodec* acodec = avcodec_find_decoder(avstream->codecpar->codec_id);
            g_acodecctx = avcodec_alloc_context3(acodec);
            res = avcodec_parameters_to_context(g_acodecctx, avstream->codecpar);
            res = avcodec_open2(g_acodecctx, NULL, NULL);
            
            SDL_Log("audio fmt=%d samplerate=%d channels=%d channel_layout=%ld", 
                avstream->codecpar->format, 
                avstream->codecpar->sample_rate, 
                avstream->codecpar->channels, 
                avstream->codecpar->channel_layout);
        }
    }

    // init sws, convert yuv420p10 or others to yuv420p
    g_swsctx = sws_getContext(
        g_vcodecctx->width, g_vcodecctx->height, g_vcodecctx->pix_fmt,
        WINDOW_W, WINDOW_H, AV_PIX_FMT_YUV420P, 
        0, NULL, NULL, NULL
    );
    res = av_image_alloc(g_yuvdata, g_yuvlinesize, 
        WINDOW_W, WINDOW_H, AV_PIX_FMT_YUV420P, 2
    );

    // init swr, convert to 44100hz 16bit
    g_swrctx = swr_alloc_set_opts(NULL, 
        AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, SAMPLE_RATE, 
        AV_CH_LAYOUT_STEREO, g_acodecctx->sample_fmt, g_acodecctx->sample_rate, 0, NULL
    );

    // init avpacket, avframe
    g_avpacket = av_packet_alloc();
    g_vframe = av_frame_alloc();
    g_aframe = av_frame_alloc();
    g_audiofifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_S16, CHANNELS, SAMPLES*AUDIO_FRAME_NUM);

    return res;
}

static int on_init()
{
    if(!g_videopath[0]) return -1;

    init_sdl();
    init_ffmpeg();

    return 0;
}

static int on_reset()
{
    SDL_Log("# on_reset");
    g_audiotime = 0;
    avformat_seek_file(g_avformatctx, -1, INT64_MIN, 0, INT64_MAX, AVSEEK_FLAG_FRAME);
    av_audio_fifo_reset(g_audiofifo);
    return 0;
}

static int on_pause()
{
    SDL_Log("# on_pause status=%d", g_playerstatus);
    if(g_playerstatus==STATUS_RUNNING) g_playerstatus = STATUS_PAUSE;
    else g_playerstatus=STATUS_RUNNING;
    return 0;
}

static int on_cleanup()
{
    // clean SDL
    SDL_DestroyTexture(g_videotex);
    SDL_DestroyRenderer(g_renderer);
    SDL_DestroyWindow(g_window);
    SDL_DestroyMutex(g_audiomutex);
    SDL_PauseAudioDevice(g_audio, 0);
    SDL_CloseAudioDevice(g_audio);

    // clean ffmpeg
    sws_freeContext(g_swsctx);
    swr_free(&g_swrctx);

    av_packet_free(&g_avpacket);
    av_frame_free(&g_vframe);
    av_frame_free(&g_aframe);
    av_audio_fifo_free(g_audiofifo);
    
    av_freep(&g_yuvdata[0]);
    avcodec_close(g_vcodecctx);
    avcodec_close(g_acodecctx);
    avformat_close_input(&g_avformatctx);

    return 0;
}

int on_event()
{
	SDL_Event event;
	while (SDL_PollEvent(&event))
	{
		switch (event.type)
        {
        case SDL_KEYDOWN:
            if(event.key.keysym.scancode==SDL_SCANCODE_ESCAPE) 
            {
                SDL_Log("on_exit");
                return SDL_QUIT;
            }
            else if(event.key.keysym.sym==SDLK_r)
            {
                return on_reset();
            }
            else if(event.key.keysym.scancode==SDL_SCANCODE_SPACE)
            {
                return on_pause();
            }
            break;
        
        case SDL_QUIT:
            return SDL_QUIT;
            break;
        
        default:
            break;
        }
	}
    return 0;
}

static int on_update()
{
    if(g_playerstatus == STATUS_PAUSE) return 0;

    int res;
    int nb_samples = av_audio_fifo_size(g_audiofifo);
    int nb_maxsamples = SAMPLES * AUDIO_FRAME_NUM;
    if(nb_samples >= nb_maxsamples) return 1;

    // demux
    res = av_read_frame(g_avformatctx, g_avpacket);
    if(res ==0)
    {
        if(g_avpacket->stream_index == g_vindex)
        {
            res = avcodec_send_packet(g_vcodecctx, g_avpacket);
        }
        else if(g_avpacket->stream_index == g_aindex)
        {
            res = avcodec_send_packet(g_acodecctx, g_avpacket);
        }
        av_packet_unref(g_avpacket);
    }

    // decode audio
    // must recive after send packet or some frame will lost
    res = avcodec_receive_frame(g_acodecctx, g_aframe); 
    if(res==0) 
    {
        AVFrame *tmpaframe = make_target_aframe();
        swr_convert_frame(g_swrctx, tmpaframe, g_aframe);
        
        SDL_LockMutex(g_audiomutex);
        av_audio_fifo_write(g_audiofifo, (void**)tmpaframe->data, tmpaframe->nb_samples);
        SDL_UnlockMutex(g_audiomutex);
        
        av_frame_unref(tmpaframe);
        av_frame_free(&tmpaframe);
        av_frame_unref(g_aframe);
    }

    // decode video
    while(avcodec_receive_frame(g_vcodecctx, g_vframe)==0)
    {
        double videotime = av_q2d(g_avformatctx->streams[g_vindex]->time_base) * g_vframe->pts * 1000;
        // SDL_Log("videotime=%lf audiotime=%lf\n", videotime, g_audiotime);
        if (videotime > g_audiotime)
        {
            // sync video with audio time
            Uint32 start = SDL_GetTicks();
            sws_scale(g_swsctx, (const uint8_t *const*)g_vframe->data, g_vframe->linesize, 
                0, g_vcodecctx->height, g_yuvdata, g_yuvlinesize);
            SDL_UpdateYUVTexture(g_videotex, NULL, 
                g_yuvdata[0], g_yuvlinesize[0],
                g_yuvdata[1], g_yuvlinesize[1],
                g_yuvdata[2], g_yuvlinesize[2]
            );
            Uint32 now = SDL_GetTicks();
            int delay = videotime - g_audiotime - (now - start);
            // SDL_Log("delay=%d", delay);
            if(delay>0 && delay<100 && g_audiotime>0.01) SDL_Delay(delay);
        }

        av_frame_unref(g_vframe);
    }
    return 0;
}

static int on_render()
{
    static Uint32 s_lasttime = 0;
    Uint32 time = SDL_GetTicks();
    if( time - s_lasttime >= (Uint32)(1./FPS))
    {
        SDL_RenderCopy(g_renderer, g_videotex, NULL, NULL);
        SDL_RenderPresent(g_renderer);
        s_lasttime = time;
    }
}

static int main_loop()
{
    int ret;
    g_playerstatus = STATUS_RUNNING;
    while (1)
    {
        ret = on_event();
        if(ret<0 || ret==SDL_QUIT) break;
        ret = on_update();
        ret = on_render();
    }
    return ret;
}

int main(int argc, char *argv[])
{
    if(argc<=1)
    {
        printf("Usage: sdl_player videopath");
        return 0;
    }
    strcpy(g_videopath, argv[1]);
    on_init();
    int ret = main_loop();
    on_cleanup();
    return ret;
}