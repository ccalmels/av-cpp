#include "ffmpeg.hpp"
#include "packet_queue.hpp"
#include <iostream>
#include <vector>

static void read_stream(packet_queue *q, av::decoder &&decoder)
{
	av::packet p;
	av::frame f;

	while (!q->is_closed()) {
		p = q->dequeue();

		decoder << p;

		while (decoder >> f) {
			std::cerr << "got frame " << f.f->pts
				  << " on stream: " << p.stream_index()
				  << std::endl;
		}
	}

	std::cerr << "queue is closed" << std::endl;
}

int main(int argc, char *argv[])
{
	av::input multi;
	std::vector<std::thread> decoders;
	std::vector<packet_queue *> queues;
	av::packet p;

	if (argc < 2) {
		std::cerr << "Usage: " << argv[0] << " <multi_stream_video>"
			  << std::endl;
		return -1;
	}

	if (!multi.open(argv[1]))
		return -1;

	while (multi >> p) {
		int index = p.stream_index();

		std::cerr << "got packet on stream: " << p.stream_index()
			  << std::endl;

		if (index >= (int)decoders.size()) {
			decoders.resize(index + 1);
			queues.resize(index + 1);
		}

		if (!queues[index]) {
			queues[index] = new packet_queue();
			decoders[index] = std::thread(
			    read_stream, queues[index], multi.get(index));
		}

		queues[index]->release(p);
	}

	std::cerr << "finished" << std::endl;

	for (auto p : queues)
		p->close();

	for (auto &d : decoders)
		d.join();

	return 0;
}
