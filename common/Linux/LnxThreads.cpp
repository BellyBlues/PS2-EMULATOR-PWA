// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "common/Threading.h"
#include "common/Assertions.h"

#include <memory>

#include <pthread.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#include <sys/types.h>
#include <sched.h>

// glibc < v2.30 doesn't define gettid...
#if __GLIBC__ == 2 && __GLIBC_MINOR__ < 30
#include <sys/syscall.h>
#define gettid() syscall(SYS_gettid)
#endif
#elif defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#include <emscripten/threading.h>

#include <algorithm>
#include <atomic>
#else
#include <pthread_np.h>
#endif

// Note: assuming multicore is safer because it forces the interlocked routines to use
// the LOCK prefix.  The prefix works on single core CPUs fine (but is slow), but not
// having the LOCK prefix is very bad indeed.

__forceinline void Threading::Timeslice()
{
	sched_yield();
}

// For use in spin/wait loops,  Acts as a hint to Intel CPUs and should, in theory
// improve performance and reduce cpu power consumption.
__forceinline void Threading::SpinWait()
{
	// If this doesn't compile you can just comment it out (it only serves as a
	// performance hint and isn't required).
#if defined(ARCH_X86)
	__asm__("pause");
#elif defined(ARCH_ARM64)
	__asm__ __volatile__("isb");
#endif
}

__forceinline void Threading::EnableHiresScheduler()
{
	// Don't know if linux has a customizable scheduler resolution like Windows (doubtful)
}

__forceinline void Threading::DisableHiresScheduler()
{
}

// Unit of time of GetThreadCpuTime/GetCpuTime
u64 Threading::GetThreadTicksPerSecond()
{
	return 1000000;
}

// Helper function to get either either the current cpu usage
// in called thread or in id thread
static u64 get_thread_time(uptr id = 0)
{
	clockid_t cid;
	if (id)
	{
		int err = pthread_getcpuclockid((pthread_t)id, &cid);
		if (err)
			return 0;
	}
	else
	{
		cid = CLOCK_THREAD_CPUTIME_ID;
	}

	struct timespec ts;
	int err = clock_gettime(cid, &ts);
	if (err)
		return 0;

	return (u64)ts.tv_sec * (u64)1e6 + (u64)ts.tv_nsec / (u64)1e3;
}

// Returns the current timestamp (not relative to a real world clock)
u64 Threading::GetThreadCpuTime()
{
	return get_thread_time();
}

Threading::ThreadHandle::ThreadHandle() = default;

Threading::ThreadHandle::ThreadHandle(const ThreadHandle& handle)
	: m_native_handle(handle.m_native_handle)
#ifdef __linux__
	, m_native_id(handle.m_native_id)
#endif
{
}

Threading::ThreadHandle::ThreadHandle(ThreadHandle&& handle)
	: m_native_handle(handle.m_native_handle)
#ifdef __linux__
	, m_native_id(handle.m_native_id)
#endif
{
	handle.m_native_handle = nullptr;
#ifdef __linux__
	handle.m_native_id = 0;
#endif
}

Threading::ThreadHandle::~ThreadHandle() = default;

Threading::ThreadHandle Threading::ThreadHandle::GetForCallingThread()
{
	ThreadHandle ret;
	ret.m_native_handle = (void*)pthread_self();
#ifdef __linux__
	ret.m_native_id = gettid();
#endif
	return ret;
}

Threading::ThreadHandle& Threading::ThreadHandle::operator=(ThreadHandle&& handle)
{
	m_native_handle = handle.m_native_handle;
	handle.m_native_handle = nullptr;
#ifdef __linux__
	m_native_id = handle.m_native_id;
	handle.m_native_id = 0;
#endif
	return *this;
}

Threading::ThreadHandle& Threading::ThreadHandle::operator=(const ThreadHandle& handle)
{
	m_native_handle = handle.m_native_handle;
#ifdef __linux__
	m_native_id = handle.m_native_id;
#endif
	return *this;
}

#ifdef __EMSCRIPTEN__
// Per-thread wait accounting (Threading::ScopedWaitTimer): a thread is entered here the first time it
// waits or asks for its own CPU time, and its CPU time is its wall time since then minus its waits.
namespace
{
	struct ThreadWaitTimes
	{
		std::atomic<pthread_t> thread{};
		std::atomic<u64> start_us{0};
		std::atomic<u64> waited_us{0};
		std::atomic<u64> wait_since_us{0}; // non-zero while the thread waits
	};

