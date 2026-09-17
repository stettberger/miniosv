#include <mutex>
#include <shared_mutex>
#include <random>
#include <atomic>
#include <vector>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <sched.h>
#include <cassert>
#include "osv/spinlocks.hh"

#include "lockbench/posix-common.hh"
#include "lockbench/common.h"
#include "lockbench/lockbench.hh"

class PosixMutex : public lock_interface {
    pthread_mutex_t the_lock;
public:
    PosixMutex() {
        pthread_mutex_init(&the_lock, NULL);
    }
    void wlock(Node &) { pthread_mutex_lock(&the_lock); }
    void wunlock(Node &) { pthread_mutex_unlock(&the_lock); }

};

class StdMutex : public lock_interface {
    std::mutex mtx;
public:
    void wlock(Node &) { mtx.lock(); }
    void wunlock(Node &) { mtx.unlock(); }
};

class StdRwMutex : public lock_interface {
    std::shared_mutex mtx;
public:
    void wlock(Node &) { mtx.lock(); }
    void wunlock(Node &) { mtx.unlock(); }

    void rlock(Node &) { mtx.lock_shared(); }
    void runlock(Node &) { mtx.unlock_shared(); }

};


int main() {
    Table table;

    int seconds = 2;

    std::vector<double> writer_chance = {0, 0.001, 0.01, 0.1, 1, 10, 20, 50, 99};
    std::vector<double> reader_threads = {1,2,4,8,16,32};

    for (double wc : writer_chance) {
        for (int readers : reader_threads) {
            run_lockbench<StdRwMutex>  (table, readers, 0, seconds, 0, wc);
        }
    }

    std::vector<int> thread_counts = {1, 2, 4, 8, 16, 24, 32};
    std::vector<int> all_writers_short = {1, 2, 3, 4, 5, 6, 8, 16, 24, 32};
    std::vector<int> wait_percentages_short = {0, 1, 10, 50, 100};

    int readers = 0;
    for (int writers : all_writers_short) {
        for (int wait : wait_percentages_short) {

            run_lockbench<StdMutex>(table, 0, writers, seconds, wait);
            run_lockbench<StdRwMutex>(table, 0, writers, seconds, wait);

            run_lockbench<PosixMutex>(table, 0, writers, seconds, wait);

            run_lockbench<TATAS<BackoffNone>>(table, readers, writers, seconds, wait);

            using BackoffE = BackoffExponential<>;
            run_lockbench<TATAS<BackoffE>>(table, readers, writers, seconds, wait);

            using BackoffA = BackoffAutoTune<>;
            run_lockbench<TATAS<BackoffA>>(table, readers, writers, seconds, wait);

            run_lockbench<TicketLock>(table, readers, writers, seconds, wait);
            run_lockbench<MCSLock>(table, readers, writers, seconds, wait);
        }
    }

    return 0;
}
