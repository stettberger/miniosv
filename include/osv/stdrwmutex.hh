#pragma once

#include <osv/stdmutex.hh>
#include <lockfree/stack.hh>

struct std_rw_mutex_data {
    typedef char State;
    std::atomic<State> state{0};

    typedef uint32_t ReaderState;
    std::atomic<ReaderState> _readers;

    // inline_futex _std_mutex_queue; // 16 bytes
    // inline_futex::ref get_queue() { return inline_futex::ref(_std_mutex_queue, (uintptr_t)this); }
    inline_futex::ref get_queue() { return inline_futex::get(this); }

    std::atomic<sched::thread *> _write_waiter {nullptr};   // 8 bytes
    lockfree::stack<wait_record> _read_waiters;             // 8 bytes
};


template <class data_fields>
class std_rw_mutex_alg : public std_mutex_alg<data_fields> {
    using std_mutex = std_mutex_alg<data_fields>;

    using ReaderState = data_fields::ReaderState;

    using data_fields::_readers;
    using data_fields::_read_waiters;
    using data_fields::_write_waiter;

    static constexpr unsigned WRITER_LOCK          = 0x80000000;
    static constexpr unsigned PENDING_WRITER       = 0x40000000;
    static constexpr unsigned READERS_MASK         = 0x3fff0000;
    static constexpr unsigned IND_READ_MASK        = 0xffff0000;
    static constexpr unsigned PENDING_READERS_MASK = 0x0000ffff;
    static constexpr unsigned READER_LOCK_INC      = 0x00010000;

    static unsigned get_readers(ReaderState v) {
        return (v & READERS_MASK) >> 16;
    }
    static unsigned get_pending_readers(ReaderState v) {
        return (v & PENDING_READERS_MASK);
    }

public:
    struct Node : public std_mutex::Node {
        wait_record *pop_head;
    };

    constexpr std_rw_mutex_alg() noexcept = default;
    ~std_rw_mutex_alg() = default;

    std_rw_mutex_alg(const std_rw_mutex_alg&) = delete;
    std_rw_mutex_alg& operator=(const std_rw_mutex_alg&) = delete;


    void rlock(Node &) {
        //Try to acquire a lock for reading by adding READER_LOCK_INC to the _readers atomically
        //The loop will stop once a new writer enters wlock() past the _wmtx.lock()
        //and sets PENDING_WRITER, or writer has already locked it before, so
        //the PENDING_WRITER is already set.
        //It may race with wunlock() which removes PENDING_WRITER
        //The loop may also race with other threads calling rlock() or runlock()
        unsigned prev_readers = _readers.load(std::memory_order_acquire);
        assert((READERS_MASK & prev_readers) + READER_LOCK_INC < PENDING_WRITER);
        while (prev_readers < PENDING_WRITER) {
            if (_readers.compare_exchange_weak(prev_readers, prev_readers + READER_LOCK_INC, std::memory_order_acq_rel)) {
                return;
            }
            CPU_PAUSE();
        }

        //We stopped because the PENDING_WRITER or WRITER_LOCK was set so let us add ourselves to
        //the pending readers
        //This may race with wunlock() which removes WRITER_LOCK
        //1) if we win (1st) - wunlock() will see the latest number of pending readers including us
        //2) if we lose (2nd) - we will see both PENDING_WRITER and WRITER_LOCK off (the while condition true)
        assert((PENDING_READERS_MASK & prev_readers) + 1 < READER_LOCK_INC);
        prev_readers = _readers.fetch_add(1, std::memory_order_acq_rel) + 1;
        //We add 1 above, because this is the value we expect to modify below
        //Let us try in a loop again because maybe the 2nd scenario above is true
        while (prev_readers < PENDING_WRITER) {
            if (_readers.compare_exchange_weak(prev_readers, prev_readers + READER_LOCK_INC - 1, std::memory_order_acq_rel)) {
                return;
            }
        }

        //We have failed to acquire the lock for reading and bumped the pending readers count
        //Let us wait until wunlock() or downgrade() wakes us and bumps the _readers
        //by (READER_LOCK_INC - 1) on our behalf
        wait_record wr(sched::thread::current());
        _read_waiters.push(&wr);
        wr.wait();
    }

