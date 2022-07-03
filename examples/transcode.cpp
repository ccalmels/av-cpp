#include "ffmpeg.hpp"
#include <iostream>

int main(int argc, char *argv[])
{
	av::input in;
	av::output out;
	av::decoder dec;
	av::encoder enc;
	av::hw_device accel("cuda");
	std::string encoder = "hevc_nvenc";
	int count = 0;

	if (!accel) {
		accel = av::hw_device("vaapi");
		encoder = "h264_vaapi";
	}

	if (argc < 3) {
		std::cerr << "Usage: " << argv[0] << " <input> <output>"
			  << std::endl;
		return -1;
	}

	if (!accel)
		return -1;

	if (!in.open(argv[1]))
		return -1;
#if 1
	dec = in.get(accel, 0);
#else
	dec = in.get(accel, 0, "h264_cuvid", "resize=1920x1080");
#endif
	if (!dec)
		return -1;

	if (!out.open(argv[2]))
		return -1;

	av::packet p;
	av::frame f;

	while (in >> p) {
		if (p.stream_index() != 0)
			continue;

		dec << p;

		while (dec >> f) {
			if (!enc)
				enc = out.add_stream(
				    dec.get_hw_frames(), encoder,
				    "time_base=" + av::to_string(av_inv_q(
						       in.frame_rate(0))));

			f.f->pts = count++;
			enc << f;

			while (enc >> p)
				out << p;
		}
	}

	dec.flush();
	while (dec >> f) {
		f.f->pts = count++;
		enc << f;

		while (enc >> p)
			out << p;
	}

	enc.flush();
	while (enc >> p)
		out << p;

	return 0;
}