	static constexpr int MAX_TIMED_THREADS = 64;
	static ThreadWaitTimes s_thread_times[MAX_TIMED_THREADS];
	static std::atomic<int> s_timed_threads{0};
	static thread_local ThreadWaitTimes* s_my_times = nullptr;
	static thread_local int s_wait_depth = 0;

	static u64 NowMicroseconds()
	{
		return static_cast<u64>(emscripten_get_now() * 1000.0);
	}

	static ThreadWaitTimes* MyWaitTimes()
	{
		if (!s_my_times)
		{
			const int index = s_timed_threads.fetch_add(1, std::memory_order_relaxed);
			if (index >= MAX_TIMED_THREADS)
				return nullptr;
			ThreadWaitTimes& t = s_thread_times[index];
			t.start_us.store(NowMicroseconds(), std::memory_order_relaxed);
			t.thread.store(pthread_self(), std::memory_order_release);
			s_my_times = &t;
		}
		return s_my_times;
	}
} // namespace

void Threading::BeginWait()
{
	if (s_wait_depth++ == 0)
	{
		if (ThreadWaitTimes* t = MyWaitTimes())
			t->wait_since_us.store(NowMicroseconds(), std::memory_order_relaxed);
	}
}

void Threading::EndWait()
{
	if (--s_wait_depth == 0 && s_my_times)
	{
		const u64 since = s_my_times->wait_since_us.exchange(0, std::memory_order_relaxed);
		if (since)
			s_my_times->waited_us.fetch_add(NowMicroseconds() - since, std::memory_order_relaxed);
	}
}

u64 Threading::ThreadHandle::GetCPUTime() const
{
	if (!m_native_handle)
		return 0;
	const pthread_t thread = (pthread_t)m_native_handle;
	if (pthread_equal(thread, pthread_self()))
		MyWaitTimes();
	const int count = std::min(s_timed_threads.load(std::memory_order_acquire), MAX_TIMED_THREADS);
	for (int i = 0; i < count; i++)
	{
		const ThreadWaitTimes& t = s_thread_times[i];
		if (!pthread_equal(t.thread.load(std::memory_order_acquire), thread))
			continue;
		const u64 now = NowMicroseconds();
		const u64 since = t.wait_since_us.load(std::memory_order_relaxed);
		const u64 waited = t.waited_us.load(std::memory_order_relaxed) + (since ? now - since : 0);
		const u64 lived = now - t.start_us.load(std::memory_order_relaxed);
		// Never 0 for a known thread: 0 means "no CPU clock" to the frontends.
		return std::max<u64>(1, lived > waited ? lived - waited : 0);
	}
	return 0;
}
#else
u64 Threading::ThreadHandle::GetCPUTime() const
{
	return m_native_handle ? get_thread_time((uptr)m_native_handle) : 0;
}
#endif

bool Threading::ThreadHandle::SetAffinity(u64 processor_mask) const
{
#if defined(__linux__)
	cpu_set_t set;
	CPU_ZERO(&set);

	if (processor_mask != 0)
	{
		for (u32 i = 0; i < 64; i++)
		{
			if (processor_mask & (static_cast<u64>(1) << i))
			{
				CPU_SET(i, &set);
			}
		}
	}
	else
	{
		long num_processors = sysconf(_SC_NPROCESSORS_CONF);
		for (long i = 0; i < num_processors; i++)
		{
			CPU_SET(i, &set);
		}
	}

	return sched_setaffinity((pid_t)m_native_id, sizeof(set), &set) >= 0;
#else
	return false;
#endif
}

Threading::Thread::Thread() = default;

Threading::Thread::Thread(Thread&& thread)
	: ThreadHandle(thread)
	, m_stack_size(thread.m_stack_size)
{
	thread.m_stack_size = 0;
}

Threading::Thread::Thread(EntryPoint func)
	: ThreadHandle()
{
	if (!Start(std::move(func)))
		pxFailRel("Failed to start implicitly started thread.");
}

Threading::Thread::~Thread()
{
	pxAssertRel(!m_native_handle, "Thread should be detached or joined at destruction");
}

