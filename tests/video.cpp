#include "ffmpeg.hpp"

#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#define NB_FRAMES 100

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
		av_frame_get_buffer(f, 0);
	}

	av_frame_make_writable(f);

	/* Y */
	for (y = 0; y < height; y++)
		for (x = 0; x < width; x++)
			f->data[0][y * f->linesize[0] + x] = x + y + index * 3;

	/* Cb and Cr */
	for (y = 0; y < height / 2; y++) {
		for (x = 0; x < width / 2; x++) {
			f->data[1][y * f->linesize[1] + x] =
			    128 + y + index * 2;
			f->data[2][y * f->linesize[2] + x] = 64 + x + index * 5;
		}
	}

	f->pts = index;
}

TEST_CASE("Encoding video using software encoder", "[encoding][software]")
{
	std::string encoder_name;
	av::output generated;
	av::encoder encode_video;
	av::frame f;
	av::packet p;

	SECTION("h264 encoding") { encoder_name = "libx264"; }
	SECTION("hevc encoding") { encoder_name = "libx265"; }

	REQUIRE(generated.open("/tmp/test." + encoder_name + ".mkv"));

	encode_video = generated.add_stream(
	    encoder_name,
	    "video_size=960x540:pixel_format=yuv420p:time_base=1/25");
	REQUIRE(!!encode_video);

	f = encode_video.get_empty_frame();

	for (int i = 0; i < NB_FRAMES; i++) {
		generate_frame(f.f, i, 960, 540);

		REQUIRE(encode_video << f);

		while (encode_video >> p)
			generated << p;
	}

	encode_video.flush();
	while (encode_video >> p)
		generated << p;
}

TEST_CASE("HW encoding using HW frames", "[encoding][hwaccel]")
{
	std::string encoder_name, hw_name;
	int width = 960, height = 540;
	AVPixelFormat hw_format;
	av::hw_device hw("cuda");
	if (!hw) {
		hw = av::hw_device("vaapi");
		if (!hw)
			return;

		hw_name = "vaapi";
		hw_format = AV_PIX_FMT_NV12;
	} else {
		hw_name = "nvenc";
		hw_format = AV_PIX_FMT_YUV420P;
	}

	av::hw_frames frames = hw.get_hw_frames(hw_format, width, height);
	REQUIRE(!!frames);

	SECTION("h264 encoding") { encoder_name = "h264_" + hw_name; }
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

	for (int i = 0; i < NB_FRAMES; i++) {
		generate_frame(f.f, i, width, height);

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

TEST_CASE("HW Decoding video", "[decoding][hwaccel]")
{
	av::hw_device hw_accel("cuda");
	if (!hw_accel)
		hw_accel = av::hw_device("vaapi");
	if (!hw_accel)
		return;

	std::string filename;
	av::input video;
	av::decoder stream0_decoder;
	av::frame f;
	av::packet p;
	int count = NB_FRAMES;

	SECTION("h264 decoding") { filename = "/tmp/test.libx264.mkv"; }
#if 0
	SECTION("hevc decoding") {
		filename = "/tmp/test.libx265.mkv";
	}
#endif
	REQUIRE(video.open(filename));

	stream0_decoder = video.get(hw_accel, 0);

	REQUIRE(!!stream0_decoder);

	while (video >> p) {
		if (p.stream_index() != 0)
			continue;

		REQUIRE(stream0_decoder << p);

		while (stream0_decoder >> f)
			count--;
	}

	stream0_decoder.flush();
	while (stream0_decoder >> f)
		count--;

	REQUIRE(count == 0);
}

TEST_CASE("Software Decoding video", "[decoding][software]")
{
	std::string filename;
	av::input video;
	av::decoder stream0_decoder;
	av::frame f;
	av::packet p;
	int count = NB_FRAMES;

	SECTION("h264 decoding") { filename = "/tmp/test.libx264.mkv"; }
	SECTION("hevc decoding") { filename = "/tmp/test.libx265.mkv"; }

	REQUIRE(video.open(filename));

	stream0_decoder = video.get(0);

	REQUIRE(!!stream0_decoder);

	while (video >> p) {
		if (p.stream_index() != 0)
			continue;

		REQUIRE(stream0_decoder << p);

		while (stream0_decoder >> f)
			count--;
	}

	stream0_decoder.flush();
	while (stream0_decoder >> f)
		count--;

	REQUIRE(count == 0);
}

TEST_CASE("Metadata handling", "[metadata]")
{
	std::string metadata = "service_name=foo:service_provider=bar";
	std::string encoder_name;
	av::output generated;
	av::encoder encode_video;
	av::frame f;
	av::packet p;

	encoder_name = "libx264";

	REQUIRE(generated.open("/tmp/metadata_test." + encoder_name + ".ts"));

	generated.add_metadata(metadata);

	encode_video = generated.add_stream(
	    encoder_name,
	    "video_size=960x540:pixel_format=yuv420p:time_base=1/25");
	REQUIRE(!!encode_video);

	f = encode_video.get_empty_frame();

	for (int i = 0; i < NB_FRAMES; i++) {
		generate_frame(f.f, i, 960, 540);

		REQUIRE(encode_video << f);

		while (encode_video >> p)
			generated << p;
	}

	encode_video.flush();
	while (encode_video >> p)
		generated << p;

	av::input video;

	REQUIRE(video.open("/tmp/metadata_test." + encoder_name + ".ts"));

	REQUIRE(metadata.compare(video.program_metadata(0)) == 0);
}
