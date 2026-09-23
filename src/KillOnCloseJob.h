#pragma once

#include <windows.h>

// A job object that terminates every process in it when its last handle
// closes, or null. Every child this player starts - ffmpeg, ffprobe, yt-dlp,
// NeuralWorker - is put in one before it is resumed, so a player that crashes
// or is killed cannot leave a helper holding a GPU, a pipe or a file.
//
// Seven copies of these eight lines existed (MediaPipeline and NeuralWorker
// each had a named one, AudioPlayer, VideoDecoder and YouTubeResolver inlined
// theirs); they agreed, but a limit added to one would silently not reach the
// others.
inline HANDLE CreateKillOnCloseJob()
{
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return nullptr;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        CloseHandle(job);
        return nullptr;
    }
    return job;
}
