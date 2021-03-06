#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <SDL2/SDL.h>
#include <stdio.h>

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000
#define OUT_SAMPLE_RATE 44100 //44100
#define OUT_SAMPLE_FMT AV_SAMPLE_FMT_FLT


typedef struct PacketQueue {
	AVPacketList *first_pkt, *last_pkt;
	int nb_packets;
	int size;
	SDL_mutex *mutex;
	SDL_cond *cond;
} PacketQueue;

int quit = 0;
PacketQueue a_queue;// must be declare after typedef=.=
SwrContext *swr;
int put_all = 0;

void packet_queue_init(PacketQueue *q) {
	memset(q, 0, sizeof(PacketQueue));
	q->mutex = SDL_CreateMutex();
	q->cond = SDL_CreateCond();
}

/**
* @return 0 on success, negative on error
*/
int packet_queue_put(PacketQueue *q, AVPacket *pkt) {
	AVPacketList *pkt1;
	// init packet
	if(av_packet_ref(pkt, pkt) <0){
		return -1;
	}
	pkt1 = av_malloc(sizeof(AVPacketList));
	if(!pkt1) {
		return -1;
	}
	pkt1->pkt = *pkt;
	pkt1->next = NULL;
	
	SDL_LockMutex(q->mutex);

	if(!q->last_pkt)
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

/**
* @return 0 on success, negative on error
**/
int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block) {
	AVPacketList *pkt1;
	int ret;

	SDL_LockMutex(q->mutex);

	for(;;) {
		if(quit) {
			ret = -1;
			break;
		}
		pkt1 = q->first_pkt;
		if(pkt1) {//not null
			q->first_pkt = pkt1 ->next;
			if(!q->first_pkt)
				q->last_pkt = NULL;
			q->nb_packets--;
			q->size -= pkt1->pkt.size;
			*pkt = pkt1->pkt;
			av_free(pkt1);
			ret = 1;
			break;
		} else if(!block) {
			ret = 0;
			break;
		} else {
			// wait only when there still has data
			// TODO there may be problem with this solution due to updating
			//  put_all without ensuring in multithread program
			if(put_all!=1)
				SDL_CondWait(q->cond, q->mutex);
			else {
				ret = -2;
				break;
			}
		}
	}

	SDL_UnlockMutex(q->mutex);
	return ret;
}

