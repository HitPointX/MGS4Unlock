#pragma once

namespace mgs4
{
    // Normalises the camera turn rate above 60 fps.
    //
    // The camera settles towards where the stick points by a fixed fraction per
    // call, with no frame time involved, so it converges sooner the more often
    // it runs. Most obvious where the camera moves slowly and deliberately, such
    // as crawling through a duct.
    //
    // Takes the frame timing struct the cutscene fix already resolves.
    bool InstallCameraTiming(const void* frameTimingStruct);

    void LogCameraCounters(double intervalSeconds);
} // namespace mgs4
