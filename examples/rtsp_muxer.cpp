#include <atomic>
#include <condition_variable>
#include <iostream>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ffmpeg.hpp"
#include "packet_queue.hpp"

static void read_stream_delta(av::input &in, packet_queue &q, int stream_index,
			      int64_t delta)
{
	std::cerr << "got delta: " << delta << " on stream " << stream_index
		  << std::endl;

	while (!q.is_closed()) {
		av::packet p = q.acquire();

		if (!(in >> p))
			return;

		p.add_delta_pts(delta);
		p.stream_index(stream_index);

		q.release(p);
	}
}

static void read_stream(av::input &in, packet_queue &q, int stream_index)
{
	static std::atomic<int64_t> t0(AV_NOPTS_VALUE);
	av::packet p;

	while (in >> p) {
		int64_t realtime;

		realtime = in.start_time_realtime();

		if (realtime == AV_NOPTS_VALUE)
			continue;

		if (stream_index == 0) {
			t0 = realtime;
			return read_stream_delta(in, q, 0, 0);
		}
		if (t0 == AV_NOPTS_VALUE)
			continue;

		// now calculate delta
		int64_t delta;
		AVRational time_base;

		time_base = in.time_base(0);
		delta = av_rescale(realtime - t0, time_base.den,
				   time_base.num * 1000000);

		return read_stream_delta(in, q, stream_index, delta);
	}
}

int main(int argc, char *argv[])
{
	if (argc < 3) {
		std::cerr << "Usage: " << argv[0] << " <output> <rtsp_uri>..."
			  << std::endl;
		return -1;
	}

	av::output output;
	std::vector<av::input> inputs(argc - 2);
	std::vector<std::thread> reads(argc - 2);
	packet_queue q;

	if (!output.open(argv[1])) {
		std::cerr << "Can't open output " << argv[1] << std::endl;
		return -1;
	}

	for (int i = 0; i < argc - 2; i++) {
		if (!inputs[i].open(argv[i + 2], "rtsp_transport=tcp")) {
			std::cerr << "Can't open input " << argv[i + 2]
				  << std::endl;
			return -1;
		}

		int res = output.add_stream(inputs[i], 0);
		if (res != i) {
			std::cerr << "Can't add stream " << i << ":" << res
				  << std::endl;
			return -1;
		}
	}

	for (size_t i = 0; i < reads.size(); i++)
		reads[i] = std::thread(read_stream, std::ref(inputs[i]),
				       std::ref(q), i);

	while (1) {
		av::packet p = q.dequeue();

		if (!(output << p))
			break;

		q.enqueue(p);
	}

	q.close();

	for (size_t i = 0; i < reads.size(); i++)
		reads[i].join();

	return 0;
}
