#include <stdio.h>
#include <assert.h>
#include <SDL.h>
#include <SDL_thread.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/dict.h>
#include <libswresample/swresample.h>
};

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000
#define OUT_SAMPLE_RATE 44100 //44100
#define OUT_SAMPLE_FMT AV_SAMPLE_FMT_FLT

struct PacketQueue {
  AVPacketList *first_pkt, *last_pkt;
  int nb_packets;
  int size;
  SDL_mutex *mutex;
  SDL_cond *cond;
};

AVFrame wanted_frame;
PacketQueue audioq;

SwrContext *swr;
int quit = 0;

void packet_queue_init(PacketQueue *q) 
{
  memset(q, 0, sizeof(PacketQueue));
  q->mutex = SDL_CreateMutex();
  q->cond = SDL_CreateCond();
}

int packet_queue_put(PacketQueue *q, AVPacket *pkt) 
{
  AVPacketList *pkt1;
  if(av_dup_packet(pkt) < 0) {
    return -1;
  }
  pkt1 = (AVPacketList *)av_malloc(sizeof(AVPacketList));
  if (!pkt1)
    return -1;
  pkt1->pkt = *pkt;
  pkt1->next = NULL;
  
  
  SDL_LockMutex(q->mutex);
  
  if (!q->last_pkt)
    q->first_pkt = pkt1;
  else
    q->last_pkt->next = pkt1;
  q->last_pkt = pkt1;
  q->nb_packets++;
  q->size += pkt1->pkt.size;
  SDL_CondSignal(q->cond);
  
  SDL_UnlockMutex(q->mutex);
  return 0;
}

static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block)
{
  AVPacketList *pkt1;
  int ret;
  
  SDL_LockMutex(q->mutex);
  
  for(;;) {
    
    if(quit) {
      ret = -1;
      break;
    }

    pkt1 = q->first_pkt;
    if (pkt1) {
      q->first_pkt = pkt1->next;
      if (!q->first_pkt)
				q->last_pkt = NULL;
      q->nb_packets--;
      q->size -= pkt1->pkt.size;
      *pkt = pkt1->pkt;
      av_free(pkt1);
      ret = 1;
      break;
    } else if (!block) {
      ret = 0;
      break;
    } else {
      SDL_CondWait(q->cond, q->mutex);
    }
  }
  SDL_UnlockMutex(q->mutex);
  return ret;
}

