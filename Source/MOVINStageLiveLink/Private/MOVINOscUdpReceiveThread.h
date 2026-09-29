#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "Misc/SingleThreadRunnable.h"

#include <atomic>

class FRunnableThread;
class FSocket;
class ISocketSubsystem;

/**
 * Delegate for one received datagram. The buffer is owned by the receive thread and is only valid
 * for the duration of the call, which is what lets the receiver avoid a per-packet allocation.
 */
DECLARE_DELEGATE_TwoParams(FMOVINOscOnDatagram, const uint8* /*Data*/, int32 /*NumBytes*/);

struct FMOVINOscReceiveThreadStats
{
	uint64 Datagrams = 0;
	uint64 Bytes = 0;
	uint64 DrainOverruns = 0;
	uint64 RecvErrors = 0;
	uint64 ZeroLengthDatagrams = 0;
};

struct FMOVINOscReceiveThreadTuning
{
	/** Reusable receive buffer size. The maximum UDP payload is 65507 bytes. */
	int32 MaxDatagramBytes = 65536;

	/** Datagrams consumed per readable notification. Must cover one point cloud frame burst. */
	int32 MaxDatagramsPerWait = 8192;

	double WaitMilliseconds = 1.0;
};

/**
 * UDP receive thread with no per-datagram allocation.
 *
 * Replaces FUdpSocketReceiver, which allocates an FArrayReader plus its storage for every
 * datagram (UdpSocketReceiver.h:152-153) and calls HasPendingData() first, costing three
 * syscalls per packet. At tens of thousands of datagrams per second that allocation traffic
 * becomes the bottleneck, especially because Windows reports the whole queue depth through
 * FIONREAD so the allocation tends to be sized at the full read buffer.
 *
 * This receiver keeps one preallocated buffer, drains with Recv() until it reports the queue is
 * empty, and hands the callee a raw pointer.
 */
class FMOVINOscUdpReceiveThread : public FRunnable, private FSingleThreadRunnable
{
public:
	FMOVINOscUdpReceiveThread(FSocket* InSocket, const FMOVINOscReceiveThreadTuning& InTuning, const TCHAR* InThreadName);
	virtual ~FMOVINOscUdpReceiveThread() override;

	/** Must be bound before Start(). Cannot be rebound while the thread runs. */
	FMOVINOscOnDatagram& OnDatagram();

	void Start();

	/** FPlatformTime::Seconds() of the most recent drain burst that saw traffic. */
	double GetLastPacketTime() const { return LastPacketTime.load(std::memory_order_relaxed); }

	FMOVINOscReceiveThreadStats SnapshotStats() const;

	//~ FRunnable interface
	virtual bool Init() override;
	virtual uint32 Run() override;
	virtual void Stop() override;
	virtual void Exit() override;
	virtual FSingleThreadRunnable* GetSingleThreadInterface() override { return this; }

private:
	//~ FSingleThreadRunnable interface
	virtual void Tick() override;

	void DrainOnce();
	void PublishStats(bool bForce);

	FSocket* Socket = nullptr;
	ISocketSubsystem* SocketSubsystem = nullptr;
	FRunnableThread* Thread = nullptr;
	FString ThreadName;
	FMOVINOscReceiveThreadTuning Tuning;
	FTimespan WaitTime;

	TArray<uint8> ReceiveBuffer;
	FMOVINOscOnDatagram DatagramDelegate;

	/** Receive thread owned and unsynchronized. Ten relaxed atomics per packet would be waste. */
	FMOVINOscReceiveThreadStats Stats;
	double NextStatsPublishTime = 0.0;

	mutable FCriticalSection StatsLock;
	FMOVINOscReceiveThreadStats PublishedStats;

	std::atomic<double> LastPacketTime{ 0.0 };
	std::atomic<bool> bStopping{ false };
};
