#include <list>
#include <vector>
#include <iostream>
#include "ffmpeg.hpp"

/*
 * code borrow from ffmpeg documentation/example
 */
static void generate_frame(AVFrame *f, int index, int width, int height)
{
	int x, y;

	av_frame_make_writable(f);

        /* Y */
        for (y = 0; y < height; y++)
                for (x = 0; x < width; x++)
                        f->data[0][y * f->linesize[0] + x] = x + y + index * 3;

        /* Cb and Cr */
        for (y = 0; y < height / 2; y++) {
                for (x = 0; x < width / 2; x++) {
                        f->data[1][y * f->linesize[1] + x] = 128 + y + index * 2;
                        f->data[2][y * f->linesize[2] + x] = 64 + x + index * 5;
                }
        }

	f->pts = index;
}

static bool generate_video(const std::string &name,
			   const std::string &encoder_name)
{
	av::output generated;
	av::encoder encode_video;
	av::frame f;
	av::packet p;

	if (!generated.open(name)) {
		std::cerr << "Can't open " << name << std::endl;
		return false;
	}

	encode_video = generated.add_stream(
		encoder_name,
		"video_size=960x540:pixel_format=yuv420p:time_base=1/25");
	if (!encode_video) {
		std::cerr << "Can't create encoder for " << name << std::endl;
		return false;
	}

	f = encode_video.get_empty_frame();

	for (int i = 0; i < 100; i++) {
		generate_frame(f.f, i, 960, 540);

		if (!(encode_video << f)) {
			std::cerr << "Encoding frame fails" << std::endl;
			return false;
		}

		while (encode_video >> p)
			generated << p;
	}

	return true;
}

static av::hw_device no_hw_accel;
static bool decode_video(const std::string &name,
			 av::hw_device &device = no_hw_accel)
{
	av::input video;
	av::decoder stream0_decoder;
	av::frame f;
	av::packet p;

	if (!video.open(name)) {
		std::cerr << "Can't open " << name << std::endl;
		return false;
	}

	stream0_decoder = video.get(device, 0);
	if (!stream0_decoder) {
		std::cerr << "Can't create decoder for " << name << std::endl;
		return false;
	}

	while (video >> p) {
		if (p.stream_index() != 0)
			continue;

		if (!(stream0_decoder << p)) {
			std::cerr << "Decoding frame fails" << std::endl;
			return false;
		}

		while (stream0_decoder >> f) {
			std::cerr << "got frame: " << f.f->pts << std::endl;
		}
	}

	return true;
}

int main(int argc, char *argv[])
{
	if (!generate_video("/tmp/test.mkv", "libx264"))
		return -1;

	if (!decode_video("/tmp/test.mkv"))
		return -1;

	av::hw_device hw_accel("cuda");

	if (!hw_accel)
		hw_accel = av::hw_device("vaapi");

	if (!decode_video("/tmp/test.mkv", hw_accel))
		return -1;

	return 0;
}
