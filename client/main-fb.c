////////////////////////////////////////////////////////////////
//
// PiFrame (framebuffer edition)
// The Networked Digital Picture Frame for Raspberry Pi
//
// (c) Kuy Mainwaring, January 2016. All rights reserved.
//
//   This is a variation of client/main.c that behaves
// identically but does NOT use GTK+.  Instead it relies only on
// libcurl (download), libjpeg and libpng (decode), and renders
// the resulting pixels directly to the Linux framebuffer device
// (e.g. /dev/fb0).  It is intended for headless embedded Linux
// systems that have no window manager or GTK+ framework
// available - in particular the original Raspberry Pi Zero
// (BCM2835 / ARM1176).
//
//   As with the GTK+ edition, PiFrame requests images from a
// HTTP server and scales them to fill the screen.  After each
// image is downloaded and displayed, the application starts the
// next request immediately.  Therefore, timing is controlled by
// the HTTP server, which can wait for some time with an open
// connection before sending the image data.  Crucially, this
// allows for simple, flexible central coordination of many
// PiFrame instances.
//
//   Only PNG and JPEG images are supported.
//
// ./piframe-fb "http://10.0.0.84:3000/v1/nextPhoto"
//
////////////////////////////////////////////////////////////////


////////////////////////////////////////////////////////////////
//
// Useful tricks for development:
//
// Turn HDMI on or off (respectively):
//   tvservice [-p|-o]
//
// Find the framebuffer geometry / pixel format:
//   fbset -i
//
// Compile this file (Raspberry Pi / Raspbian):
//   gcc -o piframe-fb -O3 client/main-fb.c \
//       $(pkg-config --cflags --libs libcurl libpng) -ljpeg -lm
//
// (libjpeg has no pkg-config module on most systems, hence the
//  explicit -ljpeg.  libjpeg-turbo, which ships with Raspbian,
//  is a drop-in replacement and is recommended on the Pi.)
//
// Run (HDMI framebuffer is normally /dev/fb0):
//   ./piframe-fb "http://10.0.0.84:3000/v1/nextPhoto"
//
// Use a different framebuffer device:
//   ./piframe-fb -f /dev/fb1 "http://.../nextPhoto"
//
////////////////////////////////////////////////////////////////


////////////////////////////////////////////////////////////////
//
// Tips:
//
// It's useful to identify the piframe instance to the server if
//   you have multiple displays set up (e.g. on a single wall or
//   multiple rooms.)  To do this, parameterize the URL at
//   launch time
//
// ./piframe-fb "http://10.0.0.84:3000/v1/nextPhoto?id=$DEVICEID"
//
// For example, you could easily derive $DEVICEID from the
//   WiFi adapter's MAC address:
//
// ./piframe-fb "http://10.0.0.84:3000/v1/nextPhoto?id=$(tr ':' '_' < /sys/class/net/wlan0/address)"
//
////////////////////////////////////////////////////////////////


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>

#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/fb.h>
#include <linux/kd.h>

#include <curl/curl.h>
#include <jpeglib.h>
#include <png.h>


// FBIO_WAITFORVSYNC isn't declared in every <linux/fb.h>, but the
//   BCM2835 framebuffer driver implements it.  Define it ourselves
//   if the header didn't.
#ifndef FBIO_WAITFORVSYNC
	#define FBIO_WAITFORVSYNC	_IOW('F', 0x20, __u32)
#endif


#if defined(DEBUG)

	static void DebugPrintf(char const* fmt, ...)
	{
		va_list v;
		va_start(v, fmt);
		vfprintf(stderr, fmt, v);
		va_end(v);
	}

#else

	#define DebugPrintf(...) do{}while(0)

#endif


////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////
//
// Image: a simple 24-bit (R,G,B) top-down pixel buffer.  Both
//   the JPEG and PNG decoders normalise to this format so that
//   the rest of the program only ever deals with one layout.
//
////////////////////////////////////////////////////////////////

