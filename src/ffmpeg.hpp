#pragma once
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

namespace av {

class packet {
public:
	packet();
	~packet();

	packet(const packet &o);
	packet &operator=(const packet &o);

	packet(packet &&o);
	packet &operator=(packet &&o);

	int stream_index() const;
	void stream_index(int index);

	void add_delta_pts(int64_t delta);

	friend class input;
	friend class output;
	friend class encoder;
	friend class decoder;
private:
	AVPacket *p;
};

struct frame {
	frame();
	~frame();

	frame(const frame &o);
	frame &operator=(const frame &o);

	frame(frame &&o);
	frame &operator=(frame &&o);

	bool is_hardware() const;
	frame transfer(AVPixelFormat hint = AV_PIX_FMT_NONE) const;

	AVFrame *f;
};

class hw_frames {
public:
	hw_frames() : ctx(nullptr) {}
	~hw_frames();

	hw_frames(const hw_frames &o);
	hw_frames &operator=(const hw_frames &o);

	hw_frames(hw_frames &&o);
	hw_frames &operator=(hw_frames &&o);

	bool operator!();

	friend class hw_device;
	friend class output;
	friend class decoder;
private:
	AVBufferRef *ctx;
};

class hw_device {
public:
	hw_device() : ctx(nullptr) {}
	hw_device(const std::string &name, const std::string &device = "");
	~hw_device();

	hw_device(const hw_device &o);
	hw_device &operator=(const hw_device &o);

	hw_device(hw_device &&o);
	hw_device &operator=(hw_device &&o);

	bool operator!();

	hw_frames get_hw_frames(AVPixelFormat sw_format, int width, int height);

	friend class input;
private:
	AVBufferRef *ctx;
	enum AVHWDeviceType type;
};

class codec {
public:
	codec();
	~codec();

	codec(codec &&o);
	codec &operator=(codec &&o);

	bool operator!();
protected:
	AVCodecContext *ctx;
private:
	codec(const codec &) = delete;
	codec &operator=(const codec &) = delete;

	void drop();
};

class decoder : public codec {
public:
	bool send(const AVPacket *packet);
	bool flush();
	bool receive(AVFrame *frame);

	bool operator<<(const packet &p);
	bool operator>>(frame &f);

	hw_frames get_hw_frames();

	friend class input;
};

class input {
public:
	input() : ctx(nullptr) {}
	~input() { close(); }

	input(input &&o);
	input &operator=(input &&o);

	bool open(const std::string &uri, const std::string &options = "");
	bool open_format(const std::string &uri, const std::string &format,
			 const std::string &options = "");

	int read(AVPacket *packet);
	bool operator>>(packet &p);

	int get_video_index(int id) const;
	int get_audio_index(int id) const;

	decoder get(int index);
	decoder get(int index, const std::string &codec_name,
		    const std::string &options = "");
	decoder get(const hw_device &device, int index);
	decoder get(const hw_device &device, int index,
		    const std::string &codec_name,
		    const std::string &options = "");

	int64_t start_time_realtime() const;
	AVRational frame_rate(int index) const;
	AVRational time_base(int index) const;

	std::string metadata() const;
	std::string program_metadata(int index) const;
	std::string stream_metadata(int index) const;

	friend class output;
private:
	input(const input &) = delete;
	input &operator=(const input &) = delete;

	void close();

	AVFormatContext *ctx;
};

class encoder : public codec {
public:
	bool send(const AVFrame *frame);
	bool flush();
	bool receive(AVPacket *packet);

	bool operator<<(const frame &f);
	bool operator>>(packet &p);

	frame get_empty_frame();

	friend class output;
private:
	int stream_index;
};

class output {
public:
	output(): ctx(nullptr), write_header(false), write_trailer(false) {}
	~output() { close(); }

	output(output &&o);
	output &operator=(output &&o);

	bool open(const std::string &uri);

	encoder add_stream(const std::string &codec,
			   const std::string &options = "");
	encoder add_stream(const hw_frames &frames,
			   const std::string &codec,
			   const std::string &options = "");
	int add_stream(const input &in, int index);

	int write(AVPacket *packet, bool rescale = true);
	bool operator<<(const packet &p);
	bool write_norescale(const packet &p);

	void add_metadata(const std::string &data);
	void add_program_metadata(const std::string &data, int index);
	void add_stream_metadata(const std::string &data, int index);
private:
	output(const output &) = delete;
	output &operator=(const output &) = delete;

	void close();

	AVFormatContext *ctx;
	bool write_header, write_trailer;
	std::vector<AVRational> time_bases;
};

}
