#pragma once

#include <iostream>
#include <iomanip>
#include <vector>
#include <set>
#include <sstream>
#include <fstream>



#define die(fmt) do { perror(fmt); exit(EXIT_FAILURE); } while(0)
#define ARRAY_SIZE(arr) (sizeof(arr)/sizeof(*arr))


class Table {
	std::vector<std::string> names;
	std::vector<std::string> values;
	std::vector<unsigned> widths;
	bool printed_header = false;
	std::set<std::string> fields;

	void dumpVector(const std::vector<std::string> &v) {
		for (unsigned i = 0; i < v.size(); i++) {
			if (i != 0) {
				if (fields.size() > 0 &&
					fields.find(names[i]) == fields.end())
					continue;
				std::cout << '\t';
			}

			
			std::cout << std::left << std::setw(widths[i]) << v[i];
		}
		std::cout << std::endl;
	}

public:
	Table() {
		char *f = getenv("FIELDS");
		if (f) {
			std::stringstream ss(f);
			std::string item;
			while (std::getline(ss, item, ' ')) {
				fields.insert(item);
			}
		}
	}

	template<typename T>
	Table & col (const std::string &header, T value) {
		if (!printed_header)
			names.push_back(header);
		std::stringstream ss;
		ss << value;
		values.push_back(ss.str());
		return *this;
	}

	void dump(const std::string & prefix="") {
		if (!printed_header)
            widths.resize(names.size());
        for (unsigned i = 0; i < names.size(); i++) {
            widths[i] = std::max(widths[i], (unsigned)names[i].length());
            widths[i] = std::max(widths[i], (unsigned)values[i].length());
        }
        if (!printed_header) {
            std::cout << prefix;
            dumpVector(names);
			printed_header = true;
		}
        std::cout << prefix;
		dumpVector(values);
		values.clear();
	}
};


static inline long long unsigned time_ns(struct timespec* const ts) {
    if (clock_gettime(CLOCK_REALTIME, ts)) {
        exit(1);
    }
    return ((long long unsigned) ts->tv_sec) * (long long unsigned) 1e9
        + (long long unsigned) ts->tv_nsec;
}

static inline double time_delta(struct timespec* const ts, int clock_id=CLOCK_REALTIME) {
	struct timespec N;
	if (clock_gettime(clock_id, &N)) {
		exit(1);
	}
	double ret = ((double) N.tv_sec - ts->tv_sec) + (double) (N.tv_nsec - ts->tv_nsec) /1e9;
	*ts = N;
	return ret;
}

class cpu_ns_avg {
    std::atomic<uint16_t> wake_count = 0;
    std::atomic<uint64_t> wake_sum = 0;
    std::atomic<uint64_t> wake_max = 0;

    uint64_t cpu_time() {
#ifndef POSIX
        return clock::get()->time();
#else
        timespec ts;
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + ts.tv_nsec;
#endif
    }

public:
    uint64_t begin() {
        return cpu_time();
    }
    
    void end(const std::string &hd, uint64_t begin) {
        uint64_t end = cpu_time();
        uint64_t delta = end - begin;
        wake_sum += delta;
        // if (delta > wake_max)
        //     wake_max = delta;
        if (wake_count++ == 0) {
            unsigned nano = wake_sum.exchange(0);
            double per_op = nano/((double)(1 << 16));
            std ::cout << hd << ": avg=" << per_op
                       << ", max=" << wake_max
                       << std::endl;
        }
    }
};



#ifdef POSIX

#include <boost/algorithm/string.hpp>
unsigned long long
readTLBShootdownCount(void) {
    std::ifstream irq_stats("/proc/interrupts");
    assert (!!irq_stats);

    for (std::string line; std::getline(irq_stats, line); ) {
        if (line.find("TLB") != std::string::npos) {
            std::vector<std::string> strs;
            boost::split(strs, line, boost::is_any_of("\t "));
            unsigned long long count = 0;
            for (size_t i = 0; i < strs.size(); i++) {
                std::set<std::string> bad_strs = {"", "TLB", "TLB:", "shootdowns"};
                if (bad_strs.find(strs[i]) != bad_strs.end())
                    continue;
                std::stringstream ss(strs[i]);
                unsigned long long c;
                ss >> c;
                count += c;
            }
            return count;
        }
    }
    return 0;
}

#endif
