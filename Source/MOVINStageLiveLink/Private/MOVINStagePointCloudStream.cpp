#include "MOVINStagePointCloudStream.h"

FMOVINStagePointCloudStream::FMOVINStagePointCloudStream(int32 InMaxPoints)
	: MaxPoints(FMath::Max(InMaxPoints, 1))
{
	// Reserve up front so steady state never allocates on the receive thread.
	for (FSlot& Slot : Slots)
	{
		Slot.Frame.Points.Reserve(MaxPoints);
	}
}

int32 FMOVINStagePointCloudStream::IndexOfChecked(const FMOVINStagePointCloudFrame* Frame) const
{
	for (int32 Index = 0; Index < NumSlots; ++Index)
	{
		if (&Slots[Index].Frame == Frame)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

FMOVINStagePointCloudFrame* FMOVINStagePointCloudStream::AcquireWriteSlot()
{
	FScopeLock ScopeLock(&Lock);

	for (FSlot& Slot : Slots)
	{
		if (Slot.State == ESlotState::Free)
		{
			Slot.State = ESlotState::Writing;
			return &Slot.Frame;
		}
	}

	// Nothing free: recycle the oldest ready frame. It is about to be superseded anyway, so this is
	// strictly better than dropping the incoming frame.
	FSlot* Oldest = nullptr;
	for (FSlot& Slot : Slots)
	{
		if (Slot.State != ESlotState::Ready)
		{
			continue;
		}
		if (Oldest == nullptr || Slot.PublishOrder < Oldest->PublishOrder)
		{
			Oldest = &Slot;
		}
	}

	if (Oldest != nullptr)
	{
		++FramesDropped;
		Oldest->State = ESlotState::Writing;
		return &Oldest->Frame;
	}

	// Every slot is held downstream. Drop the frame rather than block the receive thread.
	++WriteSlotStarvations;
	return nullptr;
}

void FMOVINStagePointCloudStream::PublishWriteSlot(FMOVINStagePointCloudFrame* Frame)
{
	FScopeLock ScopeLock(&Lock);

	const int32 Index = IndexOfChecked(Frame);
	if (Index == INDEX_NONE || Slots[Index].State != ESlotState::Writing)
	{
		return;
	}

	// Only the newest frame is worth showing; recycle anything older that was never consumed.
	for (int32 Other = 0; Other < NumSlots; ++Other)
	{
		if (Other != Index && Slots[Other].State == ESlotState::Ready)
		{
			Slots[Other].State = ESlotState::Free;
			++FramesDropped;
		}
	}

	Slots[Index].State = ESlotState::Ready;
	Slots[Index].PublishOrder = NextPublishOrder++;
	++FramesPublished;
}

void FMOVINStagePointCloudStream::AbandonWriteSlot(FMOVINStagePointCloudFrame* Frame)
{
	FScopeLock ScopeLock(&Lock);

	const int32 Index = IndexOfChecked(Frame);
	if (Index != INDEX_NONE && Slots[Index].State == ESlotState::Writing)
	{
		Slots[Index].State = ESlotState::Free;
	}
}

FMOVINStagePointCloudFrame* FMOVINStagePointCloudStream::AcquireLatestReadyFrame()
{
	FScopeLock ScopeLock(&Lock);

	FSlot* Newest = nullptr;
	for (FSlot& Slot : Slots)
	{
		if (Slot.State != ESlotState::Ready)
		{
			continue;
		}
		if (Newest == nullptr || Slot.PublishOrder > Newest->PublishOrder)
		{
			Newest = &Slot;
		}
	}

	if (Newest == nullptr)
	{
		return nullptr;
	}

	// Any remaining ready frame is older than the one we are about to hand out.
	for (FSlot& Slot : Slots)
	{
		if (&Slot != Newest && Slot.State == ESlotState::Ready)
		{
			Slot.State = ESlotState::Free;
			++FramesDropped;
		}
	}

	Newest->State = ESlotState::InUse;
	++FramesConsumed;
	return &Newest->Frame;
}

void FMOVINStagePointCloudStream::ReleaseFrame(FMOVINStagePointCloudFrame* Frame)
{
	FScopeLock ScopeLock(&Lock);

	const int32 Index = IndexOfChecked(Frame);
	if (Index != INDEX_NONE && Slots[Index].State == ESlotState::InUse)
	{
		Slots[Index].State = ESlotState::Free;
	}
}

uint64 FMOVINStagePointCloudStream::GetFramesPublished() const
{
	FScopeLock ScopeLock(&Lock);
	return FramesPublished;
}

uint64 FMOVINStagePointCloudStream::GetFramesConsumed() const
{
	FScopeLock ScopeLock(&Lock);
	return FramesConsumed;
}

uint64 FMOVINStagePointCloudStream::GetFramesDroppedBeforeDisplay() const
{
	FScopeLock ScopeLock(&Lock);
	return FramesDropped;
}

uint64 FMOVINStagePointCloudStream::GetWriteSlotStarvations() const
{
	FScopeLock ScopeLock(&Lock);
	return WriteSlotStarvations;
}
