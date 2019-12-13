#include "ffmpeg.hpp"
#include <iostream>
#include <sstream>
#include <cassert>
#include <map>

extern "C" {
#include <libavutil/opt.h>
}

static AVDictionary *dictionary(const std::string &options)
{
	AVDictionary *dict = nullptr;
	std::istringstream iss(options);
	std::string option, key, value;

	while (getline(iss, option, ':')) {
		auto equal = option.find('=');

		if (equal != std::string::npos) {
			key = option.substr(0, equal);
			value = option.erase(0, equal + 1);

			av_dict_set(&dict, key.c_str(), value.c_str(), 0);
		}
	}

	return dict;
}

static void free_dictionary(AVDictionary *dict)
{
	AVDictionaryEntry *t = nullptr;

	while ((t = av_dict_get(dict, "", t, AV_DICT_IGNORE_SUFFIX)))
		std::cerr << "Warning: option '" << t->key << "' not used"
			  << std::endl;

	av_dict_free(&dict);
}

static AVFormatContext *ffmpeg_input_format_context(const std::string &uri,
						    const std::string &format,
						    const std::string &options)
{
	AVFormatContext *fmt_ctx = nullptr;
	AVInputFormat *ifmt = nullptr;
	AVDictionary *opts = nullptr;

	if (!format.empty()) {
		ifmt = av_find_input_format(format.c_str());
		if (!ifmt) {
			std::cerr << "Cannot find input format '" << format
				  << "'" << std::endl;
			return nullptr;
		}
	}

	opts = dictionary(options);

	if (avformat_open_input(&fmt_ctx, uri.c_str(), ifmt, &opts) != 0) {
		std::cerr << "Cannot open input file '" << uri << "'"
			  << std::endl;
		return nullptr;
	}

	free_dictionary(opts);

	if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
		std::cerr << "Cannot find input stream information" << std::endl;
		avformat_close_input(&fmt_ctx);
		return nullptr;
	}

	//av_dump_format(fmt_ctx, 0, uri.c_str(), 0);
	return fmt_ctx;
}

static AVFormatContext *ffmpeg_output_format_context(const std::string &uri)
{
	AVFormatContext *format_ctx = nullptr;
	AVOutputFormat *oformat = nullptr;
	const char *ofmt = nullptr;

	oformat = av_guess_format(nullptr, uri.c_str(), nullptr);
	if (!oformat) {
		ofmt = "mpegts";
		std::cerr << "output format not found for '" << uri
			  << "' using " << ofmt << " by default" << std::endl;
	}

	if (avformat_alloc_output_context2(&format_ctx, oformat, ofmt,
					   uri.c_str()) < 0) {
		std::cerr << "failed to allocate an output format" << std::endl;
		return nullptr;
	}

	return format_ctx;
}

static AVBufferRef *ffmpeg_hw_context(enum AVHWDeviceType type,
				      const std::string &device)
{
	AVBufferRef *hw_device_ctx = nullptr;
	const char *device_name = nullptr;

	if (!device.empty())
		device_name = device.c_str();

	if (av_hwdevice_ctx_create(&hw_device_ctx, type, device_name, nullptr, 0) < 0) {
		std::cerr << "fail to create " << av_hwdevice_get_type_name(type)
			  << " HW device" << std::endl;
		return nullptr;
	}

	std::cerr << "Using " << av_hwdevice_get_type_name(type)
		  << " HW decoding" << std::endl;
	return hw_device_ctx;
}

static void ffmpeg_hw_device_setup(AVCodecContext *ctx,
				   AVBufferRef *hw_device_ctx,
				   enum AVHWDeviceType type)
{
	static std::map<AVCodecContext*, enum AVPixelFormat> hw_maps;
	const AVCodec *codec = ctx->codec;

	for (int i = 0;; i++) {
		const AVCodecHWConfig *config = avcodec_get_hw_config(codec, i);
		if (!config) {
			std::cerr << "decoder " << codec->name
				  << " does not support device type "
				  << av_hwdevice_get_type_name(type)
				  << std::endl;
			return;
		}
		if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
		    config->device_type == type) {
			hw_maps[ctx] = config->pix_fmt;
			break;
		}
	}

	ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
	ctx->get_format =
		[](AVCodecContext *ctx,
		   const enum AVPixelFormat *pix_fmts) {
			const enum AVPixelFormat *p;

			for (p = pix_fmts; *p != -1; p++)
				if (*p == hw_maps[ctx])
					return *p;

			std::cerr << "Failed to get HW surface format." << std::endl;
			return AV_PIX_FMT_NONE;
		};
}