int audio_decode_frame(AVCodecContext *aCodecCtx, uint8_t *audio_buf, int buf_size) 
{
  AVPacket   packet;
     AVFrame    *frame;
     int        got_frame;
     int        pkt_size = 0;
//     uint8_t    *pkt_data = NULL;
     int        decode_len;
     int        try_again = 0;
     long long  audio_buf_index = 0;
     long long  data_size = 0;
     SwrContext *swr_ctx = NULL;
     int        convert_len = 0;
     int        convert_all = 0;

     if (packet_queue_get(&audioq, &packet, 1) < 0)
     {
          assert(0);
          return -1;
     }

//     pkt_data = packet.data;
     pkt_size = packet.size;
//     fprintf(ERR_STREAM, "pkt_size = %d\n", pkt_size);

     frame = av_frame_alloc();
     while(pkt_size > 0)
     {
//          memset(frame, 0, sizeof(AVFrame));
          //pcodec_ctx:解码器信息
          //frame:输出，存数据到frame
          //got_frame:输出。0代表有frame取了，不意味发生了错误。
          //packet:输入，取数据解码。
          decode_len = avcodec_decode_audio4(aCodecCtx,
                  frame, &got_frame, &packet);
          if (decode_len < 0) //解码出错
          {
               //重解, 这里如果一直<0呢？
               assert(0);
               if (try_again == 0)
               {
                    try_again++;
                    continue;
               }
               try_again = 0;
          }


          if (got_frame)
          {

 /*              //用定的音频参数获取样本缓冲区大小
               data_size = av_samples_get_buffer_size(NULL,
                       pcodec_ctx->channels, frame->nb_samples,
                       pcodec_ctx->sample_fmt, 1);

               assert(data_size <= buf_size);
//               memcpy(audio_buf + audio_buf_index, frame->data[0], data_size);
*/
              //chnanels: 通道数量, 仅用于音频
              //channel_layout: 通道布局。
              //多音频通道的流，一个通道布局可以具体描述其配置情况.通道布局这个概念不懂。
              //大概指的是单声道(mono)，立体声道（stereo), 四声道之类的吧？
              //详见源码及：https://xdsnet.gitbooks.io/other-doc-cn-ffmpeg/content/ffmpeg-doc-cn-07.html#%E9%80%9A%E9%81%93%E5%B8%83%E5%B1%80


              if (frame->channels > 0 && frame->channel_layout == 0)
              {
                   //获取默认布局，默认应该了stereo吧？
                   frame->channel_layout = av_get_default_channel_layout(frame->channels);
              }
              else if (frame->channels == 0 && frame->channel_layout > 0)
              {
                  frame->channels = av_get_channel_layout_nb_channels(frame->channel_layout);
              }


              if (swr_ctx != NULL)
              {
                   swr_free(&swr_ctx);
                   swr_ctx = NULL;
              }

              //设置common parameters
              //2,3,4是output参数，4,5,6是input参数。
              swr_ctx = swr_alloc_set_opts(NULL, wanted_frame.channel_layout,
                      (AVSampleFormat)wanted_frame.format,
                      wanted_frame.sample_rate, frame->channel_layout,
                      (AVSampleFormat)frame->format, frame->sample_rate, 0, NULL);
              //初始化
              if (swr_ctx == NULL || swr_init(swr_ctx) < 0)
              {
                   assert(0);
                   break;
              }
              //av_rescale_rnd(): 用指定的方式队64bit整数进行舍入(rnd:rounding),
              //使如a*b/c之类的操作不会溢出。
              //swr_get_delay(): 返回 第二个参数分之一（下面是：1/frame->sample_rate)
              //AVRouding是一个enum，1的意思是round away from zero.
    /*
              int dst_nb_samples = av_rescale_rnd(
                      swr_get_delay(swr_ctx, frame->sample_rate) + frame->nb_samples,
                      wanted_frame.sample_rate, wanted_frame.format,
                      AVRounding(1));
    */

              //转换音频。把frame中的音频转换后放到audio_buf中。
              //第2，3参数为output， 第4，5为input。
              //可以使用#define AVCODE_MAX_AUDIO_FRAME_SIZE 192000 
              //把dst_nb_samples替换掉, 最大的采样频率是192kHz.
              convert_len = swr_convert(swr_ctx, 
                                &audio_buf + audio_buf_index,
                                MAX_AUDIO_FRAME_SIZE,
                                (const uint8_t **)frame->data, 
                                frame->nb_samples);

              printf("decode len = %d, convert_len = %d\n", decode_len, convert_len);
              //解码了多少，解码到了哪里
    //          pkt_data += decode_len;
              pkt_size -= decode_len;
              //转换后的有效数据存到了哪里，又audio_buf_index标记
              audio_buf_index += convert_len;//data_size;
              //返回所有转换后的有效数据的长度
              convert_all += convert_len;
         }
     }
     return wanted_frame.channels * convert_all * av_get_bytes_per_sample((AVSampleFormat)wanted_frame.format);
//     return audio_buf_index;
}

void audio_callback(void *userdata, Uint8 *stream, int len) {

  AVCodecContext *aCodecCtx = (AVCodecContext *)userdata;
  int len1 = 0, audio_size = 0;

  static uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
  static unsigned int audio_buf_size = 0;
  static unsigned int audio_buf_index = 0;

	SDL_memset(stream, 0, len);
	
  while(len > 0) {
    if(audio_buf_index >= audio_buf_size) {
      /* We have already sent all our data; get more */
      audio_size = audio_decode_frame(aCodecCtx, audio_buf, sizeof(audio_buf));
      if(audio_size < 0) {
				/* If error, output silence */
				audio_buf_size = 1024; // arbitrary?
				memset(audio_buf, 0, audio_buf_size);
      } else {
				audio_buf_size = audio_size;
      }
      audio_buf_index = 0;
    }
    len1 = audio_buf_size - audio_buf_index;
    if(len1 > len)
      len1 = len;
		
		SDL_MixAudioFormat(stream, (uint8_t *)audio_buf + audio_buf_index, AUDIO_S16SYS, len, SDL_MIX_MAXVOLUME);
		
		//SDL_MixAudio(stream, (uint8_t*)audio_buf + audio_buf_index, len, SDL_MIX_MAXVOLUME);
    //memcpy(stream, (uint8_t *)audio_buf + audio_buf_index, len1);
    len -= len1;
    stream += len1;
    audio_buf_index += len1;
  }
}

