// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "CDVD/Emscripten/WebDisc.h"

#include "common/Assertions.h"
#include "common/Error.h"

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <memory>

// The worker writes these words with Atomics on an Int32Array; std::atomic of the same size, lock-free
// and sequentially consistent, compiles to the same wasm atomics. Waits and wakes go through
// emscripten_futex_wait/wake, which are memory.atomic.wait32/notify on exactly the word given. (Not
// std::atomic::wait: libc++ may wait on a contention-table entry instead, which a notify from JS would
// never wake.)

namespace WebDisc
{
	static RequestBlock s_block;
	static std::atomic<bool> s_aborted{false};

	// Wait slices: requesters re-check the block state, their cancel flag and the deadline this often.
	static constexpr double WAIT_SLICE_MS = 500.0;
	static constexpr double CANCEL_SLICE_MS = 10.0;
	static constexpr double NO_SLOT_SLEEP_MS = 1.0;

	static void RingDoorbell()
	{
		s_block.doorbell.fetch_add(1, std::memory_order_seq_cst);
		emscripten_futex_wake(&s_block.doorbell, INT_MAX);
	}

	static s32 NotReadyResult(s32 state)
	{
		return (state == BLOCK_STOPPED) ? E_CANCELLED : E_NO_DISC;
	}
} // namespace WebDisc

WebDisc::RequestBlock* WebDisc::GetBlock()
{
	return &s_block;
}

bool WebDisc::IsDiscPath(std::string_view path)
{
	static constexpr std::string_view prefix = "/disc/";
	return path.size() > prefix.size() && path.starts_with(prefix) &&
		   path.find('/', prefix.size()) == std::string_view::npos;
}

bool WebDisc::Query(Info* info, Error* error)
{
	// The worker publishes a disc as: STATE = no disc, generation + 1, size, generation + 1, STATE =
	// ready (all in one synchronous step). A ready state seen between two equal generation reads therefore
	// belongs to that generation, and so does the size read in between. That step runs on another thread,
	// so "no disc" may be its middle: only a state that stays put for PUBLISH_SETTLE_MS is an answer.
	static constexpr u32 PUBLISH_SETTLE_MS = 20;
	const bool can_wait = !emscripten_is_main_browser_thread();
	u32 settled_ms = 0;
	u32 last_generation = s_block.generation.load(std::memory_order_seq_cst);
	for (u32 attempt = 0; attempt < 1000; attempt++)
	{
		const u32 generation = s_block.generation.load(std::memory_order_seq_cst);
		const s32 state = s_block.state.load(std::memory_order_seq_cst);
		const u32 size_lo = s_block.size_lo.load(std::memory_order_seq_cst);
		const u32 size_hi = s_block.size_hi.load(std::memory_order_seq_cst);
		if (s_block.generation.load(std::memory_order_seq_cst) != generation)
			continue;

		if (state != BLOCK_READY)
		{
			if (generation != last_generation)
			{
				last_generation = generation;
				settled_ms = 0;
			}
			if (can_wait && settled_ms < PUBLISH_SETTLE_MS)
			{
				u32 never = 1;
				emscripten_futex_wait(&never, 1, NO_SLOT_SLEEP_MS);
				settled_ms++;
				continue;
			}
			Error::SetStringView(error, (state == BLOCK_DETACHED) ? "The page's disc reader is not running." :
																	"The page has no disc open.");
			return false;
		}

		info->generation = generation;
		info->size = (static_cast<u64>(size_hi) << 32) | size_lo;
		return true;
	}

	Error::SetStringView(error, "The page's disc kept changing while it was being opened.");
	return false;
}

