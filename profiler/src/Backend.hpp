#ifndef __BACKEND_HPP__
#define __BACKEND_HPP__

#include <functional>
#include <stdint.h>

#include "WindowPosition.hpp"
#if defined(_WIN32) && defined(TRACY_STREAM_VIEW)
void BeginStreamingFrameTiming(bool unpaced);
void ConfigureStreamingPresentation(bool active,bool unpaced=false);
double StreamingFrameWorkMilliseconds();
double StreamingFrameUiMilliseconds();
double StreamingFrameRenderMilliseconds();
double StreamingFramePresentMilliseconds();
double StreamingFrameCpuMilliseconds();
// Saves the next rendered frame as a PNG at `path`.
void RequestStreamingScreenshot(const char* path);
void ConfigureStreamingWeb(const char* channel);
void UpdateStreamingWebState(uint64_t input,uint64_t navigation,int64_t begin,int64_t end,uint64_t selected,uint32_t flags,uint64_t queued);
#endif

class RunQueue;

class Backend
{
public:
    Backend( const char* title, const std::function<void()>& redraw, const std::function<void(float)>& scaleChanged, const std::function<int(void)>& isBusy, RunQueue* mainThreadTasks );
    ~Backend();

    void Show();
    void Run();
#ifdef _WIN32
    void RequestClose();
#endif
    void Attention();

    void NewFrame( int& w, int& h );
    void EndFrame();

    void SetIcon( uint8_t* data, int w, int h );
    void SetTitle( const char* title );

    float GetDpiScale();

private:
    WindowPosition m_winPos;
    int m_w, m_h;
};

#endif
