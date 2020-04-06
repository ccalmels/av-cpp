#include "ffmpeg.hpp"
#include <iostream>
#include <sstream>
#include <cassert>
#include <map>

extern "C" {
#include <libavutil/opt.h>
}

static std::string dictionary_to_string(const AVDictionary *d)
{
	std::string ret;
	char *buf = nullptr;

	av_dict_get_string(d, &buf, '=', ':');
	if (buf && strlen(buf))
		ret = buf;
	av_freep(&buf);
	return ret;
}


struct dictionary {
	AVDictionary *d;

	dictionary(const std::string &options) : d(nullptr) {
		av_dict_parse_string(&d, options.c_str(), "=", ":", 0);
	}
	~dictionary() {
		std::string options = dictionary_to_string(d);

		if (!options.empty())
			std::cerr << "Warning: unused options: "
				  << options << std::endl;

		av_dict_free(&d);
	}
	AVDictionary **ptr() { return &d; }
};

static AVFormatContext *ffmpeg_input_format_context(const std::string &uri,
						    const std::string &format,
						    const std::string &options)
{
	AVFormatContext *fmt_ctx = nullptr;
	AVInputFormat *ifmt = nullptr;
	int ret;

	if (!format.empty()) {
		ifmt = av_find_input_format(format.c_str());
		if (!ifmt) {
			std::cerr << "Cannot find input format '" << format
				  << "'" << std::endl;
			return nullptr;
		}
	}

	ret = avformat_open_input(&fmt_ctx, uri.c_str(), ifmt,
				  dictionary(options).ptr());
	if (ret) {
		std::cerr << "Cannot open input file '" << uri << "'"
			  << std::endl;
		return nullptr;
	}

	if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
		std::cerr << "Cannot find input stream infos" << std::endl;
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

	if (av_hwdevice_ctx_create(&hw_device_ctx, type,device_name,
				   nullptr, 0) < 0) {
		std::cerr << "fail to create " << av_hwdevice_get_type_name(type)
			  << " HW device" << std::endl;
		return nullptr;
	}

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
		[](AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts)
		{
			const enum AVPixelFormat *p;

			for (p = pix_fmts; *p != -1; p++)
				if (*p == hw_maps[ctx])
					return *p;

			std::cerr << "Failed to get HW pixel format." << std::endl;
			return AV_PIX_FMT_NONE;
		};
}

static AVBufferRef *ffmpeg_hw_frames_ctx(AVBufferRef* hw_device_ctx,
					 AVPixelFormat sw_format,
					 int width, int height)
{
	AVHWFramesConstraints *constraints;
	AVPixelFormat hw_format;
	AVBufferRef *hw_frames_ctx;
	AVHWFramesContext *frames_ctx;

	constraints =
		av_hwdevice_get_hwframe_constraints(hw_device_ctx, nullptr);
	if (!constraints)
		return nullptr;

	hw_format = constraints->valid_hw_formats[0];

	av_hwframe_constraints_free(&constraints);

	hw_frames_ctx = av_hwframe_ctx_alloc(hw_device_ctx);
	if (!hw_frames_ctx)
		return nullptr;

	frames_ctx = (AVHWFramesContext*)hw_frames_ctx->data;
	frames_ctx->format = hw_format;
	frames_ctx->sw_format = sw_format;
	frames_ctx->width = width;
	frames_ctx->height = height;

	if  (av_hwframe_ctx_init(hw_frames_ctx) < 0) {
		av_buffer_unref(&hw_frames_ctx);
		return nullptr;
	}

	return hw_frames_ctx;
}

