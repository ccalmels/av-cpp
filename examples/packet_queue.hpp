#include <list>
#include <thread>
#include <mutex>
#include <condition_variable>
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

		while (filled_packets.empty() && !closed)
			cv.wait(l);

		if (!filled_packets.empty()) {
			p = filled_packets.front();
			filled_packets.pop_front();
		}

		l.unlock();

		return p;
	}

	void enqueue(av::packet p) {
		std::lock_guard<std::mutex> l(m);

		empty_packets.push_back(p);
	}

	bool is_closed() {
		std::lock_guard<std::mutex> l(m);

		return closed && filled_packets.empty();
	}

	void close(bool immediately = false) {
		{
			std::lock_guard<std::mutex> l(m);

			closed = true;

			if (immediately)
				filled_packets.clear();
		}
		cv.notify_all();
	}

	bool closed;
	std::mutex m;
	std::condition_variable cv;
	std::list<av::packet> filled_packets;
	std::list<av::packet> empty_packets;
};

