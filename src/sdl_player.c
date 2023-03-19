#include <stdio.h>

#define WINDOW_W 1280
#define WINDOW_H 720
#define FPS 60
#define SAMPLE_RATE 44100
#define CHANNELS 2
#define SAMPLES 4096
#define AFRAME_SIZE SAMPLES*CHANNELS*2
#define AFRAME_NUM 100
#define VFRAME_NUM 25
#define SYNC_THRESHOLD (double)SAMPLES/(double)SAMPLE_RATE*1000.f/3.f
#define TRUE 1
#define FALSE 0
typedef int bool;

// sdl
#include <SDL2/SDL.h>

static SDL_Window* g_window;
static SDL_Renderer* g_renderer;
static SDL_Texture* g_videotex;
static SDL_mutex* g_audiomutex;
static SDL_mutex* g_videomutex;
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
#include <libavutil/fifo.h>

// ffmpeg context
static AVFormatContext *g_avformatctx;
static AVCodecContext *g_vcodecctx, *g_acodecctx;
static int g_vindex, g_aindex;

// ffmpeg avpacket, avframe, fifo, status
static AVPacket *g_avpacket;
static AVFrame *g_vframe, *g_aframe;
static struct AVAudioFifo *g_asamplefifo; // store audio pcm sample
static struct AVFifoBuffer *g_vframefifo; // store AVFrame*
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

// ffmpeg function
static AVFrame* ff_target_aframe()
{
    AVFrame *tmpaframe = av_frame_alloc();
    tmpaframe->nb_samples = SAMPLES;
    tmpaframe->channel_layout = AV_CH_LAYOUT_STEREO;
    tmpaframe->format = AV_SAMPLE_FMT_S16;
    tmpaframe->sample_rate = SAMPLE_RATE;
    av_frame_get_buffer(tmpaframe, 0);
    return tmpaframe;
}

static int ff_demux(void *args)
{
    int res = 0;

    // audio is important, must be enough
    if(av_audio_fifo_size(g_asamplefifo) > 3*SAMPLES)
    {
        if(av_audio_fifo_space(g_asamplefifo) < SAMPLES) res = 1;
        if(av_fifo_space(g_vframefifo) < sizeof(AVFrame*) ) res = 2;
        if(res!=0) return res;
    }

    res = av_read_frame(g_avformatctx, g_avpacket);
    if(res ==0)
    {
        if(g_avpacket->stream_index == g_vindex)
        {
            // SDL_Log("ff_demux video");
            res = avcodec_send_packet(g_vcodecctx, g_avpacket);
        }
        else if(g_avpacket->stream_index == g_aindex)
        {
            // SDL_Log("ff_demux audio");
            res = avcodec_send_packet(g_acodecctx, g_avpacket);
        }
        av_packet_unref(g_avpacket);
    }
    return res;
}

// audio decode, to g_asamplefifo
static int ff_adecode(void *args)
{
    // must recive after send packet or some frame will lost
    if(av_audio_fifo_space(g_asamplefifo) < SAMPLES) return 1;
    
    int res = 0;
    AVFrame *tmpaframe = ff_target_aframe();
    res = avcodec_receive_frame(g_acodecctx, g_aframe);

    if(res==0)
    {
        // SDL_Log("ff_adecode");
        swr_convert_frame(g_swrctx, tmpaframe, g_aframe);
        
        SDL_LockMutex(g_audiomutex);
        av_audio_fifo_write(g_asamplefifo, (void**)tmpaframe->data, tmpaframe->nb_samples);
        SDL_UnlockMutex(g_audiomutex);
        
        av_frame_unref(tmpaframe);
        av_frame_unref(g_aframe);
    }

    av_frame_free(&tmpaframe);
    return res;
}

// video decode, to g_vframefifo
static int ff_vdecode(void *args)
{
    if(av_fifo_space(g_vframefifo) < sizeof(AVFrame*) ) return 1;
    
    int res = 0;
    res = avcodec_receive_frame(g_vcodecctx, g_vframe);
    if(res==0)
    {
        // SDL_Log("ff_vdecode");
        SDL_LockMutex(g_videomutex);
        AVFrame *tmpvframe = av_frame_clone(g_vframe);
        av_fifo_generic_write(g_vframefifo, (void*)&tmpvframe, sizeof(tmpvframe), NULL);
        SDL_UnlockMutex(g_videomutex);
    }

    return res;
}

// make fifo full
static void ff_fullfifo()
{
    enum PLAYER_STATUS oldstatus = g_playerstatus;
    g_playerstatus = STATUS_PAUSE;
    while(av_audio_fifo_space(g_asamplefifo) >= SAMPLES &&
         av_fifo_space(g_vframefifo) >= sizeof(AVFrame*))
    {
        ff_demux(NULL);
        ff_adecode(NULL);
        ff_vdecode(NULL);
    }
    g_playerstatus = oldstatus;
}

static int init_ffmpeg()
{
    int res = 0;
    
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
    g_asamplefifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_S16, CHANNELS, SAMPLES*AFRAME_NUM);
    g_vframefifo = av_fifo_alloc_array(VFRAME_NUM, sizeof(AVFrame*));

    return res;
}

