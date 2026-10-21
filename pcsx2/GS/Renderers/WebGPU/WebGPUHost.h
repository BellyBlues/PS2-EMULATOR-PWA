// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <webgpu/webgpu.h>

#include <atomic>
#include <string>
#include <utility>

// Platform layer of the WebGPU GS device (docs/web-port/WEBGPU-DESIGN.md §3.2, §12), made from the
// runtime spike's primitives (tools/wasm/webgpu-spike/spike.cpp):
//   - timed waits for WebGPU futures, which suspend the GS thread's stack with JSPI;
//   - a suspending sleep, which lets the GS thread's worker run its event loop;
//   - the GS-owned OffscreenCanvas the device draws into, and the frame port: each frame is posted
//     to the page as an ImageBitmap, without returning to the event loop;
//   - write-mapped staging buffers, mapped from JS (see StagingMap());
//   - the page's surface-size feed and the frontend's switches.
// The frontend functions are lock-free and may be called from any thread (the page's exports); the
// others run on the GS thread, which owns every WebGPU object (they live in its worker).
namespace WebGPUHost
{
	// ---- frontend, any thread ----

	/// False keeps the GS on GSDeviceNull and the page on its 2D canvas (?gpu=0).
	void SetEnabled(bool enabled);

	/// The page's canvas backing size in device pixels, and its device pixel ratio.
	void SetSurfaceSize(u32 width, u32 height, float scale);

	/// Test hook: the device destroys itself at its next present, as on a device loss.
	void RequestTestDeviceLoss();

	/// Test hook: false keeps the GS thread's idle wait blocking while it owns a device, so that only
	/// the bounded event-loop turn in EndPresent() delivers WebGPU callbacks (default true).
	void SetIdleWaitsSuspend(bool enabled);
	bool IdleWaitsSuspend();

	/// Test switch: true makes a draw whose TFX pipeline is not created yet wait for it to the end, as
	/// before pipelines were created in the background (for pixel-exact comparisons). The default,
	/// false, waits a few milliseconds and skips the draw if the pipeline is still not ready.
	void SetPipelinesWait(bool always);
	bool PipelinesWait();

	/// Per-game TFX pipeline lists (pcsx2-wasm/README.md, "Pipeline lists"): blocks of TFX pipeline keys
	/// (GSDeviceWebGPU::TFXPipelineKey, TFX_KEY_BYTES each) shared with the page, which keeps a list per
	/// game in IndexedDB. version_lo/hi identify what a key means (GSDeviceWebGPU::ComputeTFXKeyVersion()).
	static constexpr u32 TFX_KEY_BYTES = 36;
	static constexpr u32 TFX_KEY_CAPACITY = 4096;
	struct TFXKeyBlock
	{
		/// The log: a session number, incremented when a device starts a new log. The prewarm slot:
		/// TFX_SLOT_* (the page claims it with FREE -> WRITING or READY -> WRITING, then sets READY).
		std::atomic<u32> state;
		std::atomic<u32> count; ///< keys in records (published with release by their writer)
		u32 version_lo;
		u32 version_hi;
		alignas(8) u8 records[TFX_KEY_CAPACITY * TFX_KEY_BYTES];
	};
	static constexpr u32 TFX_SLOT_FREE = 0;
	static constexpr u32 TFX_SLOT_WRITING = 1;
	static constexpr u32 TFX_SLOT_READY = 2;
	static constexpr u32 TFX_SLOT_TAKING = 3;
	/// The keys the GS device's draws asked for this session, in order (GS thread appends, the page reads).
	TFXKeyBlock* TFXKeyLog();
	/// A list the page hands over for prewarming (the page writes, the GS thread takes it at a present).
	TFXKeyBlock* TFXPrewarmSlot();

	// ---- GS thread ----

	/// The frontend allows WebGPU, the GS thread's worker has navigator.gpu and JSPI, and WebGPU was
	/// not given up.
	bool IsAvailable();

	/// Gives up WebGPU for this page after repeated device loss: tells the page, which switches its
	/// canvas back to the 2D path and shows a notice. IsAvailable() is false from now on.
	void GiveUp(const char* reason);

	/// No device could be created for this GS session (at boot): it draws through the 2D path, and
	/// the next session tries WebGPU again. Tells the page why.
	void NoteUnavailable(const char* reason);

	bool ConsumeTestDeviceLossRequest();

	/// Why the last device creation failed, in plain words for the page ("the browser gave no WebGPU
	/// adapter"), and whether it got no adapter at all (then the adapter retries already took their
	/// time and the GS does not try a whole device again). ClearFailure() at the start of a creation.
	void ClearFailure();
	void SetFailure(std::string reason, bool no_adapter = false);
	const char* FailureReason();
	bool FailureWasNoAdapter();

	/// The device works for presenting but lacks what the hardware renderer needs (dual-source
	/// blending): the GS then draws with the SW renderer through it. Empty when the HW renderer can run.
	void SetHWUnsupported(std::string reason);
	const std::string& HWUnsupported();

