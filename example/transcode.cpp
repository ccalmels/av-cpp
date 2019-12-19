#include <iostream>
#include "ffmpeg.hpp"

int main(int argc, char *argv[])
{
	av::input in;
	av::output out;
	av::decoder dec;
	av::encoder enc;
	av::hw_device accel("cuda");
	int count = 0;

	if (argc < 3) {
		std::cerr << "Usage: " << argv[0]
			  << " <input> <output>"
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
			static bool encoder_initialized;
			if (!encoder_initialized) {
				encoder_initialized = true;

				enc = out.add_stream(dec.get_hw_frames(), "hevc_nvenc",
						     "time_base=1/25:preset=slow:spatial_aq=true:aq-strength=15:qp=36");
			}

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
