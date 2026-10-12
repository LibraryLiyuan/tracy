#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <backends/imgui_impl_opengl3_loader.h>

#include <algorithm>
#include <chrono>
#include <GLFW/glfw3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <thread>
#include <vector>
#include <filesystem>
#include <fstream>

#include "profiler/TracyConfig.hpp"
#include "profiler/TracyImGui.hpp"

#include "Backend.hpp"
#include "RunQueue.hpp"

#ifdef __APPLE__
#include <objc/objc.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include "icon.hpp"
#endif


static GLFWwindow* s_window;
static std::function<void()> s_redraw;
static std::function<void(float)> s_scaleChanged;
static RunQueue* s_mainThreadTasks;
static WindowPosition* s_winPos;
static bool s_iconified;
static float s_prevScale = -1;
#if defined( _WIN32 ) && defined( TRACY_STREAM_VIEW )
#  include <Windows.h>
static std::chrono::steady_clock::time_point s_streamFrameStart;
static double s_streamFrameWork = 0;
static double s_streamFrameUi = 0, s_streamFrameRender = 0;
static bool s_streamFrameTimed = false;
static double s_streamCpuStart = -1, s_streamFrameCpu = -1;
static bool s_streamAdaptivePresentation = false;
static double s_streamFramePresent = 0;
static bool s_streamWeb = false;
void ConfigureStreamingPresentation( bool active, bool unpaced )
{
    static int interval = 1;
    const int requested = active ? 0 : 1;
    if( requested != interval )
    {
        glfwSwapInterval( requested );
        interval = requested;
    }
    s_streamAdaptivePresentation = active && !unpaced;
}
static double StreamingThreadCpuMilliseconds()
{
    FILETIME created, exited, kernel, user;
    if( !GetThreadTimes( GetCurrentThread(), &created, &exited, &kernel, &user ) ) return -1;
    ULARGE_INTEGER k, u;
    k.LowPart = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;
    return double( k.QuadPart + u.QuadPart ) / 10000.;
}
// A screenshot of the next rendered frame, for checking the long-capture
// views without a person at the screen. Written as an uncompressed PNG.
static std::string s_streamScreenshot;
void RequestStreamingScreenshot( const char* path ) { s_streamScreenshot = path; }
static void WriteStreamingScreenshot( int w, int h )
{
    std::vector<uint8_t> pixels( size_t( w ) * h * 4 );
    glPixelStorei( GL_PACK_ALIGNMENT, 1 );
    glReadPixels( 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
    std::vector<uint8_t> raw;
    raw.reserve( size_t( w * 3 + 1 ) * h );
    for( int y = h - 1; y >= 0; --y )
    {
        raw.push_back( 0 );
        for( int x = 0; x < w; ++x )
        {
            const auto* p = &pixels[( size_t( y ) * w + x ) * 4];
            raw.insert( raw.end(), p, p + 3 );
        }
    }
    uint32_t table[256];
    for( uint32_t n = 0; n < 256; ++n )
    {
        uint32_t c = n;
        for( int k = 0; k < 8; ++k ) c = c & 1 ? 0xEDB88320u ^ ( c >> 1 ) : c >> 1;
        table[n] = c;
    }
    const auto crc = [&]( const uint8_t* data, size_t size, uint32_t c ) {
        for( size_t n = 0; n < size; ++n ) c = table[( c ^ data[n] ) & 0xFF] ^ ( c >> 8 );
        return c;
    };
    std::vector<uint8_t> zlib = { 0x78, 0x01 };
    uint32_t a = 1, b = 0;
    for( size_t offset = 0; offset < raw.size(); )
    {
        const auto size = std::min<size_t>( 65535, raw.size() - offset );
        zlib.push_back( offset + size == raw.size() ? 1 : 0 );
        zlib.push_back( uint8_t( size ) );
        zlib.push_back( uint8_t( size >> 8 ) );
        zlib.push_back( uint8_t( ~size ) );
        zlib.push_back( uint8_t( ~size >> 8 ) );
        zlib.insert( zlib.end(), raw.begin() + offset, raw.begin() + offset + size );
        for( size_t n = offset; n < offset + size; ++n )
        {
            a = ( a + raw[n] ) % 65521;
            b = ( b + a ) % 65521;
        }
        offset += size;
    }
    const uint32_t adler = ( b << 16 ) | a;
    for( int shift = 24; shift >= 0; shift -= 8 ) zlib.push_back( uint8_t( adler >> shift ) );
    FILE* file = fopen( s_streamScreenshot.c_str(), "wb" );
    if( !file ) return;
    const auto chunk = [&]( const char* type, const std::vector<uint8_t>& data ) {
        const uint8_t length[4] = { uint8_t( data.size() >> 24 ), uint8_t( data.size() >> 16 ), uint8_t( data.size() >> 8 ), uint8_t( data.size() ) };
        fwrite( length, 1, 4, file );
        fwrite( type, 1, 4, file );
        fwrite( data.data(), 1, data.size(), file );
        auto c = crc( reinterpret_cast<const uint8_t*>( type ), 4, 0xFFFFFFFFu );
        c = crc( data.data(), data.size(), c ) ^ 0xFFFFFFFFu;
        const uint8_t sum[4] = { uint8_t( c >> 24 ), uint8_t( c >> 16 ), uint8_t( c >> 8 ), uint8_t( c ) };
        fwrite( sum, 1, 4, file );
    };
    static const uint8_t signature[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    fwrite( signature, 1, 8, file );
    chunk( "IHDR", { uint8_t( w >> 24 ), uint8_t( w >> 16 ), uint8_t( w >> 8 ), uint8_t( w ), uint8_t( h >> 24 ), uint8_t( h >> 16 ), uint8_t( h >> 8 ), uint8_t( h ), 8, 2, 0, 0, 0 } );
    chunk( "IDAT", zlib );
    chunk( "IEND", {} );
    fclose( file );
}
void BeginStreamingFrameTiming( bool unpaced )
{
    (void)unpaced;
    s_streamFrameStart = std::chrono::steady_clock::now();
    s_streamCpuStart = StreamingThreadCpuMilliseconds();
    s_streamFrameTimed = true;
}
double StreamingFrameWorkMilliseconds() { return s_streamFrameWork; }
double StreamingFrameUiMilliseconds() { return s_streamFrameUi; }
double StreamingFrameRenderMilliseconds() { return s_streamFrameRender; }
double StreamingFramePresentMilliseconds() { return s_streamFramePresent; }
double StreamingFrameCpuMilliseconds() { return s_streamFrameCpu; }
#include "StreamingWebFrame.inl"
#endif

#ifdef __APPLE__
typedef long NSInteger;
typedef unsigned long NSUInteger;

namespace
{

static void EnsureMacAppRegistration()
{
    static bool initialized = false;
    if( initialized ) return;
    initialized = true;

    id pool = ((id (*)(Class, SEL))objc_msgSend)((Class)objc_getClass("NSAutoreleasePool"), sel_getUid("alloc"));
    pool = ((id (*)(id, SEL))objc_msgSend)(pool, sel_getUid("init"));

    id app = ((id (*)(Class, SEL))objc_msgSend)((Class)objc_getClass("NSApplication"), sel_getUid("sharedApplication"));
    ((void (*)(id, SEL, NSInteger))objc_msgSend)(app, sel_getUid("setActivationPolicy:"), (NSInteger)0);
    ((void (*)(id, SEL, BOOL))objc_msgSend)(app, sel_getUid("activateIgnoringOtherApps:"), (BOOL)1);

    ((void (*)(id, SEL))objc_msgSend)(pool, sel_getUid("release"));
}

static void SetMacAppIcon()
{
    id pool = ((id (*)(Class, SEL))objc_msgSend)((Class)objc_getClass("NSAutoreleasePool"), sel_getUid("alloc"));
    pool = ((id (*)(id, SEL))objc_msgSend)(pool, sel_getUid("init"));

    id data = ((id (*)(Class, SEL, const void*, NSUInteger))objc_msgSend)((Class)objc_getClass("NSData"), sel_getUid("dataWithBytes:length:"), (const void*)Icon_data, (NSUInteger)Icon_size);
    id image = ((id (*)(Class, SEL))objc_msgSend)((Class)objc_getClass("NSImage"), sel_getUid("alloc"));
    image = ((id (*)(id, SEL, id))objc_msgSend)(image, sel_getUid("initWithData:"), data);
    if( image )
    {
        id app = ((id (*)(Class, SEL))objc_msgSend)((Class)objc_getClass("NSApplication"), sel_getUid("sharedApplication"));
        ((void (*)(id, SEL, id))objc_msgSend)(app, sel_getUid("setApplicationIconImage:"), image);
        ((void (*)(id, SEL))objc_msgSend)(image, sel_getUid("release"));
    }

    ((void (*)(id, SEL))objc_msgSend)(pool, sel_getUid("release"));
}

}
#endif


static void glfw_error_callback( int error, const char* description )
{
    fprintf(stderr, "Error %d: %s\n", error, description);
}

static void glfw_window_pos_callback( GLFWwindow* window, int x, int y )
{
    if( !glfwGetWindowAttrib( window, GLFW_MAXIMIZED ) )
    {
        s_winPos->x = x;
        s_winPos->y = y;
    }
}

static void glfw_window_size_callback( GLFWwindow* window, int w, int h )
{
    if( !glfwGetWindowAttrib( window, GLFW_MAXIMIZED ) )
    {
        s_winPos->w = w;
        s_winPos->h = h;
    }
    tracy::s_wasActive = true;
}

static void glfw_window_maximize_callback( GLFWwindow*, int maximized )
{
    s_winPos->maximize = maximized;
}

static void glfw_window_iconify_callback( GLFWwindow*, int iconified )
{
    s_iconified = iconified != 0;
}


Backend::Backend( const char* title, const std::function<void()>& redraw, const std::function<void(float)>& scaleChanged, const std::function<int(void)>& isBusy, RunQueue* mainThreadTasks )
{
    glfwSetErrorCallback( glfw_error_callback );
    if( !glfwInit() ) exit( 1 );
#ifdef DISPLAY_SERVER_WAYLAND
    glfwWindowHint( GLFW_ALPHA_BITS, 0 );
#else
    glfwWindowHint( GLFW_VISIBLE, 0 );
#endif
    glfwWindowHint( GLFW_CONTEXT_VERSION_MAJOR, 3 );
    glfwWindowHint( GLFW_CONTEXT_VERSION_MINOR, 2 );
    glfwWindowHint( GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE );
#ifdef __APPLE__
    glfwWindowHint( GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE );
#endif
#ifdef WIN32
#  if GLFW_VERSION_MAJOR > 3 || ( GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 4 )
    glfwWindowHint( GLFW_WIN32_KEYBOARD_MENU, 1 );
#  endif
#  if GLFW_VERSION_MAJOR > 3 || ( GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 3 )
    glfwWindowHint( GLFW_SCALE_TO_MONITOR, 1 );
#  endif
#endif
    s_window = glfwCreateWindow( m_winPos.w, m_winPos.h, title, NULL, NULL );
    if( !s_window ) exit( 1 );

    glfwSetWindowPos( s_window, m_winPos.x, m_winPos.y );
#if GLFW_VERSION_MAJOR > 3 || ( GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 2 )
    if( m_winPos.maximize ) glfwMaximizeWindow( s_window );
#endif

    glfwMakeContextCurrent( s_window );
    glfwSwapInterval( 1 ); // Enable vsync
    glfwSetWindowRefreshCallback( s_window, []( GLFWwindow* ) { tracy::s_wasActive = true; s_redraw(); } );

    ImGui_ImplGlfw_InitForOpenGL( s_window, true );
    ImGui_ImplOpenGL3_Init( "#version 150" );

    s_redraw = redraw;
    s_scaleChanged = scaleChanged;
    s_mainThreadTasks = mainThreadTasks;
    s_winPos = &m_winPos;
    s_iconified = false;

    glfwSetWindowPosCallback( s_window, glfw_window_pos_callback );
    glfwSetWindowSizeCallback( s_window, glfw_window_size_callback );
#if GLFW_VERSION_MAJOR > 3 || ( GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 3 )
    glfwSetWindowMaximizeCallback( s_window, glfw_window_maximize_callback );
#endif
    glfwSetWindowIconifyCallback( s_window, glfw_window_iconify_callback );

#ifdef __APPLE__
    EnsureMacAppRegistration();
#endif
}

Backend::~Backend()
{
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();

    glfwDestroyWindow( s_window );

    glfwTerminate();
}

void Backend::Show()
{
    glfwShowWindow( s_window );
}

#ifdef _WIN32
void Backend::RequestClose(){glfwSetWindowShouldClose(s_window,GLFW_TRUE);}
#endif
void Backend::Run()
{
    while( !glfwWindowShouldClose( s_window ) )
    {
        if( s_iconified
#if defined(_WIN32) && defined(TRACY_STREAM_VIEW)
            && !s_streamWeb
#endif
            )
        {
            glfwWaitEvents();
        }
        else
        {
#if defined(_WIN32) && defined(TRACY_STREAM_VIEW)
            const auto frameStart = std::chrono::steady_clock::now();
#endif
            glfwPollEvents();
            s_redraw();
            if( tracy::s_config.focusLostLimit && !glfwGetWindowAttrib( s_window, GLFW_FOCUSED )
#if defined(_WIN32) && defined(TRACY_STREAM_VIEW)
                && !s_streamWeb
#endif
                ) std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
            s_mainThreadTasks->Run();
#if defined( _WIN32 ) && defined( TRACY_STREAM_VIEW )
            if( s_streamAdaptivePresentation && !glfwWindowShouldClose( s_window ) )
            {
                const auto remaining = 1. / 60. - std::chrono::duration<double>( std::chrono::steady_clock::now() - frameStart ).count();
                if( remaining > 0 ) glfwWaitEventsTimeout( remaining );
            }
#endif
        }
    }
}

void Backend::Attention()
{
#if GLFW_VERSION_MAJOR > 3 || ( GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 3 )
    if( !glfwGetWindowAttrib( s_window, GLFW_FOCUSED ) )
    {
        glfwRequestWindowAttention( s_window );
    }
#endif
}

void Backend::NewFrame( int& w, int& h )
{
    const auto scale =
#if defined(_WIN32) && defined(TRACY_STREAM_VIEW)
        s_streamWeb ? 1.f :
#endif
        GetDpiScale();
    if( scale != s_prevScale )
    {
        s_prevScale = scale;
        s_scaleChanged( scale );
    }

    glfwGetFramebufferSize( s_window, &w, &h );
#if defined( __APPLE__ )
    w = static_cast<int>( w / scale );
    h = static_cast<int>( h / scale );
#endif
    m_w = w;
    m_h = h;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
#if defined(_WIN32) && defined(TRACY_STREAM_VIEW)
    if(s_streamWeb)
    {
        // Desktop maximize/DPI/monitor changes cannot resize the web transport.
        w=m_w=StreamingWebWidth;h=m_h=StreamingWebHeight;
        ImGui::GetIO().DisplaySize=ImVec2(float(w),float(h));
        ImGui::GetIO().DisplayFramebufferScale=ImVec2(1,1);
    }
#endif
}

void Backend::EndFrame()
{
#if defined(_WIN32) && defined(TRACY_STREAM_VIEW)
    const auto renderStart = std::chrono::steady_clock::now();
    if( s_streamFrameTimed ) s_streamFrameUi = std::chrono::duration<double, std::milli>( renderStart - s_streamFrameStart ).count();
#endif
    const ImVec4 clear_color = ImColor( 20, 20, 17 );

    ImGui::Render();
#if defined(_WIN32) && defined(TRACY_STREAM_VIEW)
    if(s_streamWeb)s_bindWebFramebuffer(WebFramebuffer,s_webFramebuffer);
#endif
    glViewport( 0, 0, m_w, m_h );
    glClearColor( clear_color.x, clear_color.y, clear_color.z, clear_color.w );
    glClear( GL_COLOR_BUFFER_BIT );
    ImGui_ImplOpenGL3_RenderDrawData( ImGui::GetDrawData() );

#if defined( _WIN32 ) && defined( TRACY_STREAM_VIEW )
    PublishStreamingWebFrame(m_w,m_h);
    if( !s_streamScreenshot.empty() )
    {
        WriteStreamingScreenshot( m_w, m_h );
        s_streamScreenshot.clear();
    }
    const auto presentStart = std::chrono::steady_clock::now();
    if(s_streamWeb)s_bindWebFramebuffer(WebFramebuffer,0);
#endif
    glfwSwapBuffers( s_window );
#if defined( _WIN32 ) && defined( TRACY_STREAM_VIEW )
    if( s_streamFrameTimed )
    {
        const auto finished = std::chrono::steady_clock::now();
        const auto cpu = StreamingThreadCpuMilliseconds();
        s_streamFrameCpu = ( cpu >= 0 && s_streamCpuStart >= 0 ) ? cpu - s_streamCpuStart : -1;
        s_streamFrameWork = std::chrono::duration<double, std::milli>( finished - s_streamFrameStart ).count();
        s_streamFrameRender = std::chrono::duration<double, std::milli>( presentStart - renderStart ).count();
        s_streamFramePresent = std::chrono::duration<double, std::milli>( finished - presentStart ).count();
        s_streamFrameTimed = false;
    }
#endif
}

void Backend::SetIcon( uint8_t* data, int w, int h )
{
#ifdef __APPLE__
    EnsureMacAppRegistration();
    SetMacAppIcon();
#else
    GLFWimage icon;
    icon.width = w;
    icon.height = h;
    icon.pixels = data;
    glfwSetWindowIcon( s_window, 1, &icon );
#endif
}

void Backend::SetTitle( const char* title )
{
    glfwSetWindowTitle( s_window, title );
}

float Backend::GetDpiScale()
{
#if GLFW_VERSION_MAJOR > 3 || ( GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 3 )
    float x, y;
    glfwGetWindowContentScale( s_window, &x, &y );
    return x;
#else
    return 1;
#endif
}