void Threading::Thread::SetStackSize(u32 size)
{
	pxAssertRel(!m_native_handle, "Can't change the stack size on a started thread");
	m_stack_size = size;
}

#ifdef __linux__
// For Linux, we have to do a bit of trickery here to get the thread's ID back from
// the thread itself, because it's not part of pthreads. We use a semaphore to signal
// when the thread has started, and filled in thread_id_ptr.
struct ThreadProcParameters
{
	Threading::Thread::EntryPoint func;
	Threading::KernelSemaphore* start_semaphore;
	unsigned int* thread_id_ptr;
};

void* Threading::Thread::ThreadProc(void* param)
{
	std::unique_ptr<ThreadProcParameters> entry(static_cast<ThreadProcParameters*>(param));
	*entry->thread_id_ptr = gettid();
	entry->start_semaphore->Post();
	entry->func();
	return nullptr;
}

bool Threading::Thread::Start(EntryPoint func)
{
	pxAssertRel(!m_native_handle, "Can't start an already-started thread");

	KernelSemaphore start_semaphore;
	std::unique_ptr<ThreadProcParameters> params(std::make_unique<ThreadProcParameters>());
	params->func = std::move(func);
	params->start_semaphore = &start_semaphore;
	params->thread_id_ptr = &m_native_id;

	pthread_attr_t attrs;
	bool has_attributes = false;

	if (m_stack_size != 0)
	{
		has_attributes = true;
		pthread_attr_init(&attrs);
	}
	if (m_stack_size != 0)
		pthread_attr_setstacksize(&attrs, m_stack_size);

	pthread_t handle;
	const int res = pthread_create(&handle, has_attributes ? &attrs : nullptr, ThreadProc, params.get());
	if (res != 0)
		return false;

	// wait until it sets our native id
	start_semaphore.Wait();

	// thread started, it'll release the memory
	m_native_handle = (void*)handle;
	params.release();
	return true;
}

#else

void* Threading::Thread::ThreadProc(void* param)
{
	std::unique_ptr<EntryPoint> entry(static_cast<EntryPoint*>(param));
	(*entry.get())();
	return nullptr;
}

bool Threading::Thread::Start(EntryPoint func)
{
	pxAssertRel(!m_native_handle, "Can't start an already-started thread");

	std::unique_ptr<EntryPoint> func_clone(std::make_unique<EntryPoint>(std::move(func)));

	pthread_attr_t attrs;
	bool has_attributes = false;

	if (m_stack_size != 0)
	{
		has_attributes = true;
		pthread_attr_init(&attrs);
	}
	if (m_stack_size != 0)
		pthread_attr_setstacksize(&attrs, m_stack_size);

	pthread_t handle;
	const int res = pthread_create(&handle, has_attributes ? &attrs : nullptr, ThreadProc, func_clone.get());
	if (res != 0)
		return false;

	// thread started, it'll release the memory
	m_native_handle = (void*)handle;
	func_clone.release();
	return true;
}

#endif

void Threading::Thread::Detach()
{
	pxAssertRel(m_native_handle, "Can't detach without a thread");
	pthread_detach((pthread_t)m_native_handle);
	m_native_handle = nullptr;
#ifdef __linux__
	m_native_id = 0;
#endif
}

void Threading::Thread::Join()
{
	pxAssertRel(m_native_handle, "Can't join without a thread");
	void* retval;
	const int res = pthread_join((pthread_t)m_native_handle, &retval);
	if (res != 0)
		pxFailRel("pthread_join() for thread join failed");

	m_native_handle = nullptr;
#ifdef __linux__
	m_native_id = 0;
#endif
}

Threading::ThreadHandle& Threading::Thread::operator=(Thread&& thread)
{
	ThreadHandle::operator=(thread);
	m_stack_size = thread.m_stack_size;
	thread.m_stack_size = 0;
	return *this;
}

void Threading::SetNameOfCurrentThread(const char* name)
{
#if defined(__linux__)
	// Extract of manpage: "The name can be up to 16 bytes long, and should be
	//						null-terminated if it contains fewer bytes."
	prctl(PR_SET_NAME, name, 0, 0, 0);
#elif defined(__EMSCRIPTEN__)
	emscripten_set_thread_name(pthread_self(), name);
#elif defined(__unix__)
	pthread_set_name_np(pthread_self(), name);
#endif
}