// sdl function
static void sdl_audiocb(void *userdata, Uint8 *stream, int len)
{
    memset(stream, 0, len); // make silence
    if(!g_asamplefifo) return;
    if(g_playerstatus == STATUS_PAUSE) return;

    int desired_samples = len / CHANNELS / 2;
    int read_samples = 0;
    AVFrame *tmpaframe = ff_target_aframe();
    SDL_LockMutex(g_audiomutex);
    read_samples = av_audio_fifo_read(g_asamplefifo, (void**)tmpaframe->data, desired_samples);
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

static int init_sdl()
{
    int res = 0;

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
    g_videomutex = SDL_CreateMutex();
    SDL_AudioSpec desired;
	SDL_zero(desired);
	desired.freq = SAMPLE_RATE;
	desired.format = AUDIO_S16;
	desired.channels = CHANNELS;
	desired.samples = SAMPLES;
	desired.callback = sdl_audiocb;

	g_audio = SDL_OpenAudioDevice(NULL, 0, &desired, &g_audiospec, 0);
    SDL_PauseAudioDevice(g_audio, 0);
}


// event loop
static int on_init()
{
    if(!g_videopath[0]) return -1;

    init_sdl();
    init_ffmpeg();
    ff_fullfifo();

    return 0;
}

static int on_reset()
{
    SDL_Log("# on_reset");
    g_audiotime = 0;
    
    SDL_LockMutex(g_audiomutex);
    av_audio_fifo_reset(g_asamplefifo);
    SDL_UnlockMutex(g_audiomutex);
    
    SDL_LockMutex(g_videomutex);
    while (av_fifo_size(g_vframefifo) > 0)
    {
        AVFrame *tmpvframe;
        av_fifo_generic_read(g_vframefifo, (void*)&tmpvframe, sizeof(tmpvframe), NULL);
        if(tmpvframe)
        {
            av_frame_unref(tmpvframe);
            av_frame_free(&tmpvframe);
        }
    }
    av_fifo_reset(g_vframefifo);
    SDL_UnlockMutex(g_videomutex);

    while (avcodec_receive_frame(g_vcodecctx, g_vframe)==0)
    {
        av_frame_unref(g_vframe);
    }
    avformat_seek_file(g_avformatctx, -1, INT64_MIN, 0, INT64_MAX, AVSEEK_FLAG_FRAME);
    ff_fullfifo();
    
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
    SDL_DestroyMutex(g_videomutex);
    SDL_PauseAudioDevice(g_audio, 0);
    SDL_CloseAudioDevice(g_audio);

    // clean ffmpeg
    sws_freeContext(g_swsctx);
    swr_free(&g_swrctx);

    av_packet_free(&g_avpacket);
    av_frame_free(&g_vframe);
    av_frame_free(&g_aframe);
    av_audio_fifo_free(g_asamplefifo);
    av_fifo_freep(&g_vframefifo);
    
    av_freep(&g_yuvdata[0]);
    avcodec_close(g_vcodecctx);
    avcodec_close(g_acodecctx);
    avformat_close_input(&g_avformatctx);

    return 0;
}

static int on_event()
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
    static bool s_newframe = TRUE;
    static Uint32 s_prevtime = 0;
    static double s_lastaudiotime = 0;

    if(g_playerstatus == STATUS_PAUSE) 
    {
        s_newframe = TRUE;
        return 0;
    }

    int res = 0;
    if(res==0)
    {
        ff_demux(NULL);
        ff_adecode(NULL);
        ff_vdecode(NULL);
    }

    // update vframe
    SDL_LockMutex(g_videomutex);
    if(av_fifo_size(g_vframefifo) > 0 )
    {
        AVFrame *tmpvframe;
        av_fifo_generic_peek(g_vframefifo, &tmpvframe, sizeof(tmpvframe), NULL);
        if(tmpvframe)
        {
            double videotime = av_q2d(g_avformatctx->streams[g_vindex]->time_base) * tmpvframe->pts * 1000;
            double delaytime = videotime - g_audiotime;

            // if audiotime not update yet, calculate the time based on newframe pts
            if(!s_newframe && s_lastaudiotime==g_audiotime)
            {
                delaytime -= SDL_GetTicks() - s_prevtime;
            }
            s_lastaudiotime = g_audiotime;
#if 0
            SDL_Log("updatevframe: fifo_v=%d, fifo_a=%d, vtime=%.1lf, atime=%.1lf | newframe=%d delay=%.1lf\n", 
                av_fifo_size(g_vframefifo)/sizeof(tmpvframe), 
                av_audio_fifo_size(g_asamplefifo)/SAMPLES,
                videotime, g_audiotime, s_newframe, delaytime);
#endif
            bool removeflag = FALSE;
            bool renderflag = FALSE;
            if(delaytime < 0)
            {
                if(delaytime >= -SYNC_THRESHOLD) renderflag = TRUE;
                else removeflag = TRUE;
            }
            else 
            {
                if (delaytime <= SYNC_THRESHOLD) renderflag = TRUE;
                else if(delaytime >= 2000.f) removeflag = TRUE; // on_reset
            }

            if(renderflag)
            {
                sws_scale(g_swsctx, (const uint8_t *const*)tmpvframe->data, tmpvframe->linesize, 
                    0, g_vcodecctx->height, g_yuvdata, g_yuvlinesize);
                removeflag = TRUE;
            }
            
            if(removeflag)
            {
                av_fifo_drain(g_vframefifo, sizeof(tmpvframe));
                av_frame_unref(tmpvframe);
                av_frame_free(&tmpvframe);
            }
            s_newframe = removeflag;
            if(s_newframe) s_prevtime = SDL_GetTicks();
        }
    }
    SDL_UnlockMutex(g_videomutex);

    return 0;
}

static int on_render()
{
    static Uint32 s_lasttime = 0;
    Uint32 time = SDL_GetTicks();
    if( time - s_lasttime >= (Uint32)(1./FPS))
    {
        SDL_UpdateYUVTexture(g_videotex, NULL, 
            g_yuvdata[0], g_yuvlinesize[0],
            g_yuvdata[1], g_yuvlinesize[1],
            g_yuvdata[2], g_yuvlinesize[2]
        );
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