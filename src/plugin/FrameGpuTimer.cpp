#include "FrameGpuTimer.h"

void FrameGpuTimer::Initialize(ID3D11Device* a_device, ID3D11DeviceContext* a_context)
{
	Release();

	if (!a_device || !a_context)
		return;

	context = a_context;

	D3D11_QUERY_DESC disjointDesc{};
	disjointDesc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;

	D3D11_QUERY_DESC timestampDesc{};
	timestampDesc.Query = D3D11_QUERY_TIMESTAMP;

	for (auto& frame : frames) {
		if (FAILED(a_device->CreateQuery(&disjointDesc, frame.disjoint.GetAddressOf())) ||
			FAILED(a_device->CreateQuery(&timestampDesc, frame.begin.GetAddressOf())) ||
			FAILED(a_device->CreateQuery(&timestampDesc, frame.end.GetAddressOf())) ||
			FAILED(a_device->CreateQuery(&timestampDesc, frame.postSubmit.GetAddressOf()))) {
			logger::warn("[FrameGpuTimer] Failed to create timestamp queries; frame GPU time will be unavailable");
			Release();
			return;
		}
		frame.frameIndex = 0;
		frame.inFlight = false;
		frame.postSubmitStamped = false;
	}

	writeSlot = 0;
	openSlot = 0;
	armed = false;
	bracketOpen = false;
	endStamped = false;
	postSubmitStamped = false;
	submittedFrameIndex = 0;
	initialized = true;

	logger::info("[FrameGpuTimer] Frame GPU timer initialised ({} buffered query sets)", kFrameLatency);
}

void FrameGpuTimer::Release()
{
	for (auto& frame : frames) {
		frame.disjoint = nullptr;
		frame.begin = nullptr;
		frame.end = nullptr;
		frame.postSubmit = nullptr;
		frame.frameIndex = 0;
		frame.inFlight = false;
		frame.postSubmitStamped = false;
	}

	context = nullptr;
	writeSlot = 0;
	openSlot = 0;
	initialized = false;
	armed = false;
	bracketOpen = false;
	endStamped = false;
	postSubmitStamped = false;
	submittedFrameIndex = 0;
	lastFramePacked.store(0, std::memory_order_release);
	lastPostSubmitPacked.store(0, std::memory_order_release);
	framesOpenedBeforeSync.store(0, std::memory_order_relaxed);
	frameCounter = 0;
}

void FrameGpuTimer::ArmFrame()
{
	if (!initialized)
		return;

	// A bracket left open by a frame that never reached Present (device reset,
	// render target relatch) would otherwise leak its slot.
	bracketOpen = false;
	endStamped = false;
	postSubmitStamped = false;

	CollectCompletedFrames();

	writeSlot = (writeSlot + 1) % kFrameLatency;

	// Still waiting on the GPU for this slot: skip timing this frame rather
	// than overwrite a query that is in flight.
	armed = !frames[writeSlot].inFlight;
}

void FrameGpuTimer::OnDrawSubmitted()
{
	if (!armed)
		return;

	armed = false;

	auto& frame = frames[writeSlot];
	context->Begin(frame.disjoint.Get());
	context->End(frame.begin.Get());

	openSlot = writeSlot;
	bracketOpen = true;
}

void FrameGpuTimer::OnFrameSync()
{
	if (!initialized)
		return;

	if (bracketOpen) {
		// The game drew before the runtime released it, so the bracket opened
		// ahead of the pacing block. Move the start forward to here: the same
		// last-End-wins rule that lets the close move per eye.
		context->End(frames[openSlot].begin.Get());
		framesOpenedBeforeSync.fetch_add(1, std::memory_order_relaxed);
	}
}

void FrameGpuTimer::OnCompositorSubmit()
{
	if (!initialized || !bracketOpen)
		return;

	// Stamp the end here, and again on any later submit in the same frame. Eyes
	// are submitted separately and Community Shaders upscales each one inside
	// the submit hook, so the first eye's submit is too early - the second
	// eye's render work would fall outside the bracket. Re-issuing End moves
	// the timestamp forward, so the reading ends at the LAST submit: after all
	// of the frame's GPU work, before the runtime call that blocks on pacing.
	context->End(frames[openSlot].end.Get());
	endStamped = true;
}