    void runlock(Node &) {
        //Release the lock for reading by subtracting READER_LOCK_INC atomically
        unsigned prev_readers = _readers.fetch_add(-READER_LOCK_INC, std::memory_order_acq_rel);

        assert(prev_readers > 0);
        assert(prev_readers < WRITER_LOCK);

        //Wake potential pending writer if any, if we are the last owning reader
        if ((prev_readers & READERS_MASK) == READER_LOCK_INC && (prev_readers & PENDING_WRITER)) {
            //Wake the _wmtx owner - pending writer - if not null
            auto waiting_writer = _write_waiter.exchange(0, std::memory_order_release);
            // wait_record * X = writers.pop_tagged();
            if (waiting_writer) {
                // if (X != nullptr) {
                //     if (X->thread() != waiting_writer) {
                //         printf("X: %p, %p %p\n", X, X->thread(), waiting_writer);
                //     }
                //     assert(X->thread() == waiting_writer);
                // }
                waiting_writer->wake();
            }
        }
    }

    void wlock(Node &node) {
        //Lock the writer mutex which may go to sleep
        sched::thread *me = sched::thread::current();
        std_mutex::wlock(node);

        //Fast Path for the uncontended path
        auto cur_readers = _readers.load(std::memory_order_relaxed);
        if (cur_readers == 0) {
            if (_readers.compare_exchange_weak(cur_readers, WRITER_LOCK,
                                               std::memory_order_acq_rel))
                return;
        }

        // Slow Path with Reader/Writer Contention
        _write_waiter.store(me, std::memory_order_release);

        //Lets set the write indicator in order to phase out the current readers and block new ones
        //from acquiring the lock for reading
        //This may race with the runlock() of the last reader
        ReaderState prev_readers = _readers.fetch_or(PENDING_WRITER, std::memory_order_acq_rel);
        cur_readers = prev_readers | PENDING_WRITER;

        //1) If we lost (the fetch above was 2nd), then the count of owning readers per
        //   prev_readers should be 0, and runlock() will not see PENDING_WRITER, and
        //   therefore the last reader will not try to wake us and change _writer_wait.
        //   The compare_exchange_weak() down below will acquire the lock for writing if
        //   successful.
        //2) If we won, then the count of owning readers per prev_readers will be equal to
        //   READER_LOCK_INC and runlock() will see PENDING_WRITER and should wake us
        //   and set _writer_wait to false.
        //   The while loop down below will not enter and proceed to wait_until()

        //Try to set WRITER_LOCK if no active readers
        //Stop looping if the lock owner by a reader
        while (get_readers(cur_readers) == 0) {
            if (_readers.compare_exchange_weak(cur_readers, WRITER_LOCK | (cur_readers & PENDING_READERS_MASK),
                                               std::memory_order_acq_rel)) {
                // we've won the race
                _write_waiter.store(nullptr, std::memory_order_relaxed);
                
                return;
            }
        }

        //Wait for last active reader to wake us
        // This is a waiter_record.wait()
        sched::thread::wait_until( [this] {
            return !this->_write_waiter.load(std::memory_order_acquire);
        });


        //Acquire the lock for writing by setting the WRITER_LOCK bit
        //We do it by adding PENDING_WRITER
        _readers.fetch_add(PENDING_WRITER, std::memory_order_acq_rel);
    }

    void wake_pending_readers(unsigned pending_readers) {
        //TODO:In order to minimize triggering 100s of IPI wakeups to other CPUs we may
        //use a new sched::thread::wake_many() method that would do similar logic to what
        //wake_impl() does for single thread - set status to waking for each thread, but
        //set need_reschedule or send IPI wake up once only for each relevant target CPU
        //
        //Wake pending readers one by one - stop when the count of the pending readers is 0
        //per the 16 least significant bits of _readers
        while (pending_readers) {
            wait_record *read_waiter = _read_waiters.pop();
            if (!read_waiter) {
                //Even though the _read_waiters is empty, re-read the count of pending readers
                //and keep trying until it reaches 0
                pending_readers = _readers.load(std::memory_order_acquire) & PENDING_READERS_MASK;
                continue;
            }
            //Acquire the lock for reading on behalf of this pending reader by adding (READER_LOCK_INC - 1)
            //and wake its thread
            pending_readers = _readers.fetch_add(READER_LOCK_INC - 1, std::memory_order_acq_rel) & PENDING_READERS_MASK; //lock - pending

            read_waiter->wake();
        }
    }

    void wunlock(Node &node) {
        assert(_readers & WRITER_LOCK);

        //If we are recursed then simply unlock and return
        // if (_wmtx.getdepth() > 1) {
        //     return _wmtx.unlock();
        // }

        //Allow pending readers to acquire a lock before new writer comes in
        //or the 1st from the pending one wakes from the the sleep
        unsigned pending_readers = _readers.fetch_and(~WRITER_LOCK, std::memory_order_acq_rel) & PENDING_READERS_MASK;

        //Wake the pending readers and acquire the lock for reading on their behalf
        wake_pending_readers(pending_readers);

        std_mutex::wunlock(node);
    }
};

class StdRwMutex : public std_rw_mutex_alg<std_rw_mutex_data> {};