s32 WebDisc::Read(u32 generation, void* dst, u64 offset, u32 length, const std::atomic<bool>* cancel, u32 timeout_ms)
{
	pxAssertMsg(!emscripten_is_main_browser_thread(), "Disc reads block, so never on the browser's main thread");

	if (length == 0)
		return 0;
	if (!dst || length > MAX_REQUEST)
		return E_INVALID;

	const auto cancel_requested = [cancel]() {
		return s_aborted.load(std::memory_order_acquire) || (cancel && cancel->load(std::memory_order_acquire));
	};

	// Claim a free slot (CAS FREE -> CLAIMED); while all are taken, sleep briefly and look again.
	Slot* slot = nullptr;
	for (;;)
	{
		const s32 state = s_block.state.load(std::memory_order_seq_cst);
		if (state != BLOCK_READY)
			return NotReadyResult(state);
		if (s_block.generation.load(std::memory_order_seq_cst) != generation)
			return E_STALE;
		if (cancel_requested())
			return E_CANCELLED;

		for (Slot& candidate : s_block.slot)
		{
			s32 expected = SLOT_FREE;
			if (candidate.state.compare_exchange_strong(expected, SLOT_CLAIMED, std::memory_order_seq_cst))
			{
				slot = &candidate;
				break;
			}
		}
		if (slot)
			break;

		u32 never = 1;
		emscripten_futex_wait(&never, 1, NO_SLOT_SLEEP_MS);
	}

	slot->result.store(0, std::memory_order_seq_cst);
	slot->dst.store(static_cast<u32>(reinterpret_cast<uptr>(dst)), std::memory_order_seq_cst);
	slot->length.store(length, std::memory_order_seq_cst);
	slot->offset_lo.store(static_cast<u32>(offset), std::memory_order_seq_cst);
	slot->offset_hi.store(static_cast<u32>(offset >> 32), std::memory_order_seq_cst);
	slot->generation.store(generation, std::memory_order_seq_cst);
	slot->cancel.store(0, std::memory_order_seq_cst);
	slot->state.store(SLOT_PENDING, std::memory_order_seq_cst);
	RingDoorbell();

	const double deadline = emscripten_get_now() + static_cast<double>(timeout_ms);
	bool cancelling = false;
	bool deadline_cancel = false;
	for (;;)
	{
		const s32 seen = slot->state.load(std::memory_order_seq_cst);
		if (seen == SLOT_DONE)
			break;

		emscripten_futex_wait(&slot->state, static_cast<u32>(seen), (cancel || cancelling) ? CANCEL_SLICE_MS : WAIT_SLICE_MS);
		if (slot->state.load(std::memory_order_seq_cst) == SLOT_DONE)
			break;

		const bool cancelled = cancel_requested();
		const bool timed_out = !cancelled && (emscripten_get_now() >= deadline);
		const bool want_cancel = timed_out || cancelled;
		const s32 block_state = s_block.state.load(std::memory_order_seq_cst);
		if (!want_cancel && block_state == BLOCK_READY)
			continue;

		// Withdraw a request the worker has not taken yet; exactly one of this CAS and the worker's
		// PENDING -> BUSY wins.
		s32 expected = SLOT_PENDING;
		if (slot->state.compare_exchange_strong(expected, SLOT_FREE, std::memory_order_seq_cst))
			return want_cancel ? (timed_out ? E_TIMEOUT : E_CANCELLED) : NotReadyResult(block_state);

		// BUSY: the worker owns dst until it stores DONE, so keep waiting. It finishes busy requests when
		// asked to cancel, on close and on stop. When its loop dies it hands them back (BUSY -> PENDING) for
		// the worker the page starts next, and when the worker itself dies the page does that (or fails
		// them, after its last restart) once it knows the worker is gone.
		if (want_cancel && !cancelling)
		{
			cancelling = true;
			deadline_cancel = timed_out;
			slot->cancel.store(timed_out ? CANCEL_DEADLINE : CANCEL_REQUESTED, std::memory_order_seq_cst);
			RingDoorbell();
		}
	}

	s32 result = slot->result.load(std::memory_order_seq_cst);
	slot->state.store(SLOT_FREE, std::memory_order_seq_cst);
	// A worker that predates CANCEL_DEADLINE (or the page failing a dead worker's slots) answers a
	// deadline cancel with CANCELLED; it is still a timeout.
	if (deadline_cancel && result == E_CANCELLED)
		result = E_TIMEOUT;
	return result;
}

void WebDisc::AbortReads()
{
	s_aborted.store(true, std::memory_order_release);

	// Wake requesters early so they withdraw or cancel now rather than at the end of their wait slice.
	for (Slot& slot : s_block.slot)
		emscripten_futex_wake(&slot.state, INT_MAX);
}

void WebDisc::ResumeReads()
{
	s_aborted.store(false, std::memory_order_release);
}

bool WebDisc::ReadsAborted()
{
	return s_aborted.load(std::memory_order_acquire);
}

const char* WebDisc::ResultName(s32 result)
{
	switch (result)
	{
		case E_IO:
			return "the disc file could not be read";
		case E_INVALID:
			return "invalid request";
		case E_NO_DISC:
			return "no disc";
		case E_STALE:
			return "the disc was changed";
		case E_CANCELLED:
			return "cancelled";
		case E_NETWORK:
			return "network error";
		case E_TIMEOUT:
			return "timed out";
		default:
			return (result >= 0) ? "ok" : "unknown error";
	}
}

