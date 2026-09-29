#include "MOVINOscUdpReceiveThread.h"

#include "HAL/PlatformTime.h"
#include "HAL/RunnableThread.h"
#include "Sockets.h"
#include "SocketSubsystem.h"

namespace
{
	constexpr uint32 ReceiveThreadStackSize = 128 * 1024;
	constexpr double StatsPublishIntervalSeconds = 0.25;
}

FMOVINOscUdpReceiveThread::FMOVINOscUdpReceiveThread(
	FSocket* InSocket,
	const FMOVINOscReceiveThreadTuning& InTuning,
	const TCHAR* InThreadName)
	: Socket(InSocket)
	, ThreadName(InThreadName)
	, Tuning(InTuning)
{
	check(Socket != nullptr);
	check(Socket->GetSocketType() == SOCKTYPE_Datagram);

	Tuning.MaxDatagramBytes = FMath::Clamp(Tuning.MaxDatagramBytes, 1024, 65536);
	Tuning.MaxDatagramsPerWait = FMath::Clamp(Tuning.MaxDatagramsPerWait, 1, 1024 * 1024);
	Tuning.WaitMilliseconds = FMath::Clamp(Tuning.WaitMilliseconds, 0.1, 100.0);

	SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	WaitTime = FTimespan::FromMilliseconds(Tuning.WaitMilliseconds);
}

FMOVINOscUdpReceiveThread::~FMOVINOscUdpReceiveThread()
{
	if (Thread != nullptr)
	{
		Thread->Kill(true);
		delete Thread;
		Thread = nullptr;
	}
}

FMOVINOscOnDatagram& FMOVINOscUdpReceiveThread::OnDatagram()
{
	check(Thread == nullptr);
	return DatagramDelegate;
}

void FMOVINOscUdpReceiveThread::Start()
{
	if (Thread != nullptr)
	{
		return;
	}

	Thread = FRunnableThread::Create(
		this,
		*ThreadName,
		ReceiveThreadStackSize,
		TPri_AboveNormal,
		FPlatformAffinity::GetPoolThreadMask());
}

FMOVINOscReceiveThreadStats FMOVINOscUdpReceiveThread::SnapshotStats() const
{
	FScopeLock Lock(&StatsLock);
	return PublishedStats;
}

bool FMOVINOscUdpReceiveThread::Init()
{
	ReceiveBuffer.SetNumUninitialized(Tuning.MaxDatagramBytes);
	NextStatsPublishTime = FPlatformTime::Seconds() + StatsPublishIntervalSeconds;
	return true;
}

uint32 FMOVINOscUdpReceiveThread::Run()
{
	while (!bStopping.load(std::memory_order_relaxed))
	{
		if (Socket->Wait(ESocketWaitConditions::WaitForRead, WaitTime))
		{
			DrainOnce();
		}

		PublishStats(false);
	}

	PublishStats(true);
	return 0;
}

void FMOVINOscUdpReceiveThread::Stop()
{
	bStopping.store(true, std::memory_order_relaxed);
}

void FMOVINOscUdpReceiveThread::Exit()
{
}

void FMOVINOscUdpReceiveThread::Tick()
{
	// -nothreading builds: drain without blocking.
	if (ReceiveBuffer.Num() != Tuning.MaxDatagramBytes)
	{
		ReceiveBuffer.SetNumUninitialized(Tuning.MaxDatagramBytes);
	}

	if (Socket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::Zero()))
	{
		DrainOnce();
	}

	PublishStats(false);
}

void FMOVINOscUdpReceiveThread::DrainOnce()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(MOVINOscUdpReceiveThread_Drain);

	uint8* const Buffer = ReceiveBuffer.GetData();
	const int32 BufferSize = ReceiveBuffer.Num();

	int32 Budget = Tuning.MaxDatagramsPerWait;
	bool bReceivedAny = false;

	while (Budget > 0)
	{
		int32 BytesRead = 0;

		// No HasPendingData() call. For a datagram socket FSocketBSD::Recv short circuits its
		// error translation (SocketsBSD.cpp:342), so a false return simply means "nothing more to
		// read" - either EWOULDBLOCK or a genuine error, distinguished below. That costs one
		// syscall per datagram instead of the three HasPendingData() would add.
		if (!Socket->Recv(Buffer, BufferSize, BytesRead, ESocketReceiveFlags::None))
		{
			const ESocketErrors Error = SocketSubsystem != nullptr
				? SocketSubsystem->GetLastErrorCode()
				: SE_NO_ERROR;

			if (Error != SE_EWOULDBLOCK && Error != SE_NO_ERROR)
			{
				// Count and keep going. Windows can surface a spurious WSAECONNRESET from an ICMP
				// port unreachable even on a receive only socket, and that must not kill the thread.
				++Stats.RecvErrors;
			}
			break;
		}

		--Budget;
		bReceivedAny = true;

		if (BytesRead <= 0)
		{
			// A zero length datagram is legal but carries nothing to dispatch.
			++Stats.ZeroLengthDatagrams;
			continue;
		}

		++Stats.Datagrams;
		Stats.Bytes += static_cast<uint64>(BytesRead);
		DatagramDelegate.ExecuteIfBound(Buffer, BytesRead);
	}

	// Heuristic: exhausting the budget usually means the queue is still backing up. It can also
	// fire when the queue happened to hold exactly MaxDatagramsPerWait datagrams.
	if (Budget == 0)
	{
		++Stats.DrainOverruns;
	}

	if (bReceivedAny)
	{
		// Once per burst rather than once per datagram - FPlatformTime::Seconds() is a
		// QueryPerformanceCounter call and the packet rate makes that measurable.
		LastPacketTime.store(FPlatformTime::Seconds(), std::memory_order_relaxed);
	}
}

void FMOVINOscUdpReceiveThread::PublishStats(bool bForce)
{
	const double Now = FPlatformTime::Seconds();
	if (!bForce && Now < NextStatsPublishTime)
	{
		return;
	}

	NextStatsPublishTime = Now + StatsPublishIntervalSeconds;

	FScopeLock Lock(&StatsLock);
	PublishedStats = Stats;
}