int main(int argc, char *argv[]) {
	AVFormatContext *pFormatCtx = NULL;
	int             i, videoStream, audioStream;
	AVCodecContext *pCodecCtxOrig = NULL;
	AVCodecContext *pCodecCtx = NULL;
	AVCodec 			 *pCodec = NULL;
	AVFrame *pFrame = NULL;
	AVPacket packet;
	int frameFinished;
	SwsContext *sws_ctx = NULL;
	
	AVCodecContext  *aCodecCtxOrig = NULL;
  AVCodecContext  *aCodecCtx = NULL;
  AVCodec         *aCodec = NULL;
	
	SDL_AudioSpec   wanted_spec, spec;
	SDL_AudioDeviceID dev;
	SDL_Event event;
	SDL_Window *screen;
	SDL_Renderer *renderer;
	SDL_Texture *texture;
	Uint8 *yPlane, *uPlane, *vPlane;
	size_t yPlaneSz, uvPlaneSz;
	int uvPitch;

	if (argc < 2) {
			fprintf(stderr, "Usage: test <file>\n");
			exit(1);
	}
	// Register all formats and codecs
	av_register_all();

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
			fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
			exit(1);
	}

	// Open video file
	if (avformat_open_input(&pFormatCtx, argv[1], NULL, NULL) != 0)
			return -1; // Couldn't open file

	// Retrieve stream information
	if (avformat_find_stream_info(pFormatCtx, NULL) < 0)
			return -1; // Couldn't find stream information

	// Dump information about file onto standard error
	av_dump_format(pFormatCtx, 0, argv[1], 0);

	// Find the first video stream
	videoStream=-1;
	audioStream=-1;
	for(i=0; i<pFormatCtx->nb_streams; i++) {
		if(pFormatCtx->streams[i]->codec->codec_type==AVMEDIA_TYPE_VIDEO && videoStream < 0) {
			videoStream = i;
		}
		if(pFormatCtx->streams[i]->codec->codec_type==AVMEDIA_TYPE_AUDIO && audioStream < 0) {
			audioStream = i;
		}
	}
	if(videoStream==-1)
		return -1; // Didn't find a video stream
	if(audioStream==-1)
		return -1;

	// Get a pointer to the codec context for the video stream
	aCodecCtx = pFormatCtx->streams[audioStream]->codec;
	// Find the decoder for the video stream
	aCodec = avcodec_find_decoder(aCodecCtx->codec_id);
	if (aCodec == NULL) {
			fprintf(stderr, "Unsupported codec!\n");
			return -1; // Codec not found
	}