static AVCodecContext *ffmpeg_decoder_context(const std::string &codec_name,
					      const AVCodecParameters* params,
					      AVBufferRef *hw_device_ctx,
					      enum AVHWDeviceType type,
					      const std::string &options)
{
	AVCodec *codec = nullptr;
	AVCodecContext *codec_ctx = nullptr;
	int ret;

	if  (codec_name.empty())
		codec = avcodec_find_decoder(params->codec_id);
	else
		codec = avcodec_find_decoder_by_name(codec_name.c_str());
	if (!codec)
		return nullptr;

	std::cerr << "Using decoder '" << codec->long_name << "'";
	if (hw_device_ctx)
		std::cerr << " with '" << av_hwdevice_get_type_name(type)
			  << "' HW accel";
	std::cerr << std::endl;

	codec_ctx = avcodec_alloc_context3(codec);
	if (!codec_ctx)
		return nullptr;

	ret = avcodec_parameters_to_context(codec_ctx, params);
	if (ret < 0)
		goto free_context;

	if (hw_device_ctx)
		ffmpeg_hw_device_setup(codec_ctx, hw_device_ctx, type);

	ret = avcodec_open2(codec_ctx, codec,
			    dictionary(std::string("refcounted_frames=1:")
				       + options).ptr());
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
					      bool global_header,
					      AVBufferRef *hw_frames_ref)
{
	AVCodec *codec = nullptr;
	AVCodecContext *codec_ctx = nullptr;

	codec = avcodec_find_encoder_by_name(codec_name.c_str());
	if (!codec)
		return nullptr;

	std::cerr << "Using encoder '" << codec->long_name << "'" << std::endl;

	codec_ctx = avcodec_alloc_context3(codec);
	if (!codec_ctx)
		return nullptr;

	auto d = dictionary(options);

	av_opt_set_dict(codec_ctx, d.ptr());
	av_opt_set_dict(codec_ctx->priv_data, d.ptr());

	codec_ctx->sample_fmt = codec_ctx->request_sample_fmt;

	if (global_header)
		codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	if (hw_frames_ref) {
		AVHWFramesContext *frames_ctx
			= (AVHWFramesContext *)(hw_frames_ref->data);

		codec_ctx->pix_fmt = frames_ctx->format;
		codec_ctx->width   = frames_ctx->width;
		codec_ctx->height  = frames_ctx->height;

		codec_ctx->hw_frames_ctx = av_buffer_ref(hw_frames_ref);
	}

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
	return nullptr;
}