	/// Tells the page which renderer the GS opened (hw: PCSX2's hardware renderer on WebGPU, else the
	/// software one) and, when it is not the one asked for or WebGPU is not used, why.
	void NoteRenderer(bool hw, bool webgpu, const char* reason);

	/// Called when a device is ready: frames posted from now on carry the next device number, so
	/// the page can tell when a recovered device's frames arrive. Ends a recovery.
	void NoteDeviceCreated();

	/// The device reported a loss: the device opened next is a recovery (until one is ready).
	void NoteDeviceLost();
	bool IsRecoveringFromLoss();

	/// The latest size from the page. False if it never sent one.
	bool GetSurfaceSize(u32* width, u32* height, float* scale);

	/// Waits for a future (suspending through JSPI) for at most timeout_ms; 0 only polls.
	WGPUWaitStatus Wait(WGPUInstance instance, WGPUFuture future, double timeout_ms);
	/// Waits until one of the futures completes, for at most timeout_ms (then at most 64 futures); 0 only
	/// polls, and completes every future that is ready.
	WGPUWaitStatus WaitAny(WGPUInstance instance, WGPUFutureWaitInfo* infos, u32 count, double timeout_ms);

	/// GS thread: starts a new key log for a device (TFXKeyLog()).
	void ResetTFXKeyLog(u64 version);

	/// Suspends the GS thread for ms milliseconds, letting its worker run its event loop.
	void SleepSuspending(double ms);

	/// True while the GS thread is suspended in Wait(), SleepSuspending() or its idle wait for work: its
	/// worker's event loop runs then, and nothing it runs may enter GS code (§3.2; checked in debug
	/// builds). Nothing is proxied to the GS thread, so there is no proxy entry to check.
	bool InSuspendingWait();

	/// How long the GS thread's worker has not run its event loop, in milliseconds: the age of a
	/// microtask it queued that has not run yet (it runs once the thread suspends). Queues one when
	/// none is pending (and returns 0).
	double EventLoopStalledMs();

	/// Creates the GS-owned OffscreenCanvas (replacing an earlier one) and registers it under
	/// CanvasSelector() for wgpuInstanceCreateSurface. The first call also hands the frame port to the
	/// page (Module.tstFramePort). wgpuSurfaceConfigure() sizes it.
	bool CreateCanvas(u32 width, u32 height);
	void DestroyCanvas();
	const char* CanvasSelector();

	/// Takes the canvas's frame with transferToImageBitmap() and posts it to the page with its post
	/// count, the device number and the number of the vsync it shows (counted from boot). Returns
	/// the time it took, in milliseconds, or a negative value if nothing was posted.
	double PostFrame(u32 vsync);

	/// Write-mapped staging buffers (MapWrite | CopySrc). After their first use (they are mapped at
	/// creation) they are mapped, written and unmapped by JS directly rather than through webgpu.h,
	/// whose futures table keeps an entry for every mapAsync that no timed wait collects (one per
	/// frame here). StagingMap() starts mapping, after the submit that read the buffer;
	/// StagingIsMapped() says whether it is mapped now; StagingWrite() copies size bytes from data to
	/// its start and unmaps it (false if it was not mapped); StagingForget() before destroying it.
	void StagingMap(WGPUBuffer buffer);
	bool StagingIsMapped(WGPUBuffer buffer);
	bool StagingWrite(WGPUBuffer buffer, const void* data, u32 size);
	void StagingForget(WGPUBuffer buffer);

	/// Milliseconds on the same clock on every thread (performance.now() of the time origin).
	double Now();

	std::string StringViewToString(WGPUStringView sv);

	/// Logs that the callback of a request whose wait had timed out ran at last (RequestResult).
	void NoteAbandonedRequestDone();

	/// The result of a request waited for with Wait() (WaitAnyOnly callback mode). Its callback runs
	/// inside the wait that sees the request complete; a wait that times out leaves the callback
	/// registered, and it runs later (at the latest with a "cancelled" status when the instance is
	/// released), so the result cannot live on the waiter's stack. The waiter allocates it with new
	/// and passes it as the callback's userdata1; the callback calls Arrive(), the waiter Take().
	template <typename T>
	struct RequestResult
	{
		T value = {};
		bool done = false;
		bool abandoned = false;
		bool quiet = false; ///< abandoned on purpose: no log when its callback runs

		/// In the callback: the result to fill in, or null when the waiter has given up (the result is
		/// freed then, and the callback releases whatever it was handed).
		static RequestResult* Arrive(void* userdata)
		{
			RequestResult* r = static_cast<RequestResult*>(userdata);
			if (r->abandoned)
			{
				if (!r->quiet)
					NoteAbandonedRequestDone();
				delete r;
				return nullptr;
			}
			r->done = true;
			return r;
		}

		/// In the waiter, after waiting: if the callback ran, moves its value to *out, frees the result
		/// and returns true. Otherwise leaves the result to the callback and returns false.
		bool Take(T* out)
		{
			if (!done)
			{
				abandoned = true;
				return false;
			}
			*out = std::move(value);
			delete this;
			return true;
		}
	};
} // namespace WebGPUHost