typedef struct Image
{
	int			width;
	int			height;
	uint8_t*	rgb;		// width * height * 3 bytes, row-major, top-down

} Image;


static void		ImageFree(Image* image)
{
	if(image == 0)
		return;

	free(image->rgb);
	free(image);
}


////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////
//
// Download subsystem: synchronously fetches a URL into a single
//   contiguous memory buffer using libcurl.  Timing is still
//   driven by the server (which may hold the connection open
//   before sending data), exactly as in the GTK+ edition.
//
////////////////////////////////////////////////////////////////

typedef struct MemoryBuffer
{
	uint8_t*	data;
	size_t		length;
	size_t		capacity;

} MemoryBuffer;


static size_t	onCURLDownloadSegment(void* segment, size_t count, size_t elements, void* user)
{
	size_t			segmentLength = count * elements;
	MemoryBuffer*	buffer = (MemoryBuffer*)user;

	// grow the buffer geometrically to amortise reallocation cost
	if(buffer->length + segmentLength > buffer->capacity)
	{
		size_t newCapacity = (buffer->capacity != 0)? buffer->capacity : (128 * 1024);
		while(buffer->length + segmentLength > newCapacity)
			newCapacity *= 2;

		uint8_t* grown = (uint8_t*)realloc(buffer->data, newCapacity);
		if(grown == 0)
			return(0);	// signal an error to libcurl (aborts the transfer)

		buffer->data = grown;
		buffer->capacity = newCapacity;
	}

	memcpy(buffer->data + buffer->length, segment, segmentLength);
	buffer->length += segmentLength;

	return(segmentLength);
}


// Download the given URL.  On success returns CURLE_OK and fills
//   *outBuffer (caller frees outBuffer->data).  On failure returns
//   the curl error code and leaves any partial buffer freed.
static int		downloadURL(char const* url, MemoryBuffer* outBuffer)
{
	DebugPrintf("+downloadURL %s\n", url);

	outBuffer->data = 0;
	outBuffer->length = 0;
	outBuffer->capacity = 0;

	CURL* curl = curl_easy_init();
	if(curl == 0)
	{
		DebugPrintf("-downloadURL curl init error\n");
		return(CURLE_FAILED_INIT);
	}

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &onCURLDownloadSegment);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)outBuffer);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "piframe-1.0/libcurl");
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);	// treat HTTP >= 400 as an error

	int result = curl_easy_perform(curl);

	curl_easy_cleanup(curl);

	if(result != CURLE_OK)
	{
		free(outBuffer->data);
		outBuffer->data = 0;
		outBuffer->length = 0;
		outBuffer->capacity = 0;
	}

	DebugPrintf("-downloadURL result=%i length=%zu\n", result, outBuffer->length);
	return(result);
}


////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////
//
// Decode subsystem: turns an in-memory JPEG or PNG file into an
//   Image (24-bit RGB).  The container format is detected from
//   the file's magic bytes.
//
////////////////////////////////////////////////////////////////

// --- JPEG ---------------------------------------------------- //

// libjpeg's default error handler calls exit() on a fatal error,
//   which would be rude in a long-running picture frame.  Instead
//   we longjmp back out of the decode call and report failure.
#include <setjmp.h>

typedef struct JPEGErrorManager
{
	struct jpeg_error_mgr	base;
	jmp_buf					escape;

} JPEGErrorManager;

static void		onJPEGError(j_common_ptr cinfo)
{
	JPEGErrorManager* err = (JPEGErrorManager*)cinfo->err;

	char message[JMSG_LENGTH_MAX];
	(*cinfo->err->format_message)(cinfo, message);
	fprintf(stderr, "piframe: JPEG decode error: %s\n", message);

	longjmp(err->escape, 1);
}

