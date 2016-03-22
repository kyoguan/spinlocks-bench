#ifndef EXCL_LOCKS_HPP
#define EXCL_LOCKS_HPP

#include <cassert>
#include <vector>
#include <atomic>
#include <mutex>

#include "os.hpp"

class Mutex
{
public:
    ALWAYS_INLINE void Enter()
    {
        Mtx.lock();
    }

    ALWAYS_INLINE void Leave()
    {
        Mtx.unlock();
    }

private:
    std::mutex Mtx;
};

#if (OS == UNIX)

#include <pthread.h>

class SpinLockPThread
{
public:
    ALWAYS_INLINE SpinLockPThread()
    {
        pthread_spin_init(&Lock, 0);
    }

    ALWAYS_INLINE void Enter()
    {
        pthread_spin_lock(&Lock);
    }

    ALWAYS_INLINE void Leave()
    {
        pthread_spin_unlock(&Lock);
    }

private:
    pthread_spinlock_t Lock;
};

#elif (OS == WIN)

class LockCriticalSection
{
public:
    ALWAYS_INLINE LockCriticalSection()
    {
        InitializeCriticalSection(&Cs);
    }

    ALWAYS_INLINE void Enter()
    {
        EnterCriticalSection(&Cs);
    }

    ALWAYS_INLINE void Leave()
    {
        LeaveCriticalSection(&Cs);
    }

private:
    CRITICAL_SECTION Cs;
};

#endif

class ScTasSpinLock
{
public:
    ALWAYS_INLINE void Enter()
    {
        while (Locked.exchange(true));
    }

    ALWAYS_INLINE void Leave()
    {
        Locked.store(false);
    }

private:
    std::atomic_bool Locked = {false};
};

class TasSpinLock
{
public:
    ALWAYS_INLINE void Enter()
    {
        while (Locked.exchange(true, std::memory_order_acquire));
    }

    ALWAYS_INLINE void Leave()
    {
        Locked.store(false, std::memory_order_release);
    }

private:
    std::atomic_bool Locked = {false};
};

class TTasSpinLock
{
public:
    ALWAYS_INLINE void Enter()
    {
        do
        {
            while (Locked.load(std::memory_order_relaxed));
        }
        while (Locked.exchange(true, std::memory_order_acquire));
    }

    ALWAYS_INLINE void Leave()
    {
        Locked.store(false, std::memory_order_release);
    }

private:
    std::atomic_bool Locked = {false};
};

class RelaxTTasSpinLock
{
public:
    ALWAYS_INLINE void Enter()
    {
        do
        {
            while (Locked.load(std::memory_order_relaxed))
                CpuRelax();
        }
        while (Locked.exchange(true, std::memory_order_acquire));
    }

    ALWAYS_INLINE void Leave()
    {
        Locked.store(false, std::memory_order_release);
    }

private:
    std::atomic_bool Locked = {false};
};

class ExpBoRelaxTTasSpinLock
{
public:
    ALWAYS_INLINE void Enter()
    {
        size_t curMaxDelay = MIN_BACKOFF_ITERS;

        while (true)
        {
            WaitUntilLockIsFree();

            if (Locked.exchange(true, std::memory_order_acquire))
                BackoffExp(curMaxDelay);
            else
                break;
        }
    }

    ALWAYS_INLINE void Leave()
    {
        Locked.store(false, std::memory_order_release);
    }

private:
    ALWAYS_INLINE void WaitUntilLockIsFree() const
    {
        size_t numIters = 0;

        while (Locked.load(std::memory_order_relaxed))
        {
            if (numIters < MAX_WAIT_ITERS)
            {
                numIters++;
                CpuRelax();
            }
            else
                YieldSleep();
        }
    }

public:
    std::atomic_bool Locked = {false};

private:
    static const size_t MAX_WAIT_ITERS = 0x10000;
    static const size_t MIN_BACKOFF_ITERS = 32;
};

class TicketSpinLock
{
public:
    ALWAYS_INLINE void Enter()
    {
        const auto myTicketNo = NextTicketNo.fetch_add(1, std::memory_order_relaxed);

        while (ServingTicketNo.load(std::memory_order_acquire) != myTicketNo)
            CpuRelax();
    }

    ALWAYS_INLINE void Leave()
    {
        // We can get around a more expensive read-modify-write operation
        // (std::atomic_size_t::fetch_add()), because noone can modify
        // ServingTicketNo while we're in the critical section.
        const auto newNo = ServingTicketNo.load(std::memory_order_relaxed)+1;
        ServingTicketNo.store(newNo, std::memory_order_release);
    }

private:
    alignas(CACHELINE_SIZE) std::atomic_size_t ServingTicketNo = {0};
    alignas(CACHELINE_SIZE) std::atomic_size_t NextTicketNo = {0};
};

static_assert(sizeof(TicketSpinLock) == 2*CACHELINE_SIZE, "");

class PropBoTicketSpinLock
{
public:
    ALWAYS_INLINE void Enter()
    {
        constexpr size_t BACKOFF_BASE = 10;
        const auto myTicketNo = NextTicketNo.fetch_add(1, std::memory_order_relaxed);

        while (true)
        {
            const auto servingTicketNo = ServingTicketNo.load(std::memory_order_acquire);
            if (servingTicketNo == myTicketNo)
                break;

            const size_t waitIters = BACKOFF_BASE*(myTicketNo-servingTicketNo);

            for (size_t i=0; i<waitIters; i++)
                CpuRelax();
        }
    }

    ALWAYS_INLINE void Leave()
    {
        const auto newNo = ServingTicketNo.load(std::memory_order_relaxed)+1;
        ServingTicketNo.store(newNo, std::memory_order_release);
    }

private:
    alignas(CACHELINE_SIZE) std::atomic_size_t ServingTicketNo = {0};
    alignas(CACHELINE_SIZE) std::atomic_size_t NextTicketNo = {0};
};

static_assert(sizeof(PropBoTicketSpinLock) == 2*CACHELINE_SIZE, "");

class McsLock
{
public:
    struct QNode
    {
        std::atomic<QNode *> Next = {nullptr};
        std::atomic_bool     Locked = {false};
    };

public:
    ALWAYS_INLINE void Enter(QNode &node)
    {
        node.Next = nullptr;
        node.Locked = true;

        QNode *oldTail = Tail.exchange(&node);

        if (oldTail != nullptr)
        {
            oldTail->Next = &node;

            while (node.Locked == true)
                CpuRelax();
        }
    }

    ALWAYS_INLINE void Leave(QNode &node)
    {
        if (node.Next.load() == nullptr)
        {
            QNode *tailWasMe = &node;
            if (Tail.compare_exchange_strong(tailWasMe, nullptr))
                return;
            
            while (node.Next.load() == nullptr)
                CpuRelax();
        }

        node.Next.load()->Locked = false;
    }

private:
    std::atomic<QNode *> Tail = {nullptr};
};

#endif
