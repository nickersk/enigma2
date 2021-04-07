#include <lib/gdi/sdl2.h>
#include <lib/actions/action.h>
#include <lib/base/init.h>
#include <lib/base/init_num.h>
#include <lib/driver/input_fake.h>
#include <lib/driver/rcsdl2.h>

gSDLDC::gSDLDC() : m_pump(eApp, 1)
	,m_window(NULL)
	,m_video_tex(NULL)
	,m_osd_tex(NULL)
	,m_frame(0,0)
	,m_buf(NULL)
{
	if (SDL_Init(SDL_INIT_VIDEO) < 0) {
		eWarning("[gSDLDC] Could not initialize SDL: %s", SDL_GetError());
		return;
	}

//	setResolution(720, 576);

	CONNECT(m_pump.recv_msg, gSDLDC::pumpEvent);

	m_surface.clut.colors = 256;
	m_surface.clut.data = new gRGB[m_surface.clut.colors];

	m_pixmap = new gPixmap(&m_surface);

	memset(m_surface.clut.data, 0, sizeof(*m_surface.clut.data)*m_surface.clut.colors);

	run();
}

gSDLDC::~gSDLDC()
{
	pushEvent(EV_QUIT);
	kill();
	SDL_Quit();
}

void gSDLDC::keyEvent(const SDL_Event &event)
{
	eSDLInputDriver *driver = eSDLInputDriver::getInstance();

	eDebug("[gSDLDC] Key %s: key=%d", (event.type == SDL_KEYDOWN) ? "Down" : "Up", event.key.keysym.sym);

	if (driver)
		driver->keyPressed(&event.key);
}

void gSDLDC::pumpEvent(const SDL_Event &event)
{
	switch (event.type) {
	case SDL_KEYDOWN:
	case SDL_KEYUP:
		keyEvent(event);
		break;
	case SDL_QUIT:
		eDebug("[gSDLDC] Quit");
		extern void quitMainloop(int exit_code);
		quitMainloop(0);
		break;
	}
}

void gSDLDC::pushEvent(enum event code, void *data1, void *data2)
{
	SDL_Event event;

	event.type = SDL_USEREVENT;
	event.user.code = code;
	event.user.data1 = data1;
	event.user.data2 = data2;

	SDL_PushEvent(&event);
}

void gSDLDC::exec(const gOpcode *o)
{
	switch (o->opcode) {
	case gOpcode::flush:
		pushEvent(EV_FLIP);
//		eDebug("[gSDLDC] FLUSH");
		break;
	default:
		gDC::exec(o);
		break;
	}
}

void gSDLDC::setResolution(int xres, int yres, int bpp)
{
	pushEvent(EV_SET_VIDEO_MODE, (void *)xres, (void *)yres);
}

void gSDLDC::displayVideoFrame(GstSample *buf)
{
	m_mutex.lock();
	if (m_buf != NULL) {
		eDebug("[gSDLDC] drop frame");
		gst_sample_unref(m_buf);
		m_buf = NULL;
	}
	m_buf = buf;
	m_mutex.unlock();
	pushEvent(EV_FLIP);
}

/*
 * SDL thread below...
 */

