#pragma once
// win_mutex.h — Drop-in replacement for std::mutex using Windows SRWLOCK.
//
// SRWLOCK is zero-initialized at compile time (SRWLOCK_INIT = {0}) and
// requires no dynamic construction.  This avoids a crash in Wine's
// MSVCP140.dll!_Mtx_lock when std::mutex objects declared at file/namespace
// scope have not been dynamically constructed yet (Wine doesn't always
// guarantee CRT static-init order for injected DLLs).
//
// Usage:
//   Replace  #include <mutex>                          →  #include "win_mutex.h"
//   Replace  static std::mutex g_myMutex;              →  static WinMutex g_myMutex;
//   Replace  std::lock_guard<std::mutex> lk(g_myMutex) →  WinLockGuard lk(g_myMutex);
//   Replace  std::unique_lock<std::mutex> lk(...)      →  WinUniqueLock lk(g_myMutex);
//
// For condition_variable usage, use WinCondVar with WinMutex + WinUniqueLock.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <chrono>

// WinMutex — std::mutex replacement backed by SRWLOCK
class WinMutex {
public:
    // Aggregate-initializable: WinMutex m = {};  or  WinMutex m;
    // Both produce a valid, locked-never state because SRWLOCK_INIT is {0}.
    constexpr WinMutex() noexcept : m_srw(SRWLOCK_INIT) {}

    // Non-copyable, non-movable (same as std::mutex)
    WinMutex(const WinMutex&) = delete;
    WinMutex& operator=(const WinMutex&) = delete;

    void lock()     noexcept { AcquireSRWLockExclusive(&m_srw); }
    void unlock()   noexcept { ReleaseSRWLockExclusive(&m_srw); }
    bool try_lock() noexcept { return TryAcquireSRWLockExclusive(&m_srw) != 0; }

    // Expose the raw lock for condition-variable interop
    SRWLOCK* native_handle() noexcept { return &m_srw; }

private:
    SRWLOCK m_srw;
};

// WinLockGuard — std::lock_guard<std::mutex> replacement
class WinLockGuard {
public:
    explicit WinLockGuard(WinMutex& mtx) noexcept : m_mtx(mtx) { m_mtx.lock(); }
    ~WinLockGuard() noexcept { m_mtx.unlock(); }

    WinLockGuard(const WinLockGuard&) = delete;
    WinLockGuard& operator=(const WinLockGuard&) = delete;

private:
    WinMutex& m_mtx;
};

// WinUniqueLock — std::unique_lock<std::mutex> replacement (for cond-var use)
class WinUniqueLock {
public:
    explicit WinUniqueLock(WinMutex& mtx) noexcept
        : m_mtx(&mtx), m_owned(true)
    {
        m_mtx->lock();
    }

    ~WinUniqueLock() noexcept {
        if (m_owned)
            m_mtx->unlock();
    }

    void lock() noexcept {
        m_mtx->lock();
        m_owned = true;
    }

    void unlock() noexcept {
        m_mtx->unlock();
        m_owned = false;
    }

    bool owns_lock() const noexcept { return m_owned; }

    WinMutex* mutex() const noexcept { return m_mtx; }

    WinUniqueLock(const WinUniqueLock&) = delete;
    WinUniqueLock& operator=(const WinUniqueLock&) = delete;

private:
    WinMutex* m_mtx;
    bool      m_owned;
};

// WinCondVar — std::condition_variable replacement using SleepConditionVariableSRW
class WinCondVar {
public:
    constexpr WinCondVar() noexcept : m_cv(CONDITION_VARIABLE_INIT) {}

    WinCondVar(const WinCondVar&) = delete;
    WinCondVar& operator=(const WinCondVar&) = delete;

    void notify_one() noexcept { WakeConditionVariable(&m_cv); }
    void notify_all() noexcept { WakeAllConditionVariable(&m_cv); }

    // SleepConditionVariableSRW atomically releases the SRWLOCK and sleeps,
    // then re-acquires it before returning. No manual unlock/lock needed.
    void wait(WinUniqueLock& lock) noexcept {
        SleepConditionVariableSRW(&m_cv, lock.mutex()->native_handle(), INFINITE, 0);
    }

    template <typename Predicate>
    void wait(WinUniqueLock& lock, Predicate pred) noexcept {
        while (!pred()) {
            wait(lock);
        }
    }

    // wait_for with chrono duration (matches std::condition_variable API)
    template <typename Rep, typename Period>
    bool wait_for(WinUniqueLock& lock, const std::chrono::duration<Rep, Period>& timeout) noexcept {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count();
        DWORD dwMs = (ms <= 0) ? 0 : static_cast<DWORD>(ms);
        BOOL result = SleepConditionVariableSRW(&m_cv, lock.mutex()->native_handle(), dwMs, 0);
        return result != 0;
    }

    // wait_for with predicate (matches std::condition_variable API)
    template <typename Rep, typename Period, typename Predicate>
    bool wait_for(WinUniqueLock& lock, const std::chrono::duration<Rep, Period>& timeout, Predicate pred) noexcept {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!pred()) {
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0)
                return pred();
            DWORD dwMs = static_cast<DWORD>(remaining.count());
            SleepConditionVariableSRW(&m_cv, lock.mutex()->native_handle(), dwMs, 0);
        }
        return true;
    }

    // Returns false on timeout (raw milliseconds version)
    bool wait_for_ms(WinUniqueLock& lock, DWORD ms) noexcept {
        BOOL result = SleepConditionVariableSRW(&m_cv, lock.mutex()->native_handle(), ms, 0);
        return result != 0;
    }

private:
    CONDITION_VARIABLE m_cv;
};
