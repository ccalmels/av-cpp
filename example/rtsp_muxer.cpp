#include <string>
#include <vector>
#include <list>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <iostream>

#include "ffmpeg.hpp"

struct packet_queue {
	av::packet acquire() {
		std::lock_guard<std::mutex> l(m);

		if (!empty_packets.empty()) {
			av::packet p = empty_packets.back();
			empty_packets.pop_back();
			return p;
		}

		return av::packet();
	}

	void release(av::packet &p) {
		{
			std::lock_guard<std::mutex> l(m);

			filled_packets.push_back(p);
		}
		cv.notify_one();
	}

	av::packet dequeue() {
		std::unique_lock<std::mutex> l(m);
		av::packet p;

		while (filled_packets.empty())
			cv.wait(l);

		p = filled_packets.front();
		filled_packets.pop_front();

		l.unlock();

		return p;
	}

	void enqueue(av::packet p) {
		std::lock_guard<std::mutex> l(m);

		empty_packets.push_back(p);
	}


	std::mutex m;
	std::condition_variable cv;
	std::list<av::packet> filled_packets;
	std::list<av::packet> empty_packets;
};

static void read_stream_delta(av::input &in, packet_queue &q, int stream_index,
			      int64_t delta)
{
        std::cerr << "got delta: " << delta << " on stream "
		  << stream_index << std::endl;

	while (1) {
		av::packet p = q.acquire();

		if (!(in >> p))
			return;

		p.p->pts += delta;
		p.p->dts += delta;
		p.p->stream_index = stream_index;

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

	for (size_t i = 0; i < reads.size(); i++)
                reads[i].join();

	return 0;
}