namespace av {

packet::packet() : p(av_packet_alloc()) {}
packet::~packet() { av_packet_free(&p); }

packet::packet(const packet &o)
{
	p = av_packet_alloc();
	av_packet_ref(p, o.p);
}

packet &packet::operator=(const packet &o)
{
	av_packet_unref(p);
	av_packet_ref(p, o.p);
	return *this;
}

packet::packet(packet &&o)
{
	p = o.p;
	o.p = nullptr;
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

frame::frame() : f(av_frame_alloc()) {}
frame::~frame() { av_frame_free(&f); }

frame::frame(const frame &o)
{
	f = av_frame_alloc();
	av_frame_ref(f, o.f);
}

frame &frame::operator=(const frame &o)
{
	av_frame_unref(f);
	av_frame_ref(f, o.f);
	return *this;
}

frame::frame(frame &&o)
{
	f = o.f;
	o.f = nullptr;
}

frame &frame::operator=(frame &&o)
{
	if (f != o.f) {
		av_frame_unref(f);
		av_frame_move_ref(f, o.f);
	}
	return *this;
}

hw_frames::~hw_frames()
{
	av_buffer_unref(&ctx);
}

hw_frames::hw_frames(const hw_frames &o)
{
	if (o.ctx)
		ctx = av_buffer_ref(o.ctx);
	else
		ctx = nullptr;
}

hw_frames &hw_frames::operator=(const hw_frames &o)
{
	av_buffer_unref(&ctx);
	if (o.ctx)
		ctx = av_buffer_ref(o.ctx);
	else
		ctx = nullptr;
	return *this;
}

hw_frames::hw_frames(hw_frames &&o)
{
	ctx = o.ctx;
	o.ctx = nullptr;
}

hw_frames &hw_frames::operator=(hw_frames &&o)
{
	if (ctx != o.ctx) {
		av_buffer_unref(&ctx);
		ctx = o.ctx;
		o.ctx = nullptr;
	}
	return *this;
}

bool hw_frames::operator!()
{
	return (ctx == nullptr);
}

hw_device::hw_device(const std::string &name, const std::string &device)
{
	type = av_hwdevice_find_type_by_name(name.c_str());
	if (type == AV_HWDEVICE_TYPE_NONE) {
		std::cerr << "HW device " << name << " not supported"
			  << std::endl;
		ctx = nullptr;
	} else
		ctx = ffmpeg_hw_context(type, device);
}

hw_device::~hw_device()
{
	av_buffer_unref(&ctx);
}

hw_device::hw_device(const hw_device &o)
{
	if (o.ctx)
		ctx = av_buffer_ref(o.ctx);
	else
		ctx = nullptr;
	type = o.type;
}

hw_device &hw_device::operator=(const hw_device &o)
{
	av_buffer_unref(&ctx);
	if (o.ctx)
		ctx = av_buffer_ref(o.ctx);
	else
		ctx = nullptr;
	type = o.type;
	return *this;
}

hw_device::hw_device(hw_device &&o)
{
	ctx = o.ctx;
	type = o.type;

	o.ctx = nullptr;
}

hw_device &hw_device::operator=(hw_device &&o)
{
	if (ctx != o.ctx) {
		av_buffer_unref(&ctx);

		ctx = o.ctx;
		type = o.type;

		o.ctx = nullptr;
	}
	return *this;
}

bool hw_device::operator!() { return ctx == nullptr; }

hw_frames hw_device::get_hw_frames(AVPixelFormat sw_format,
				   int width, int height)
{
	hw_frames ret;

	ret.ctx = ffmpeg_hw_frames_ctx(ctx, sw_format, width, height);
	return ret;
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

bool decoder::operator<<(const packet &p)
{
	return send(p.p);
}

bool decoder::operator>>(frame &f)
{
	av_frame_unref(f.f);
	return receive(f.f);
}

hw_frames decoder::get_hw_frames()
{
	hw_frames ret;

	if (ctx->hw_frames_ctx)
		ret.ctx = av_buffer_ref(ctx->hw_frames_ctx);
	else
		ret.ctx = ffmpeg_hw_frames_ctx(ctx->hw_device_ctx,
					       AV_PIX_FMT_NV12,
					       ctx->width, ctx->height);

	return ret;
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

bool input::operator>>(packet &p)
{
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

decoder input::get(int index)
{
	return get(hw_device(), index, "", "");
}

decoder input::get(int index,
		   const std::string &codec_name, const std::string &options)
{
	return get(hw_device(), index, codec_name, options);
}

decoder input::get(const hw_device &device, int index)
{
	return get(device, index, "", "");
}

decoder input::get(const hw_device &device, int index,
		   const std::string &codec_name, const std::string &options)
{
	decoder dec;
	AVCodecParameters *par = nullptr;

	assert((unsigned int)index < ctx->nb_streams);

	par = ctx->streams[index]->codecpar;

	dec.ctx = ffmpeg_decoder_context(codec_name, par,
					 device.ctx, device.type, options);

	return dec;
}



int64_t input::start_time_realtime() const
{
	return ctx->start_time_realtime;
}

AVRational input::avg_frame_rate(int index) const
{
	assert((unsigned int)index < ctx->nb_streams);
	return ctx->streams[index]->avg_frame_rate;
}

AVRational input::time_base(int index) const
{
	assert((unsigned int)index < ctx->nb_streams);
	return ctx->streams[index]->time_base;
}

std::string input::metadata() const
{
	return dictionary_to_string(ctx->metadata);
}

std::string input::program_metadata(int index) const
{
	assert((unsigned int)index < ctx->nb_programs);

	return dictionary_to_string(ctx->programs[index]->metadata);
}

std::string input::stream_metadata(int index) const
{
	assert((unsigned int)index < ctx->nb_streams);

	return dictionary_to_string(ctx->streams[index]->metadata);
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

	if (!ctx->hw_frames_ctx) {
		f.f->format = ctx->pix_fmt;
		f.f->width  = ctx->width;
		f.f->height = ctx->height;

		av_frame_get_buffer(f.f, 32);
	} else
		av_hwframe_get_buffer(ctx->hw_frames_ctx, f.f, 0);

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
		close();

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
	return add_stream(hw_frames(), codec, options);
}

encoder output::add_stream(const hw_frames &frames, const std::string &codec,
			   const std::string &options)
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
		ctx->oformat->flags & AVFMT_GLOBALHEADER, frames.ctx);

	if (enc.ctx) {
		enc.stream_index = stream->id = ctx->nb_streams - 1;

		time_bases.resize(ctx->nb_streams);
		time_bases[stream->id] = enc.ctx->time_base;
	}
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

int output::write(AVPacket *packet, bool rescale)
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

	if (rescale) {
		int index = packet->stream_index;

		assert((unsigned int)index < ctx->nb_streams);

		av_packet_rescale_ts(packet, time_bases[index], ctx->streams[index]->time_base);
	}

	packet->pos = -1;
	return av_interleaved_write_frame(ctx, packet);
}

bool output::operator<<(const packet &p)
{
	return !(write(p.p) < 0);
}

bool output::write_norescale(const packet &p)
{
	return !(write(p.p, false) < 0);
}

void output::add_metadata(const std::string &data)
{
	av_dict_parse_string(&ctx->metadata, data.c_str(), "=", ":", 0);
}

void output::add_program_metadata(const std::string &data, int index)
{
	assert((unsigned int)index < ctx->nb_programs);

	av_dict_parse_string(&ctx->programs[index]->metadata, data.c_str(), "=", ":", 0);
}

void output::add_stream_metadata(const std::string &data, int index)
{
	assert((unsigned int)index < ctx->nb_streams);

	av_dict_parse_string(&ctx->streams[index]->metadata, data.c_str(), "=", ":", 0);
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