void FrameGpuTimer::OnCompositorSubmitReturned()
{
	// endStamped, not just bracketOpen: without a matching end stamp there is
	// no interval to measure from, and a stray stamp would be read against
	// whatever `end` held from an earlier frame.
	if (!initialized || !bracketOpen || !endStamped)
		return;

	// Re-stamped per eye like the end above, so the LAST one wins and the
	// interval measured is the final eye's submit - which is exactly the part
	// that falls outside the main bracket.
	context->End(frames[openSlot].postSubmit.Get());
	postSubmitStamped = true;
}

void FrameGpuTimer::EndFrame()
{
	if (!initialized)
		return;

	if (bracketOpen) {
		auto& frame = frames[openSlot];
		// No submit reached us - a loading screen or a flat menu frame. Stamp
		// the end here so the frame is still measured rather than dropped.
		if (!endStamped) {
			context->End(frame.end.Get());
		}
		// The disjoint block only has to enclose both timestamps, so closing it
		// at Present is correct and keeps a single close path.
		context->End(frame.disjoint.Get());
		frame.frameIndex = ++submittedFrameIndex;
		frame.inFlight = true;
		// Carried into the slot because collection happens frames later, by
		// which time the live flag describes a different frame.
		frame.postSubmitStamped = postSubmitStamped;
		bracketOpen = false;
		endStamped = false;
		postSubmitStamped = false;
	}

	CollectCompletedFrames();

	// Periodic, because "the start boundary never opens early" is a claim, and
	// this is the number that settles it. Roughly once a minute at 72 Hz.
	if (++frameCounter % 4096 == 0) {
		const uint64_t early = framesOpenedBeforeSync.load(std::memory_order_relaxed);
		logger::info("[FrameGpuTimer] {} frames timed, {} opened before WaitGetPoses returned ({:.1f}%)",
			frameCounter, early, 100.0 * static_cast<double>(early) / static_cast<double>(frameCounter));
	}
}

void FrameGpuTimer::CollectCompletedFrames()
{
	for (auto& frame : frames) {
		if (frame.inFlight)
			TryCollectFrame(frame);
	}
}

bool FrameGpuTimer::TryCollectFrame(FrameQueries& a_frame)
{
	D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjointData{};
	if (context->GetData(a_frame.disjoint.Get(), &disjointData, sizeof(disjointData), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
		return false;

	uint64_t beginTicks = 0;
	uint64_t endTicks = 0;
	const bool haveTimestamps =
		context->GetData(a_frame.begin.Get(), &beginTicks, sizeof(beginTicks), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
		context->GetData(a_frame.end.Get(), &endTicks, sizeof(endTicks), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;

	// The slot is freed either way: the disjoint query has completed, so the
	// data is as good as it will ever be.
	a_frame.inFlight = false;

	// Disjoint means the GPU clock was unreliable across the interval (clock
	// change, TDR). The D3D11 contract is to throw the result away.
	if (!haveTimestamps || disjointData.Disjoint || disjointData.Frequency == 0 || endTicks <= beginTicks)
		return false;

	const uint64_t elapsedUs = ((endTicks - beginTicks) * 1000000ull) / disjointData.Frequency;
	if (elapsedUs > kMaxPlausibleFrameGpuTimeUs)
		return false;

	// Frames complete in submission order in practice, but a late arrival must
	// never overwrite a newer reading.
	if (a_frame.frameIndex <= GetLastFrameGpuTimeFrameIndex())
		return true;

	// One store, so a reader cannot observe a new index against an old time.
	lastFramePacked.store((a_frame.frameIndex << kGpuTimeBits) | (elapsedUs & kGpuTimeMask),
		std::memory_order_release);

	// E-49, published separately and only when this frame actually stamped it.
	// Everything the main reading is guarded by applies here too: the same
	// disjoint block covers it, so its Disjoint and Frequency checks above hold
	// for this delta as well.
	if (a_frame.postSubmitStamped) {
		uint64_t postTicks = 0;
		if (context->GetData(a_frame.postSubmit.Get(), &postTicks, sizeof(postTicks),
				D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
			postTicks > endTicks) {
			const uint64_t postUs = ((postTicks - endTicks) * 1000000ull) / disjointData.Frequency;
			if (postUs <= kMaxPlausibleFrameGpuTimeUs) {
				lastPostSubmitPacked.store(
					(a_frame.frameIndex << kGpuTimeBits) | (postUs & kGpuTimeMask),
					std::memory_order_release);
			}
		}
	}
	return true;
}