static AVCodecContext *ffmpeg_decoder_context(const AVCodecParameters* params,
					      AVBufferRef *hw_device_ctx,
					      enum AVHWDeviceType type,
					      const std::string &options)
{
	AVCodec *codec = nullptr;
	AVCodecContext *codec_ctx = nullptr;
	AVDictionary *opts = nullptr;
	int ret;

	codec = avcodec_find_decoder(params->codec_id);
	if (!codec)
		return nullptr;

	codec_ctx = avcodec_alloc_context3(codec);
	if (!codec_ctx)
		return nullptr;

	ret = avcodec_parameters_to_context(codec_ctx, params);
	if (ret < 0)
		goto free_context;

	if (hw_device_ctx)
		ffmpeg_hw_device_setup(codec_ctx, hw_device_ctx, type);

	opts = dictionary(options);
	av_dict_set(&opts, "refcounted_frames", "1", 0);

	ret = avcodec_open2(codec_ctx, codec, &opts);

	free_dictionary(opts);

	if (ret < 0)
		goto free_context;

	return codec_ctx;
free_context:
	avcodec_free_context(&codec_ctx);
	return nullptr;
}

static AVCodecContext *ffmpeg_encoder_context(const std::string &codec_name,
					      const std::string &options,
					      AVCodecParameters *params,
					      bool global_header)
{
	AVCodec *codec = nullptr;
	AVCodecContext *codec_ctx = nullptr;
	AVDictionary *opts = nullptr;

	codec = avcodec_find_encoder_by_name(codec_name.c_str());
	if (!codec)
		return nullptr;

	std::cerr << "Using codec '" << codec->long_name << "'" << std::endl;

	codec_ctx = avcodec_alloc_context3(codec);
	if (!codec_ctx)
		return nullptr;

	opts = dictionary(options);

	av_opt_set_dict(codec_ctx, &opts);
	av_opt_set_dict(codec_ctx->priv_data, &opts);

	free_dictionary(opts);

	codec_ctx->sample_fmt = codec_ctx->request_sample_fmt;

	if (global_header)
		codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	if (avcodec_open2(codec_ctx, codec_ctx->codec, nullptr) < 0) {
		std::cerr << "avcodec_open2 fails" << std::endl;
		goto free_context;
	}

	if (avcodec_parameters_from_context(params, codec_ctx) < 0) {
		std::cerr << "can't copy the stream parameters" << std::endl;
		goto free_context;
	}

	return codec_ctx;
free_context:
	avcodec_free_context(&codec_ctx);
	return nullptr;}