static Image*	decodeJPEG(uint8_t const* data, size_t length)
{
	DebugPrintf("+decodeJPEG length=%zu\n", length);

	struct jpeg_decompress_struct	cinfo;
	JPEGErrorManager				jerr;
	Image*							image = 0;

	cinfo.err = jpeg_std_error(&jerr.base);
	jerr.base.error_exit = &onJPEGError;

	if(setjmp(jerr.escape))
	{
		// control returns here on any fatal libjpeg error
		jpeg_destroy_decompress(&cinfo);
		ImageFree(image);
		DebugPrintf("-decodeJPEG error\n");
		return(0);
	}

	jpeg_create_decompress(&cinfo);
	jpeg_mem_src(&cinfo, (unsigned char*)data, (unsigned long)length);
	jpeg_read_header(&cinfo, TRUE);

	cinfo.out_color_space = JCS_RGB;	// force 3-component RGB output
	jpeg_start_decompress(&cinfo);

	image = (Image*)malloc(sizeof(Image));
	image->width = cinfo.output_width;
	image->height = cinfo.output_height;
	image->rgb = (uint8_t*)malloc((size_t)image->width * image->height * 3);

	// libjpeg emits scanlines top-down, which is exactly our layout
	while(cinfo.output_scanline < cinfo.output_height)
	{
		JSAMPROW row = image->rgb + (size_t)cinfo.output_scanline * image->width * 3;
		jpeg_read_scanlines(&cinfo, &row, 1);
	}

	jpeg_finish_decompress(&cinfo);
	jpeg_destroy_decompress(&cinfo);

	DebugPrintf("-decodeJPEG %ix%i\n", image->width, image->height);
	return(image);
}


// --- PNG ----------------------------------------------------- //

static Image*	decodePNG(uint8_t const* data, size_t length)
{
	DebugPrintf("+decodePNG length=%zu\n", length);

	// libpng's "simplified" API (1.6+) handles all the colour-type,
	//   bit-depth and interlace permutations for us and yields a
	//   flat RGB buffer.  Raspbian ships libpng 1.6.
	png_image png;
	memset(&png, 0, sizeof(png));
	png.version = PNG_IMAGE_VERSION;

	if(!png_image_begin_read_from_memory(&png, data, length))
	{
		fprintf(stderr, "piframe: PNG decode error: %s\n", png.message);
		DebugPrintf("-decodePNG header error\n");
		return(0);
	}

	png.format = PNG_FORMAT_RGB;	// drop any alpha; produce 3 bytes/pixel

	Image* image = (Image*)malloc(sizeof(Image));
	image->width = png.width;
	image->height = png.height;
	image->rgb = (uint8_t*)malloc(PNG_IMAGE_SIZE(png));

	// stride 0 => tightly packed (width * 3); top-down by default
	if(!png_image_finish_read(&png, NULL, image->rgb, 0, NULL))
	{
		fprintf(stderr, "piframe: PNG decode error: %s\n", png.message);
		png_image_free(&png);
		ImageFree(image);
		DebugPrintf("-decodePNG body error\n");
		return(0);
	}

	DebugPrintf("-decodePNG %ix%i\n", image->width, image->height);
	return(image);
}


// --- container sniffing -------------------------------------- //

