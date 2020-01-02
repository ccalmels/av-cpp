#include <list>
#include <vector>
#include <iostream>
#include "ffmpeg.hpp"

#define CATCH_CONFIG_MAIN
#include "catch.hpp"

/*
 * code borrow from ffmpeg documentation/example
 */
static void generate_frame(AVFrame *f, int index, int width, int height)
{
	int x, y;

	if (!av_frame_is_writable(f)) {
		f->width = width;
		f->height = height;
		f->format = AV_PIX_FMT_YUV420P;
		av_frame_get_buffer(f, 32);
	}

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

	encode_video.flush();
	while (encode_video >> p)
		generated << p;

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

	int count = 0;
	while (video >> p) {
		if (p.stream_index() != 0)
			continue;

		if (!(stream0_decoder << p)) {
			std::cerr << "Decoding frame fails" << std::endl;
			return false;
		}

		while (stream0_decoder >> f) {
			std::cerr << "got frame: " << f.f->pts << std::endl;
			count++;
		}
	}

	stream0_decoder.flush();
	while (stream0_decoder >> f) {
		std::cerr << "got frame: " << f.f->pts << std::endl;
		count++;
	}

	return (count == 100);
}

TEST_CASE("Encoding video using software encoder", "[encoding]")
{
	REQUIRE(generate_video("/tmp/test.x264.mkv", "libx264") == true);
	REQUIRE(generate_video("/tmp/test.x265.mkv", "libx265") == true);
}

TEST_CASE("Decoding video", "[decoding]")
{
	REQUIRE(decode_video("/tmp/test.x264.mkv") == true);
	REQUIRE(decode_video("/tmp/test.x265.mkv") == true);
}

TEST_CASE("Decoding video using HW", "[decoding][hwaccel]")
{
	av::hw_device hw_accel("cuda");
	if (!hw_accel)
		hw_accel = av::hw_device("vaapi");

	if (!hw_accel)
		return;

	REQUIRE(decode_video("/tmp/test.x264.mkv", hw_accel) == true);
}

TEST_CASE("HW encoding using HW frames", "[encoding][hwaccel]")
{
	std::string encoder_name, hw_name;
	av::hw_frames frames;
	av::hw_device hw("cuda");
	if (!hw) {
		hw = av::hw_device("vaapi");
		if (!hw)
			return;

		hw_name = "vaapi";
		frames = hw.get_hw_frames(AV_PIX_FMT_NV12, 960, 540);
	} else {
		hw_name = "nvenc";
		frames = hw.get_hw_frames(AV_PIX_FMT_YUV420P, 960, 540);
	}
	REQUIRE(!!frames);

	SECTION("h264 encoding") {
		encoder_name = "h264_" + hw_name;
	}
	SECTION("hevc encoding") {
		encoder_name = "hevc_" + hw_name;
	}

	av::output video;

	REQUIRE(video.open("/tmp/test." + encoder_name + ".hw.mp4"));

	av::encoder encoder;
	encoder = video.add_stream(frames, encoder_name, "time_base=1/25");

	REQUIRE(!!encoder);

	av::packet p;
	av::frame f, hw_f;

	hw_f = encoder.get_empty_frame();

	for (int i = 0; i < 100; i++) {
		generate_frame(f.f, i, 960, 540);

		REQUIRE(av_hwframe_transfer_data(hw_f.f, f.f, 0) >= 0);

		hw_f.f->pts = f.f->pts;
		REQUIRE(encoder << hw_f);

		while (encoder >> p)
			video << p;
	}

	encoder.flush();
	while (encoder >> p)
		video << p;
}