namespace av {

packet::packet() : p(av_packet_alloc()) {}
packet::~packet() { av_packet_free(&p); }

packet::packet(const packet &o)
{
	p = av_packet_clone(o.p);
}

packet &packet::operator=(const packet &o)
{
	av_packet_unref(p);
	av_packet_ref(p, o.p);
	return *this;
}

packet::packet(packet &&o)
{
	p = av_packet_clone(o.p);
	av_packet_unref(o.p);
}

packet &packet::operator=(packet &&o)
{
	if (p != o.p) {
		av_packet_unref(p);
		av_packet_move_ref(p, o.p);
	}
	return *this;
}

int packet::stream_index() const
{
	return p->stream_index;
}

void packet::stream_index(int index)
{
	p->stream_index = index;
}

void packet::add_delta_pts(int64_t delta)
{
	p->pts += delta;
	p->dts += delta;
}

frame::frame() { f = av_frame_alloc(); }
frame::~frame() { av_frame_free(&f); }

frame::frame(const frame &o)
{
	f = av_frame_clone(o.f);
}

frame &frame::operator=(const frame &o)
{
	av_frame_unref(f);
	av_frame_ref(f, o.f);
	return *this;
}

frame::frame(frame &&o)
{
	f = av_frame_clone(o.f);
	av_frame_unref(o.f);
}

frame &frame::operator=(frame &&o)
{
	if (f != o.f) {
		av_frame_unref(f);
		av_frame_move_ref(f, o.f);
	}
	return *this;
}

hw_device::hw_device(const std::string &name, const std::string &device) {
	type = av_hwdevice_find_type_by_name(name.c_str());
	if (type == AV_HWDEVICE_TYPE_NONE) {
		std::cerr << name << " hwdevice not supported" << std::endl;
		ctx = nullptr;
	} else
		ctx = ffmpeg_hw_context(type, device);
}

hw_device::~hw_device() {
	drop();
}

hw_device::hw_device(const hw_device &o) {
	if (o.ctx)
		ctx = av_buffer_ref(o.ctx);
	else
		ctx = nullptr;
	type = o.type;
}

hw_device &hw_device::operator=(const hw_device &o) {
	drop();
	if (o.ctx)
		ctx = av_buffer_ref(o.ctx);
	else
		ctx = nullptr;
	type = o.type;
	return *this;
}

hw_device::hw_device(hw_device &&o) {
	ctx = o.ctx;
	type = o.type;

	o.ctx = nullptr;
}

hw_device &hw_device::operator=(hw_device &&o) {
	if (ctx != o.ctx) {
		drop();

		ctx = o.ctx;
		type = o.type;

		o.ctx = nullptr;
	}
	return *this;
}

bool hw_device::operator!() { return ctx == nullptr; }

void hw_device::drop() {
	if (ctx)
		av_buffer_unref(&ctx);
}

codec::codec() : ctx(nullptr) {}
codec::~codec() { drop(); }

codec::codec(codec &&o)
{
	ctx = o.ctx;
	o.ctx = nullptr;
}

codec &codec::operator=(codec &&o)
{
	if (ctx != o.ctx) {
		drop();
		ctx = o.ctx;
		o.ctx = nullptr;
	}
	return *this;
}

bool codec::operator!()
{
	return (ctx == nullptr);
}

void codec::drop()
{
	avcodec_free_context(&ctx);
}


bool decoder::send(const AVPacket *p)
{
	int ret;

	ret = avcodec_send_packet(ctx, p);

	return !(ret < 0);
}

bool decoder::flush()
{
	return send(nullptr);
}

bool decoder::receive(AVFrame *f)
{
	int ret;

	ret = avcodec_receive_frame(ctx, f);

	return !(ret < 0);
}

bool decoder::operator<<(const packet &p) {
	return send(p.p);
}

bool decoder::operator>>(frame &f) {
	av_frame_unref(f.f);
	return receive(f.f);
}

input::input(input &&o)
{
	ctx = o.ctx;
	o.ctx = nullptr;
}

input &input::operator=(input &&o)
{
	if (ctx != o.ctx) {
		close();
		ctx = o.ctx;
		o.ctx = nullptr;
	}
	return *this;
}

bool input::open(const std::string &uri, const std::string &options)
{
	return open_format(uri, "", options);
}

bool input::open_format(const std::string &uri, const std::string &format,
			const std::string &options)
{
	close();

	ctx = ffmpeg_input_format_context(uri, format, options);

	return (ctx != nullptr);
}


void input::close()
{
	avformat_close_input(&ctx);
}

int input::read(AVPacket *packet)
{
	return av_read_frame(ctx, packet);
}

bool input::operator>>(packet &p) {
	av_packet_unref(p.p);
	return !(read(p.p) < 0);
}


int input::get_video_index(int id) const
{
	return av_find_best_stream(ctx, AVMEDIA_TYPE_VIDEO, id, -1, nullptr, 0);
}

int input::get_audio_index(int id) const
{
	return av_find_best_stream(ctx, AVMEDIA_TYPE_AUDIO, id, -1, nullptr, 0);
}

decoder input::get(int index, const std::string &options)
{
	return get(hw_device(), index, options);
}

decoder input::get(const hw_device &device, int index,
		   const std::string &options)
{
	decoder dec;
	AVCodecParameters *par = nullptr;

	assert((unsigned int)index < ctx->nb_streams);

	par = ctx->streams[index]->codecpar;

	dec.ctx = ffmpeg_decoder_context(par, device.ctx, device.type, options);

	return dec;
}

int64_t input::start_time_realtime() const
{
	return ctx->start_time_realtime;
}

AVRational input::time_base(int index) const
{
	assert((unsigned int)index < ctx->nb_streams);
	return ctx->streams[index]->time_base;
}

bool encoder::send(const AVFrame *frame)
{
	int ret;

	ret = avcodec_send_frame(ctx, frame);

	return !(ret < 0);
}

bool encoder::flush()
{
	return send(nullptr);
}

bool encoder::receive(AVPacket *packet)
{
	int ret;

	ret = avcodec_receive_packet(ctx, packet);

	packet->stream_index = stream_index;

	return !(ret < 0);
}

bool encoder::operator<<(const frame &f)
{
	return send(f.f);
}

bool encoder::operator>>(packet &p)
{
	av_packet_unref(p.p);
	return receive(p.p);
}

frame encoder::get_empty_frame()
{
	frame f;

	f.f->format = ctx->pix_fmt;
	f.f->width  = ctx->width;
	f.f->height = ctx->height;

	av_frame_get_buffer(f.f, 32);

	return f;
}

output::output(output &&o)
{
	ctx = o.ctx;
	write_header = o.write_header;
	write_trailer = o.write_trailer;
	time_bases = o.time_bases;

	o.ctx = nullptr;
	o.write_header = o.write_trailer = false;
	o.time_bases.clear();
}

output &output::operator=(output &&o)
{
	if (ctx != o.ctx) {
		ctx = o.ctx;
		write_header = o.write_header;
		write_trailer = o.write_trailer;
		time_bases = o.time_bases;

		o.ctx = nullptr;
		o.write_header = o.write_trailer = false;
		o.time_bases.clear();
	}
	return *this;
}

bool output::open(const std::string &uri)
{
	close();

	ctx = ffmpeg_output_format_context(uri);
	if (!ctx)
		return false;

	if (!(ctx->oformat->flags & AVFMT_NOFILE))
		if (avio_open(&ctx->pb, uri.c_str(), AVIO_FLAG_WRITE) < 0)
			return false;

	write_header = true;
	write_trailer = false;
	return true;
}

encoder output::add_stream(const std::string &codec, const std::string &options)
{
	encoder enc;
	AVStream *stream;

	stream = avformat_new_stream(ctx, nullptr);
	if (!stream) {
		std::cerr << "avformat_new_stream fails" << std::endl;
		return enc;
	}

	enc.ctx = ffmpeg_encoder_context(
		codec, options, stream->codecpar,
		ctx->oformat->flags & AVFMT_GLOBALHEADER);

	enc.stream_index = stream->id = ctx->nb_streams - 1;

	time_bases.resize(ctx->nb_streams);
	time_bases[stream->id] = enc.ctx->time_base;
	return enc;
}

int output::add_stream(const input &in, int index)
{
	AVStream *stream;

	stream = avformat_new_stream(ctx, nullptr);
	if (!stream) {
		std::cerr << "avformat_new_stream fails" << std::endl;
		return -1;
	}

	if (avcodec_parameters_copy(stream->codecpar,
				    in.ctx->streams[index]->codecpar) < 0) {
		std::cerr << "Failed to copy codec parameters" << std::endl;
		return -1;
	}
	stream->codecpar->codec_tag = 0;
	stream->id = ctx->nb_streams - 1;

	time_bases.resize(ctx->nb_streams);
	time_bases[stream->id] = in.ctx->streams[index]->time_base;
	return stream->id;
}

int output::write(AVPacket *packet)
{
	if (write_header) {
		int ret;

		//av_dump_format(ctx, 0, "output", 1);

		ret = avformat_write_header(ctx, nullptr);
		if (ret < 0)
			return ret;

		write_header = false;
		write_trailer = true;
	}

	assert((unsigned int)packet->stream_index < ctx->nb_streams);

	av_packet_rescale_ts(packet, time_bases[packet->stream_index],
			     ctx->streams[packet->stream_index]->time_base);
	packet->pos = -1;
	return av_interleaved_write_frame(ctx, packet);
}

bool output::operator<<(const packet &p)
{
	return !(write(p.p) < 0);
}

void output::close()
{
	if (!ctx)
		return;

	if (write_trailer)
		av_write_trailer(ctx);

	if (!(ctx->oformat->flags & AVFMT_NOFILE))
		avio_closep(&ctx->pb);

	avformat_free_context(ctx);
	ctx = nullptr;
}

}