namespace WebDisc
{
	// OpenCFile(): the cookie behind the FILE*. Windows are aligned to WINDOW_SIZE; a read that misses fills
	// the least recently used one with one request to the worker. Reads of a window or more skip the cache.
	static constexpr u32 WINDOW_SIZE = 128 * 1024;
	static constexpr u32 WINDOWS = 4;

	struct CFileCookie
	{
		u32 generation = 0;
		u64 size = 0;
		u64 pos = 0;
		u64 use = 0;
		struct Window
		{
			u64 start = UINT64_MAX;
			u32 length = 0;
			u64 used = 0;
			std::unique_ptr<u8[]> data;
		} window[WINDOWS];
	};

	static ssize_t CFileRead(void* opaque, char* buf, size_t len)
	{
		CFileCookie* c = static_cast<CFileCookie*>(opaque);
		size_t done = 0;
		while (done < len && c->pos < c->size)
		{
			const u64 want = std::min<u64>(len - done, c->size - c->pos);
			if (want >= WINDOW_SIZE)
			{
				const u32 length = static_cast<u32>(std::min<u64>(want, MAX_REQUEST));
				const s32 got = Read(c->generation, buf + done, c->pos, length);
				if (got <= 0)
					return done ? static_cast<ssize_t>(done) : -1;
				done += static_cast<u32>(got);
				c->pos += static_cast<u32>(got);
				continue;
			}

			const u64 start = c->pos - (c->pos % WINDOW_SIZE);
			CFileCookie::Window* w = nullptr;
			for (CFileCookie::Window& x : c->window)
			{
				if (x.start == start)
					w = &x;
			}
			if (!w)
			{
				w = &c->window[0];
				for (CFileCookie::Window& x : c->window)
				{
					if (x.used < w->used)
						w = &x;
				}
				if (!w->data)
					w->data = std::make_unique<u8[]>(WINDOW_SIZE);
				const u32 length = static_cast<u32>(std::min<u64>(WINDOW_SIZE, c->size - start));
				w->start = UINT64_MAX;
				const s32 got = Read(c->generation, w->data.get(), start, length);
				if (got <= 0)
					return done ? static_cast<ssize_t>(done) : -1;
				w->start = start;
				w->length = static_cast<u32>(got);
			}
			w->used = ++c->use;
			const u32 at = static_cast<u32>(c->pos - start);
			if (at >= w->length)
				break;
			const u32 n = static_cast<u32>(std::min<u64>(want, w->length - at));
			std::memcpy(buf + done, w->data.get() + at, n);
			done += n;
			c->pos += n;
		}
		return static_cast<ssize_t>(done);
	}

	static int CFileSeek(void* opaque, off_t* offset, int whence)
	{
		CFileCookie* c = static_cast<CFileCookie*>(opaque);
		s64 base;
		switch (whence)
		{
			case SEEK_SET:
				base = 0;
				break;
			case SEEK_CUR:
				base = static_cast<s64>(c->pos);
				break;
			case SEEK_END:
				base = static_cast<s64>(c->size);
				break;
			default:
				return -1;
		}
		const s64 to = base + static_cast<s64>(*offset);
		if (to < 0)
			return -1;
		c->pos = static_cast<u64>(to);
		*offset = static_cast<off_t>(to);
		return 0;
	}

	static int CFileClose(void* opaque)
	{
		delete static_cast<CFileCookie*>(opaque);
		return 0;
	}
} // namespace WebDisc

std::FILE* WebDisc::OpenCFile(const char* path, Error* error)
{
	if (!path || !IsDiscPath(path))
	{
		Error::SetStringView(error, "Not a disc path.");
		return nullptr;
	}

	Info info;
	if (!Query(&info, error))
		return nullptr;

	CFileCookie* cookie = new CFileCookie();
	cookie->generation = info.generation;
	cookie->size = info.size;
	cookie_io_functions_t io = {};
	io.read = CFileRead;
	io.seek = CFileSeek;
	io.close = CFileClose;
	std::FILE* fp = fopencookie(cookie, "rb", io);
	if (!fp)
	{
		delete cookie;
		Error::SetStringView(error, "Could not open the disc as a file.");
		return nullptr;
	}

	// Unbuffered: with a buffer, musl splits every fread into two callbacks (DISC-DESIGN.md 5.1); the windows
	// above are the buffer.
	std::setvbuf(fp, nullptr, _IONBF, 0);
	return fp;
}