void audio_callback(void *userdata, Uint8* stream, int len) {
	AVCodecContext *acodec_ctx = (AVCodecContext *) userdata;
	int len1, audio_size;
	//why multiply MAX_AUDIO_FRAME_SIZE by 3 then divide by 2?
	static uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE*3)/2];
	static unsigned int audio_buf_size =0;
	static unsigned int audio_buf_index =0;

	while(len > 0) {
		if(audio_buf_index >= audio_buf_size) {
			//already play all data, get more
			audio_size = audio_decode_frame(acodec_ctx, audio_buf, sizeof(audio_buf));
			if(audio_size<0){
				// error, try output silence
				audio_buf_size = 1024;
				memset(audio_buf, 0, audio_buf_size);
			}else {
				audio_buf_size = audio_size;
			}
			audio_buf_index = 0;
		}
		len1 = audio_buf_size - audio_buf_index;
		if(len1 > len) {
			len1 = len;
		}
		memcpy(stream, (uint8_t *)audio_buf+audio_buf_index, len1);
		len -= len1;
		stream += len1;
		audio_buf_index += len1;
	}
}
/**
* @return number of data on success, negative value when error/fail
**/
int audio_decode_frame(AVCodecContext * acodec_ctx, uint8_t *audio_buf, int buf_size) {
	static AVPacket pkt;
	static uint8_t *audio_pkt_data = NULL;
	static int audio_pkt_size = 0;
	static AVFrame frame, *reframe;
	int len1, data_size = 0, ret;
	for(;;) {
		// 1 packet can contains multiple frames
		while(audio_pkt_size>0) {
			int got_frame = 0;

			//TODO new API
			// got_frame = avcodec_receive_frame(acodec_ctx, &frame);
			// len1 = frame.linesize[0];
			// end new  API

			len1 = avcodec_decode_audio4(acodec_ctx, &frame, &got_frame, &pkt);
			int len2 = frame.pkt_size;
			// printf("len1 %d len2 %d\n", len1, len2);

			if(len1 < 0) {
				/* if error, skip frame */
				audio_pkt_size = 0;
				break;
	    	}

			audio_pkt_data += len1;
			audio_pkt_size -= len1;

			data_size = 0;
			if(got_frame) {
				// get size of data in frame
				// data_size = av_samples_get_buffer_size(NULL, acodec_ctx->channels,
				// 	frame.nb_samples, acodec_ctx->sample_fmt, 0);
				// memcpy(audio_buf, frame.data[0], data_size);

				// instead of get data from input audio frame, convert
				// input frame to SDL2 recognizable audio frame
				reframe = av_frame_alloc();
				reframe->channel_layout = frame.channel_layout;
				reframe->sample_rate = frame.sample_rate;
				reframe->format = OUT_SAMPLE_FMT;
				int ret = swr_convert_frame(swr, reframe, &frame);
				if(ret < 0 ){
					// convert error, try next frame
					continue;
				}
				data_size = av_samples_get_buffer_size(NULL, reframe->channels,
					reframe->nb_samples, reframe->format, 0);
				memcpy(audio_buf, reframe->data[0], data_size);
				av_frame_free(&reframe);
			}
			if(data_size <= 0) {
				// nodata, get more frames
				continue;
			}
			return data_size;
		}
		if(pkt.data) {// not null
			av_packet_unref(&pkt);
		}
		if(quit) {
			return -1;
		}
		// get next packet
		if(packet_queue_get(&a_queue, &pkt, 1) < 0) {// no more packet
			printf("Play audio completed, try shutdown\n");
			quit = 1; // stop program
			return -1;
		}
		
		//TODO new API
		// avcodec_send_packet(acodec_ctx, &pkt);
		// end new API

		audio_pkt_data = pkt.data;
		audio_pkt_size = pkt.size;
	}
}