void gSDLDC::evSetVideoMode(unsigned long xres, unsigned long yres)
{
	m_window = SDL_CreateWindow("enigma2-SDL2", 0, 0, xres, yres, SDL_WINDOW_RESIZABLE);
	if (!m_window) {
		eFatal("[gSDLDC] Could not create SDL window: %s", SDL_GetError());
		return;
	}
	m_render = SDL_CreateRenderer(m_window, -1, /*SDL_RENDERER_SOFTWARE*/ SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if (!m_render) {
		eFatal("[gSDLDC] Could not create SDL renderer: %s", SDL_GetError());
		return;
	}
	m_osd = SDL_CreateRGBSurface(SDL_SWSURFACE, xres, yres, 32, 0, 0, 0, 0);
	SDL_SetColorKey(m_osd, SDL_TRUE, SDL_MapRGB(m_osd->format, 0, 0, 0));
	m_osd_tex = SDL_CreateTexture(m_render, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, xres, yres);
	SDL_SetTextureBlendMode(m_osd_tex, SDL_BLENDMODE_BLEND);

	m_surface.x = m_osd->w;
	m_surface.y = m_osd->h;
	m_surface.bpp = m_osd->format->BitsPerPixel;
	m_surface.bypp = m_osd->format->BytesPerPixel;
	m_surface.stride = m_osd->pitch;
	m_surface.data = m_osd->pixels;

//	SDL_EnableUNICODE(1);
}

#define I420_Y_ROWSTRIDE(width) (GST_ROUND_UP_4(width))
#define I420_U_ROWSTRIDE(width) (GST_ROUND_UP_8(width)/2)
#define I420_V_ROWSTRIDE(width) ((GST_ROUND_UP_8(I420_Y_ROWSTRIDE(width)))/2)

#define I420_Y_OFFSET(w,h) (0)
#define I420_U_OFFSET(w,h) (I420_Y_OFFSET(w,h)+(I420_Y_ROWSTRIDE(w)*GST_ROUND_UP_2(h)))
#define I420_V_OFFSET(w,h) (I420_U_OFFSET(w,h)+(I420_U_ROWSTRIDE(w)*GST_ROUND_UP_2(h)/2))

#define I420_SIZE(w,h)     (I420_V_OFFSET(w,h)+(I420_V_ROWSTRIDE(w)*GST_ROUND_UP_2(h)/2))


void gSDLDC::evFlip()
{
	if (!m_window)
		return;

	// Render and Texture operations only allowed in SDL2 thread.
	// Never manipulate textures from gstreamer thread!

	SDL_RenderClear(m_render);

	// Update Video texture;
	m_mutex.lock();
	GstSample *gst_buf = m_buf;
	m_buf = NULL;
	m_mutex.unlock();

	if (gst_buf) {
		GstCaps* caps = NULL;//gst_buffer_get_caps(gst_buf);
		if (!caps) {
			eFatal("could not get snapshot format");
		}
		gint width, height;
		GstStructure* s = gst_caps_get_structure(caps, 0);
		int res = gst_structure_get_int(s, "width", &width)
				| gst_structure_get_int(s, "height", &height);
		if (!res) {
			eFatal("could not get snapshot dimension\n");
		}
	//	eDebug("CAPS: %s", gst_caps_to_string(caps));

		if (m_frame.w != width || m_frame.h != height) {
			eDebug("Create new video texture");
			if(m_video_tex)
				SDL_DestroyTexture(m_video_tex);
			m_video_tex = SDL_CreateTexture(m_render, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STATIC, width, height);
			m_frame.w = width;
			m_frame.h = height;
		}
		SDL_Rect r;
		r.x = 0;
		r.y = 0;
		r.w = width;
		r.h = height;

		GstMapInfo info;
		GstBuffer *buf = gst_sample_get_buffer(gst_buf);
		gst_buffer_map(buf, &info, GST_MAP_READ);

		// TODO: get format from caps
		// I420
		guint8 *y, *u, *v;
		int ypitch, upitch, vpitch;
		y = info.data;
		v = y + I420_U_OFFSET(width, height);
		u = y + I420_V_OFFSET(width, height);
		ypitch = I420_Y_ROWSTRIDE(width);
		vpitch = I420_V_ROWSTRIDE(width);
		upitch = I420_U_ROWSTRIDE(width);
		SDL_UpdateYUVTexture(m_video_tex, &r, y, ypitch, u, upitch, v, vpitch);

		gst_buffer_unmap(buf, &info);
		gst_sample_unref(gst_buf);
	}

	// Render Video
	if (m_video_tex) {
		SDL_RenderCopy(m_render, m_video_tex, NULL, NULL);
	}

	// Render OSD
	SDL_UpdateTexture(m_osd_tex, NULL, m_osd->pixels, m_osd->pitch);
	SDL_RenderCopy(m_render, m_osd_tex, NULL, NULL);

	SDL_RenderPresent(m_render);
}

void gSDLDC::cleanup()
{
	// TODO
	SDL_DestroyTexture(m_osd_tex);
}

void gSDLDC::thread()
{
	hasStarted();

	bool stop = false;
	while (!stop) {
		SDL_Event event;
		if (SDL_WaitEvent(&event)) {
			switch (event.type) {
			case SDL_KEYDOWN:
			case SDL_KEYUP:
			case SDL_QUIT:
				m_pump.send(event);
				break;
			case SDL_USEREVENT:
				switch (event.user.code) {
				case EV_SET_VIDEO_MODE:
					evSetVideoMode((unsigned long)event.user.data1, (unsigned long)event.user.data2);
					break;
				case EV_FLIP:
					evFlip();
					break;
				case EV_QUIT:
					stop = true;
					cleanup();
					break;
				}
				break;
			}
		}
	}
}

eAutoInitPtr<gSDLDC> init_gSDLDC(eAutoInitNumbers::graphic-1, "gSDLDC");