static Image*	decodeImage(uint8_t const* data, size_t length)
{
	static uint8_t const kPNGSignature[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
	static uint8_t const kJPEGSignature[3] = { 0xFF, 0xD8, 0xFF };

	if((length >= sizeof(kPNGSignature)) && (memcmp(data, kPNGSignature, sizeof(kPNGSignature)) == 0))
		return(decodePNG(data, length));

	if((length >= sizeof(kJPEGSignature)) && (memcmp(data, kJPEGSignature, sizeof(kJPEGSignature)) == 0))
		return(decodeJPEG(data, length));

	fprintf(stderr, "piframe: unsupported image format (only PNG and JPEG are supported)\n");
	return(0);
}


// Read an entire local file into memory (used for the startup image).
static Image*	decodeImageFile(char const* path)
{
	FILE* file = fopen(path, "rb");
	if(file == 0)
	{
		fprintf(stderr, "piframe: can't open \"%s\": %s\n", path, strerror(errno));
		return(0);
	}

	fseek(file, 0, SEEK_END);
	long size = ftell(file);
	fseek(file, 0, SEEK_SET);

	if(size <= 0)
	{
		fclose(file);
		return(0);
	}

	uint8_t* data = (uint8_t*)malloc((size_t)size);
	if(fread(data, 1, (size_t)size, file) != (size_t)size)
	{
		fclose(file);
		free(data);
		return(0);
	}
	fclose(file);

	Image* image = decodeImage(data, (size_t)size);
	free(data);
	return(image);
}


////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////
//
// Framebuffer subsystem: opens, maps and renders to the Linux
//   framebuffer device.  Handles the common 16-bit (RGB565) and
//   32-bit (XRGB/ARGB) layouts generically by reading the pixel
//   bitfield offsets reported by the driver.
//
//   When the driver exposes a virtual resolution tall enough for
//   two pages, rendering is double-buffered with page flipping
//   (FBIOPAN_DISPLAY) for tear-free updates - the framebuffer
//   analogue of the GTK+ edition's two swapped GtkImage widgets.
//   Otherwise a single off-screen buffer is composited and then
//   copied to the visible page in one pass.
//
////////////////////////////////////////////////////////////////

typedef struct FrameBuffer
{
	int							fd;

	struct fb_var_screeninfo	var;
	struct fb_fix_screeninfo	fix;

	uint8_t*					mem;		// mmap'd device memory
	size_t						memLength;

	int							width;		// visible resolution
	int							height;
	int							bytesPerPixel;
	int							lineLength;	// bytes per row (>= width * bpp)

	int							pageCount;	// 1 (no flipping) or 2 (page flipping)
	int							currentPage;

	uint8_t*					backBuffer;	// used only when pageCount == 1

} FrameBuffer;


static int		FrameBufferOpen(FrameBuffer* fb, char const* device)
{
	memset(fb, 0, sizeof(*fb));

	fb->fd = open(device, O_RDWR);
	if(fb->fd < 0)
	{
		fprintf(stderr, "piframe: can't open framebuffer \"%s\": %s\n", device, strerror(errno));
		return(-1);
	}

	if(ioctl(fb->fd, FBIOGET_VSCREENINFO, &fb->var) < 0 ||
	   ioctl(fb->fd, FBIOGET_FSCREENINFO, &fb->fix) < 0)
	{
		fprintf(stderr, "piframe: can't query framebuffer geometry: %s\n", strerror(errno));
		close(fb->fd);
		return(-1);
	}

	fb->width = fb->var.xres;
	fb->height = fb->var.yres;
	fb->bytesPerPixel = (fb->var.bits_per_pixel + 7) / 8;
	fb->lineLength = fb->fix.line_length;

	if((fb->var.bits_per_pixel != 16) && (fb->var.bits_per_pixel != 24) && (fb->var.bits_per_pixel != 32))
	{
		fprintf(stderr, "piframe: unsupported framebuffer depth %u bpp (need 16, 24 or 32)\n", fb->var.bits_per_pixel);
		close(fb->fd);
		return(-1);
	}

	// map enough memory for whatever the driver advertises (covers
	//   both pages when a virtual resolution is configured)
	fb->memLength = fb->fix.smem_len;
	fb->mem = (uint8_t*)mmap(0, fb->memLength, PROT_READ | PROT_WRITE, MAP_SHARED, fb->fd, 0);
	if(fb->mem == MAP_FAILED)
	{
		fprintf(stderr, "piframe: can't mmap framebuffer: %s\n", strerror(errno));
		close(fb->fd);
		return(-1);
	}

	// can we page-flip?  We need room for two full pages vertically.
	fb->pageCount = 1;
	fb->currentPage = 0;
	fb->backBuffer = 0;
	if((fb->var.yres_virtual >= fb->var.yres * 2) &&
	   ((size_t)fb->lineLength * fb->var.yres * 2 <= fb->memLength))
	{
		fb->pageCount = 2;
	}
	else
	{
		// no second page: composite off-screen then blit
		fb->backBuffer = (uint8_t*)malloc((size_t)fb->lineLength * fb->height);
		if(fb->backBuffer == 0)
		{
			munmap(fb->mem, fb->memLength);
			close(fb->fd);
			return(-1);
		}
	}

	DebugPrintf("framebuffer %ix%i %ubpp line=%i pages=%i\n",
		fb->width, fb->height, fb->var.bits_per_pixel, fb->lineLength, fb->pageCount);

	return(0);
}


static void		FrameBufferClose(FrameBuffer* fb)
{
	if(fb->backBuffer != 0)
		free(fb->backBuffer);

	if(fb->mem != 0 && fb->mem != MAP_FAILED)
		munmap(fb->mem, fb->memLength);

	if(fb->fd >= 0)
		close(fb->fd);

	memset(fb, 0, sizeof(*fb));
	fb->fd = -1;
}


// Pack an 8-bit-per-channel colour into the framebuffer's native
//   pixel format using the bitfield positions reported by the driver.
static inline uint32_t	FrameBufferPack(FrameBuffer const* fb, uint8_t r, uint8_t g, uint8_t b)
{
	uint32_t pixel =
		(((uint32_t)r >> (8 - fb->var.red.length))   << fb->var.red.offset)   |
		(((uint32_t)g >> (8 - fb->var.green.length)) << fb->var.green.offset) |
		(((uint32_t)b >> (8 - fb->var.blue.length))  << fb->var.blue.offset);

	// opaque alpha if the format carries one
	if(fb->var.transp.length != 0)
		pixel |= (((uint32_t)0xFF >> (8 - fb->var.transp.length)) << fb->var.transp.offset);

	return(pixel);
}

static inline void		FrameBufferStore(FrameBuffer const* fb, uint8_t* p, uint32_t pixel)
{
	switch(fb->bytesPerPixel)
	{
	case 2:
		*(uint16_t*)p = (uint16_t)pixel;
		break;
	case 3:
		p[0] = (uint8_t)(pixel);
		p[1] = (uint8_t)(pixel >> 8);
		p[2] = (uint8_t)(pixel >> 16);
		break;
	case 4:
		*(uint32_t*)p = pixel;
		break;
	}
}


////////////////////////////////////////////////////////////////
//
// scaleToFillScreen + render: identical policy to the GTK+
//   edition.  The image is uniformly scaled so that it covers
//   the whole screen, cropping the overflow on the long axis and
//   centring it.  We fuse the scale, bilinear resample, format
//   conversion and write into a single pass so no intermediate
//   full-screen RGB buffer is needed.
//
//   gdk_pixbuf_scale() maps a destination pixel back to the
//   source as src = (dest - offset) / scale; we reproduce that
//   mapping exactly.
//
////////////////////////////////////////////////////////////////

static void		renderImage(FrameBuffer* fb, Image const* image, uint8_t* dest)
{
	int		screenWidth = fb->width,
			screenHeight = fb->height;

	double	pWidth = (double)image->width,
			pHeight = (double)image->height,
			pAspect = pWidth / pHeight;

	double	scale, xOff, yOff;

	if(pAspect < ((double)screenWidth / (double)screenHeight))
	{
		scale = (double)screenWidth / pWidth;
		yOff = ((scale * pHeight) - (double)screenHeight) / -2.0;
		xOff = 0.0;
	}
	else
	{
		scale = (double)screenHeight / pHeight;
		xOff = ((scale * pWidth) - (double)screenWidth) / -2.0;
		yOff = 0.0;
	}

	int maxX = image->width - 1;
	int maxY = image->height - 1;

	for(int dy = 0; dy < screenHeight; dy++)
	{
		// map this destination row back to a (fractional) source row
		double sy = ((double)dy - yOff) / scale;
		if(sy < 0.0) sy = 0.0;
		if(sy > (double)maxY) sy = (double)maxY;

		int		y0 = (int)sy;
		int		y1 = (y0 < maxY)? y0 + 1 : maxY;
		double	fy = sy - (double)y0;

		uint8_t* destRow = dest + (size_t)dy * fb->lineLength;

		for(int dx = 0; dx < screenWidth; dx++)
		{
			double sx = ((double)dx - xOff) / scale;
			if(sx < 0.0) sx = 0.0;
			if(sx > (double)maxX) sx = (double)maxX;

			int		x0 = (int)sx;
			int		x1 = (x0 < maxX)? x0 + 1 : maxX;
			double	fx = sx - (double)x0;

			// bilinear sample the four neighbouring source texels
			uint8_t const* p00 = image->rgb + ((size_t)y0 * image->width + x0) * 3;
			uint8_t const* p01 = image->rgb + ((size_t)y0 * image->width + x1) * 3;
			uint8_t const* p10 = image->rgb + ((size_t)y1 * image->width + x0) * 3;
			uint8_t const* p11 = image->rgb + ((size_t)y1 * image->width + x1) * 3;

			uint8_t rgb[3];
			for(int c = 0; c < 3; c++)
			{
				double top = (double)p00[c] * (1.0 - fx) + (double)p01[c] * fx;
				double bot = (double)p10[c] * (1.0 - fx) + (double)p11[c] * fx;
				double v = top * (1.0 - fy) + bot * fy;
				rgb[c] = (uint8_t)(v + 0.5);
			}

			FrameBufferStore(fb, destRow + (size_t)dx * fb->bytesPerPixel,
				FrameBufferPack(fb, rgb[0], rgb[1], rgb[2]));
		}
	}
}


// Scale and display an image, using page flipping where available.
static void		FrameBufferShow(FrameBuffer* fb, Image const* image)
{
	if(fb->pageCount == 2)
	{
		// render into the hidden page, then flip to it on the next vsync
		int page = fb->currentPage ^ 1;
		uint8_t* dest = fb->mem + (size_t)page * fb->var.yres * fb->lineLength;

		renderImage(fb, image, dest);

		fb->var.yoffset = page * fb->var.yres;
		fb->var.activate = FB_ACTIVATE_VBL;	// flip during vertical blanking
		ioctl(fb->fd, FBIOPAN_DISPLAY, &fb->var);

		// best-effort wait for the flip to actually happen (ignored if unsupported)
		uint32_t dummy = 0;
		ioctl(fb->fd, FBIO_WAITFORVSYNC, &dummy);

		fb->currentPage = page;
	}
	else
	{
		// composite off-screen then copy to the visible buffer in one go
		renderImage(fb, image, fb->backBuffer);
		memcpy(fb->mem, fb->backBuffer, (size_t)fb->lineLength * fb->height);
	}
}


////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////
//
// Console handling: while PiFrame owns the screen we put the
//   active virtual terminal into graphics mode so the text
//   console (login prompt, blinking cursor, kernel messages)
//   doesn't show through or scribble over our image.  This is
//   restored on exit.
//
////////////////////////////////////////////////////////////////

static struct
{
	int		ttyFD;
	int		previousMode;
	int		haveMode;

} gConsole = { -1, KD_TEXT, 0 };

static void		consoleEnterGraphics(void)
{
	gConsole.ttyFD = open("/dev/tty0", O_RDWR);
	if(gConsole.ttyFD < 0)
		return;	// not fatal - we just won't blank the console

	if(ioctl(gConsole.ttyFD, KDGETMODE, &gConsole.previousMode) == 0)
		gConsole.haveMode = 1;

	ioctl(gConsole.ttyFD, KDSETMODE, KD_GRAPHICS);
}

static void		consoleRestore(void)
{
	if(gConsole.ttyFD < 0)
		return;

	if(gConsole.haveMode)
		ioctl(gConsole.ttyFD, KDSETMODE, gConsole.previousMode);
	else
		ioctl(gConsole.ttyFD, KDSETMODE, KD_TEXT);

	close(gConsole.ttyFD);
	gConsole.ttyFD = -1;
}


////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////
//
// PiFrame application
//
////////////////////////////////////////////////////////////////

typedef struct AppOptions
{
	char const*		serviceURL;
	char const*		framebufferDevice;
	char const*		startupImage;
	unsigned int	delayMS;

} AppOptions;


// clean shutdown plumbing for SIGINT / SIGTERM
static volatile sig_atomic_t	gShouldExit = 0;

static void		onSignal(int signo)
{
	(void)signo;
	gShouldExit = 1;
}


static void		parseOptions(AppOptions* outOptions, int argc, char** argv)
{
	optind = 1;

	int c, i, haveURL = 0;
	while((c = getopt(argc, argv, "d:f:s:")) != -1)
	{
		switch(c)
		{
		case 'd':	// minimum delay between photos, milliseconds
			outOptions->delayMS = atoi(optarg);
			break;
		case 'f':	// framebuffer device
			outOptions->framebufferDevice = optarg;
			break;
		case 's':	// startup image file
			outOptions->startupImage = optarg;
			break;
		}
	}

	for(i = optind; i < argc; i++)
	{
		if(!haveURL)
		{
			outOptions->serviceURL = argv[i];
			haveURL = 1;
		}
		else
			fprintf(stderr, "Warning: extra argument ignored: \"%s\"\n", argv[i]);
	}
}


// Sleep in small slices so we stay responsive to a shutdown signal.
static void		interruptibleSleepMS(unsigned int milliseconds)
{
	while(milliseconds > 0 && !gShouldExit)
	{
		unsigned int slice = (milliseconds > 100)? 100 : milliseconds;
		usleep(slice * 1000);
		milliseconds -= slice;
	}
}


int		main(int argc, char** argv)
{
	// specify default options here
	AppOptions options =
	{
		.serviceURL = "",
		.framebufferDevice = "/dev/fb0",
		.startupImage = "startup.jpg",
		.delayMS = 1,
	};
	parseOptions(&options, argc, argv);

	curl_global_init(CURL_GLOBAL_ALL);

	// open and prepare the framebuffer
	FrameBuffer fb;
	if(FrameBufferOpen(&fb, options.framebufferDevice) < 0)
	{
		curl_global_cleanup();
		return(1);
	}

	// restore the console / framebuffer on Ctrl-C or kill
	signal(SIGINT, &onSignal);
	signal(SIGTERM, &onSignal);

	consoleEnterGraphics();

	// show the startup screen from a local file, then dwell for a
	//   few seconds (mirrors the GTK+ edition's 5s initial delay)
	Image* startup = decodeImageFile(options.startupImage);
	if(startup != 0)
	{
		FrameBufferShow(&fb, startup);
		ImageFree(startup);
	}
	else
	{
		fprintf(stderr, "piframe: continuing without startup image \"%s\"\n", options.startupImage);
	}

	DebugPrintf("*Using url=\"%s\", fb=\"%s\", delay=%i\n",
		options.serviceURL, options.framebufferDevice, options.delayMS);

	interruptibleSleepMS(5000);

	// main loop: download -> decode -> scale & display -> repeat.
	//   Timing is governed by the server, which may hold the
	//   connection open before delivering the next photo.
	while(!gShouldExit)
	{
		unsigned int delay = options.delayMS;

		MemoryBuffer buffer;
		int result = downloadURL(options.serviceURL, &buffer);

		if(result == CURLE_OK)
		{
			Image* image = decodeImage(buffer.data, buffer.length);
			if(image != 0)
			{
				FrameBufferShow(&fb, image);
				ImageFree(image);
			}
			else
			{
				// decode failed: back off before retrying
				delay = 10000;	// 10 seconds
			}
		}
		else
		{
			fprintf(stderr, "piframe: download failed: %s\n", curl_easy_strerror(result));
			delay = 10000;	// 10 seconds
		}

		free(buffer.data);

		interruptibleSleepMS(delay);
	}

	// tidy up
	consoleRestore();
	FrameBufferClose(&fb);
	curl_global_cleanup();

	DebugPrintf("-main clean exit\n");
	return(0);
}