int main(int argc, char* argv[]) {
	printf("Play music from video file\n");
	if(argc<2) {
		printf("Missing video file\n");
		return -1;
	}
	char* vf_path = argv[1];
	AVFormatContext* fmt_ctx = NULL;
	AVCodecContext* vcodec_ctx = NULL;
	AVCodec* vcodec = NULL;
	AVCodecContext* acodec_ctx = NULL;
	AVCodec* acodec = NULL;
	AVFrame* frame = NULL;
	AVPacket packet;

	int ret = -1, i, v_stream_idx = -1, a_stream_idx = -1;
	av_register_all();
	ret = avformat_open_input(&fmt_ctx, vf_path, NULL, NULL);
	if(ret < 0){
		printf("Can not open %s: %s\n", vf_path, av_err2str(ret));
		return -1;
	}
	ret = avformat_find_stream_info(fmt_ctx, NULL);
	if( ret < 0) {
		printf("Can not find stream info: %s\n", av_err2str(ret));
		return -1;
	}
	av_dump_format(fmt_ctx, 0, vf_path, 0);

	for(i = 0; i < fmt_ctx->nb_streams; i++) {
		if(fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO & v_stream_idx < 0) {
			v_stream_idx = i;
		} else if(fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && a_stream_idx <0) {
			a_stream_idx = i;
		}
	}

	if(v_stream_idx < 0) {
		printf("Can not find video stream\n");
		goto end;
	}

	if(a_stream_idx < 0) {
		printf("Can not find audio stream\n");
		goto end;
	}

	vcodec_ctx = avcodec_alloc_context3(NULL);
	avcodec_parameters_to_context(vcodec_ctx, fmt_ctx->streams[v_stream_idx]->codecpar);
	acodec_ctx = avcodec_alloc_context3(NULL);
	avcodec_parameters_to_context(acodec_ctx, fmt_ctx->streams[a_stream_idx]->codecpar);

	vcodec = avcodec_find_decoder(vcodec_ctx->codec_id);
	acodec = avcodec_find_decoder(acodec_ctx->codec_id);
	if(vcodec == NULL) {
		printf("Can not find video decoder\n");
	}
	if(acodec == NULL) {
		printf("Can not find audio decoder\n");
	}

	ret = avcodec_open2(acodec_ctx, acodec, NULL);
	if(ret < 0){
		printf("Can not open audio decoder\n");
	}
	ret = avcodec_open2(vcodec_ctx, vcodec, NULL);
	if(ret < 0){
		printf("Can not open video decoder\n");
	}

	// create audio resampling, for converting audio from X format
	// to compatible format with SDL2
	swr = swr_alloc_set_opts(NULL,  // we're allocating a new context
                      acodec_ctx->channel_layout,  // out_ch_layout
                      OUT_SAMPLE_FMT,    // out_sample_fmt
                      acodec_ctx->sample_rate,                // out_sample_rate
                      acodec_ctx->channel_layout, // in_ch_layout: stereo, blabla
                      acodec_ctx->sample_fmt,   // in_sample_fmt
                      acodec_ctx->sample_rate,                // in_sample_rate
                      0,                    // log_offset
                      NULL);
	swr_init(swr);

	if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
	    fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
	    goto end;
	}


	SDL_Event event;
	// setup SDL audio
	SDL_AudioSpec wanted_spec, spec;

	SDL_memset(&wanted_spec, 0, sizeof(wanted_spec));
	wanted_spec.freq = acodec_ctx->sample_rate;
	wanted_spec.format = AUDIO_F32LSB;
	wanted_spec.channels = acodec_ctx->channels;
	wanted_spec.silence = 0;
	wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
	wanted_spec.callback = audio_callback;
	wanted_spec.userdata = acodec_ctx;

	if(SDL_OpenAudio(&wanted_spec, &spec) <0){
		printf("SDL_OpenAudio err: %s\n", SDL_GetError());
		goto enda;
	}

	packet_queue_init(&a_queue);
	SDL_PauseAudio(0);

	frame = av_frame_alloc();

	int ret1, ret2;
	while(quit != 1) {
		SDL_PollEvent(&event);
		switch(event.type) {
			case SDL_QUIT:
				printf("SDL receive stop, try shutdown\n");
				quit = 1;
				break;
			default:
				break;
		}
		ret  = av_read_frame(fmt_ctx, &packet);
		if(ret < 0) {
			// when error/finished, just sleep main thread 1 second then continue
			// or you can use nanosleep(man 2 nanosleep)
			if(put_all!=1){
				printf("Complete read frames\n");
				put_all = 1;
			}
			sleep(1);
			continue;
		}
		if(packet.stream_index == v_stream_idx) {
			// ret1 = avcodec_send_packet(vcodec_ctx, &packet);
			// ret2 = avcodec_receive_frame(vcodec_ctx, frame);
			// if(ret2 < 0 ){
			// 	continue;
			// }
			av_packet_unref(&packet);
		}else if(packet.stream_index == a_stream_idx) {
			packet_queue_put(&a_queue, &packet);
		} else {
			av_packet_unref(&packet);
		}
	}

	SDL_Quit();
	av_frame_free(&frame);
	swr_free(&swr);
	enda:
	avcodec_close(vcodec_ctx);
	avcodec_free_context(&vcodec_ctx);
	avcodec_close(acodec_ctx);
	avcodec_free_context(&acodec_ctx);
	end:
	avformat_close_input(&fmt_ctx);
	printf("Shutdown\n");
}