//	// Copy context
//	aCodecCtx = avcodec_alloc_context3(aCodec);
//	if (avcodec_copy_context(aCodecCtx, aCodecCtxOrig) != 0) {
//			fprintf(stderr, "Couldn't copy codec context");
//			return -1; // Error copying codec context
//	}
//	if (aCodecCtx->sample_fmt == AV_SAMPLE_FMT_S16P) {
//		aCodecCtx->request_sample_fmt = AV_SAMPLE_FMT_S16;
//	}

	// Set audio settings from codec info
  wanted_spec.freq = aCodecCtx->sample_rate;
  wanted_spec.format = AUDIO_S16SYS;
  wanted_spec.channels = aCodecCtx->channels;
  wanted_spec.silence = 0;
  wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
  wanted_spec.callback = audio_callback;
  wanted_spec.userdata = aCodecCtx;
  
	dev = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &spec, SDL_AUDIO_ALLOW_ANY_CHANGE);
  if(dev <= 0) {
    fprintf(stderr, "SDL_OpenAudioDevice: %s\n", SDL_GetError());
    return -1;
  }

	//设置参数，供解码时候用, swr_alloc_set_opts的in部分参数
  wanted_frame.format         = AV_SAMPLE_FMT_S16;
  wanted_frame.sample_rate    = spec.freq;
  wanted_frame.channel_layout = av_get_default_channel_layout(spec.channels);
  wanted_frame.channels       = spec.channels;

	avcodec_open2(aCodecCtx, aCodec, NULL);
	
	// audio_st = pFormatCtx->streams[index]
  packet_queue_init(&audioq);

	SDL_PauseAudioDevice(dev, 0);
	
	// Get a pointer to the codec context for the video stream
  pCodecCtxOrig=pFormatCtx->streams[videoStream]->codec;
  
  // Find the decoder for the video stream
  pCodec=avcodec_find_decoder(pCodecCtxOrig->codec_id);
  if(pCodec==NULL) {
    fprintf(stderr, "Unsupported codec!\n");
    return -1; // Codec not found
  }

  // Copy context
  pCodecCtx = avcodec_alloc_context3(pCodec);
  if(avcodec_copy_context(pCodecCtx, pCodecCtxOrig) != 0) {
    fprintf(stderr, "Couldn't copy codec context");
    return -1; // Error copying codec context
  }
	
	// Open codec
	if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0)
			return -1; // Could not open codec

	// Allocate video frame
	pFrame = av_frame_alloc();

	// Make a screen to put our video
	screen = SDL_CreateWindow(
					"FFmpeg Tutorial",
					SDL_WINDOWPOS_UNDEFINED,
					SDL_WINDOWPOS_UNDEFINED,
					pCodecCtx->width,
					pCodecCtx->height,
					0
			);

	if (!screen) {
			fprintf(stderr, "SDL: could not create window - exiting\n");
			exit(1);
	}

	renderer = SDL_CreateRenderer(screen, -1, 0);
	if (!renderer) {
			fprintf(stderr, "SDL: could not create renderer - exiting\n");
			exit(1);
	}

	// Allocate a place to put our YUV image on that screen
	texture = SDL_CreateTexture(
					renderer,
					SDL_PIXELFORMAT_YV12,
					SDL_TEXTUREACCESS_STREAMING,
					pCodecCtx->width,
					pCodecCtx->height
			);
	if (!texture) {
			fprintf(stderr, "SDL: could not create texture - exiting\n");
			exit(1);
	}

	// create audio resampling, for converting audio from X format
	// to compatible format with SDL2
	swr = swr_alloc_set_opts(NULL,  // we're allocating a new context
                      aCodecCtx->channel_layout,  // out_ch_layout
                      OUT_SAMPLE_FMT,    // out_sample_fmt
                      aCodecCtx->sample_rate,                // out_sample_rate
                      aCodecCtx->channel_layout, // in_ch_layout: stereo, blabla
                      aCodecCtx->sample_fmt,   // in_sample_fmt
                      aCodecCtx->sample_rate,                // in_sample_rate
                      0,                    // log_offset
                      NULL);
	swr_init(swr);
	
	// initialize SWS context for software scaling
	sws_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height,
					pCodecCtx->pix_fmt, pCodecCtx->width, pCodecCtx->height,
					AV_PIX_FMT_YUV420P,
					SWS_BILINEAR,
					NULL,
					NULL,
					NULL);

	// set up YV12 pixel array (12 bits per pixel)
	yPlaneSz = pCodecCtx->width * pCodecCtx->height;
	uvPlaneSz = pCodecCtx->width * pCodecCtx->height / 4;
	yPlane = (Uint8*)malloc(yPlaneSz);
	uPlane = (Uint8*)malloc(uvPlaneSz);
	vPlane = (Uint8*)malloc(uvPlaneSz);
	if (!yPlane || !uPlane || !vPlane) {
			fprintf(stderr, "Could not allocate pixel buffers - exiting\n");
			exit(1);
	}

	i=0;
	uvPitch = pCodecCtx->width / 2;
	
	while (av_read_frame(pFormatCtx, &packet) >= 0) {
		// Is this a packet from the video stream?
		if (packet.stream_index == videoStream) {
				// Decode video frame
				avcodec_decode_video2(pCodecCtx, pFrame, &frameFinished, &packet);

				// Did we get a video frame?
				if (frameFinished) {
						AVPicture pict;
						pict.data[0] = yPlane;
						pict.data[1] = uPlane;
						pict.data[2] = vPlane;
						pict.linesize[0] = pCodecCtx->width;
						pict.linesize[1] = uvPitch;
						pict.linesize[2] = uvPitch;

						// Convert the image into YUV format that SDL uses
						sws_scale(sws_ctx, (uint8_t const * const *) pFrame->data,
										pFrame->linesize, 0, pCodecCtx->height, pict.data,
										pict.linesize);

						SDL_UpdateYUVTexture(
										texture,
										NULL,
										yPlane,
										pCodecCtx->width,
										uPlane,
										uvPitch,
										vPlane,
										uvPitch
								);

						SDL_RenderClear(renderer);
						SDL_RenderCopy(renderer, texture, NULL, NULL);
						SDL_RenderPresent(renderer);

				}
		} else if(packet.stream_index==audioStream) {
      packet_queue_put(&audioq, &packet);
    } else {
      av_free_packet(&packet);
    }

		SDL_PollEvent(&event);
		switch (event.type) {
		case SDL_QUIT:
			quit = 1;
			SDL_CloseAudioDevice(dev);
			SDL_DestroyTexture(texture);
			SDL_DestroyRenderer(renderer);
			SDL_DestroyWindow(screen);
			SDL_Quit();
			exit(0);
			break;
		default:
			break;
		}
	}

	// Free the YUV frame
	av_frame_free(&pFrame);
	free(yPlane);
	free(uPlane);
	free(vPlane);

	// Close the codec
	avcodec_close(pCodecCtxOrig);
  avcodec_close(pCodecCtx);
  avcodec_close(aCodecCtxOrig);
  avcodec_close(aCodecCtx);
	// Close the video file
	avformat_close_input(&pFormatCtx);

	return 0;
}