#include "ffmpeg.hpp"

int main(int argc, char *argv[])
{
	av::input pcm;
	av::output mp3;

	if (!pcm.open_format(argv[1], "s16le", "sample_rate=32000"))
		return -1;

	if (!mp3.open(argv[2]))
		return -1;

	av::decoder pcm_decoder = pcm.get(0);
	if (!pcm_decoder)
		return -1;

	av::encoder mp3_encoder = mp3.add_stream(
		"libmp3lame",
		"time_base=1/32000:ar=32000:ac=1:request_sample_fmt=s16");
	if (!mp3_encoder)
		return -1;

	av::packet p, p2;
	av::frame f;

	while (pcm >> p) {
		if (!(pcm_decoder << p))
			return -1;

		while (pcm_decoder >> f) {
			if (!(mp3_encoder << f))
				return -1;

			while (mp3_encoder >> p2)
				mp3 << p2;
		}
	}
	return 0;
}
