#ifndef __lib_gdi_sdl_h
#define __lib_gdi_sdl_h

#include <lib/base/thread.h>
#include <lib/gdi/gmaindc.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_video.h>
#include <gst/gst.h>

struct FrameInfo
{
    int w,h;
    FrameInfo(int width, int height): w(width), h(height) { }
};

class gSDLDC: public gMainDC, public eThread, public sigc::trackable
{
private:
	void exec(const gOpcode *opcode);

	gUnmanagedSurface m_surface;

	eFixedMessagePump<SDL_Event> m_pump;
	void keyEvent(const SDL_Event &event);
	void pumpEvent(const SDL_Event &event);
	virtual void thread();

	enum event {
		EV_SET_VIDEO_MODE,
		EV_FLIP,
		EV_QUIT,
	};

	void pushEvent(enum event code, void *data1 = 0, void *data2 = 0);
	void evSetVideoMode(unsigned long xres, unsigned long yres);
	void evFlip();
	void cleanup();

	SDL_Surface *m_screen;
	SDL_Window *m_window;
	SDL_Renderer *m_render;
	SDL_Texture *m_video_tex;
	SDL_Texture *m_osd_tex;
	SDL_Surface *m_osd;
	FrameInfo m_frame;
	eSingleLock m_mutex;
	GstSample *m_buf;

public:
	void setResolution(int xres, int yres, int bpp = 32);
	gSDLDC();
	virtual ~gSDLDC();
	int islocked() const { return 0; }
	/// Passing ownership of buf
	void displayVideoFrame(GstSample *buf);
};

#endif
