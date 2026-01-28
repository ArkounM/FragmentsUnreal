#pragma once

/**
 * DEPRECATED: This file was replaced by TessellationTask.h
 *
 * The original FRunnable-based worker pool (FFragmentGeometryWorker, FGeometryWorkerPool)
 * has been replaced with FAsyncTask<FTessellationTask> pattern.
 *
 * The TQueue<T, EQueueMode::Mpsc> approach had issues with Unreal types like FString and TArray
 * due to thread-local memory allocators. FAsyncTask is Unreal's recommended pattern for
 * background work and handles thread ownership correctly.
 *
 * See: TessellationTask.h for the new implementation
 * See: FragmentsImporter.h for SubmitTessellationTask() and ProcessCompletedTessellation()
 */

// This file is kept for reference but should not be used.
// All geometry processing functionality has moved to:
// - TessellationTask.h/cpp (FTessellationTask class)
// - FragmentsImporter.h/cpp (SubmitTessellationTask, ProcessCompletedTessellation)
