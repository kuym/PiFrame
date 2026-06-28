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
#include <sys/socket.h>

#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

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
//   the curl error code and leaves any partial buffer freed.  The
//   HTTP status code (if any) is reported through *outStatus so the
//   caller can describe HTTP-level failures in the on-screen HUD.
static int		downloadURL(char const* url, MemoryBuffer* outBuffer, long* outStatus)
{
	DebugPrintf("+downloadURL %s\n", url);

	outBuffer->data = 0;
	outBuffer->length = 0;
	outBuffer->capacity = 0;

	if(outStatus != 0)
		*outStatus = 0;

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

	if(outStatus != 0)
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, outStatus);

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
	int							pendingPage;	// page being drawn before the next flip

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

// Read a native pixel back out of the framebuffer (the inverse of Store).
//   Needed by the HUD so it can alpha-blend over whatever is on screen.
static inline uint32_t	FrameBufferLoad(FrameBuffer const* fb, uint8_t const* p)
{
	switch(fb->bytesPerPixel)
	{
	case 2: return(*(uint16_t const*)p);
	case 3: return((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));
	case 4: return(*(uint32_t const*)p);
	}
	return(0);
}

// Expand an n-bit channel value up to a full 8 bits by replicating the
//   high bits, so e.g. 5-bit 0x1F becomes 0xFF rather than 0xF8.
static inline uint8_t	expandBits(uint32_t value, unsigned int bits)
{
	if(bits == 0)	return(0);
	if(bits >= 8)	return((uint8_t)value);

	uint8_t hi = (uint8_t)(value << (8 - bits));
	return((uint8_t)(hi | (hi >> bits)));
}

// Unpack a native pixel into 8-bit-per-channel R, G, B (the inverse of Pack).
static inline void		FrameBufferUnpack(FrameBuffer const* fb, uint32_t pixel, uint8_t* r, uint8_t* g, uint8_t* b)
{
	*r = expandBits((pixel >> fb->var.red.offset)   & ((1u << fb->var.red.length)   - 1), fb->var.red.length);
	*g = expandBits((pixel >> fb->var.green.offset) & ((1u << fb->var.green.length) - 1), fb->var.green.length);
	*b = expandBits((pixel >> fb->var.blue.offset)  & ((1u << fb->var.blue.length)  - 1), fb->var.blue.length);
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


// Fill an entire frame with a solid colour (used when there is no image
//   to show beneath the HUD - e.g. before the first photo arrives).
static void		fillFrame(FrameBuffer* fb, uint8_t* dest, uint8_t r, uint8_t g, uint8_t b)
{
	uint32_t pixel = FrameBufferPack(fb, r, g, b);

	for(int y = 0; y < fb->height; y++)
	{
		uint8_t* row = dest + (size_t)y * fb->lineLength;
		for(int x = 0; x < fb->width; x++)
			FrameBufferStore(fb, row + (size_t)x * fb->bytesPerPixel, pixel);
	}
}


////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////
//
// HUD overlay: a heads-up display drawn on top of the image whenever
//   something goes wrong (server unreachable, timeout, TLS failure,
//   bad HTTP response, undecodable image, ...).  It dims the bottom
//   ~1/6 of the screen with a 50% gray panel and prints the condition,
//   the configured server URL and this device's IP address in white
//   text with a black outline so it stays legible over any picture.
//
//   Text is drawn with an embedded 8x8 bitmap font (no FreeType / no
//   external font files), scaled up to suit the display.
//
////////////////////////////////////////////////////////////////

// Public-domain 8x8 bitmap font (dhepper/font8x8, "basic" set) covering
//   printable ASCII 0x20..0x7E.  Each glyph is 8 rows; within a row the
//   least-significant bit is the leftmost pixel.
static unsigned char const	gFont8x8[95][8] =
{
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ' '
	{0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, // '!'
	{0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, // '"'
	{0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, // '#'
	{0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, // '$'
	{0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00}, // '%'
	{0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, // '&'
	{0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00}, // '''
	{0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, // '('
	{0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00}, // ')'
	{0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, // '*'
	{0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00}, // '+'
	{0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, // ','
	{0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00}, // '-'
	{0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, // '.'
	{0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, // '/'
	{0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, // '0'
	{0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00}, // '1'
	{0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, // '2'
	{0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00}, // '3'
	{0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, // '4'
	{0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00}, // '5'
	{0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, // '6'
	{0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00}, // '7'
	{0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, // '8'
	{0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00}, // '9'
	{0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, // ':'
	{0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06}, // ';'
	{0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, // '<'
	{0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00}, // '='
	{0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, // '>'
	{0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00}, // '?'
	{0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, // '@'
	{0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00}, // 'A'
	{0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, // 'B'
	{0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00}, // 'C'
	{0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, // 'D'
	{0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00}, // 'E'
	{0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00}, // 'F'
	{0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00}, // 'G'
	{0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, // 'H'
	{0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // 'I'
	{0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, // 'J'
	{0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00}, // 'K'
	{0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00}, // 'L'
	{0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00}, // 'M'
	{0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, // 'N'
	{0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00}, // 'O'
	{0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00}, // 'P'
	{0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00}, // 'Q'
	{0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, // 'R'
	{0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00}, // 'S'
	{0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // 'T'
	{0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00}, // 'U'
	{0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, // 'V'
	{0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, // 'W'
	{0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, // 'X'
	{0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00}, // 'Y'
	{0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, // 'Z'
	{0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00}, // '['
	{0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00}, // '\'
	{0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00}, // ']'
	{0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00}, // '^'
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, // '_'
	{0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00}, // '`'
	{0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00}, // 'a'
	{0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00}, // 'b'
	{0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00}, // 'c'
	{0x38,0x30,0x30,0x3E,0x33,0x33,0x6E,0x00}, // 'd'
	{0x00,0x00,0x1E,0x33,0x3F,0x03,0x1E,0x00}, // 'e'
	{0x1C,0x36,0x06,0x0F,0x06,0x06,0x0F,0x00}, // 'f'
	{0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F}, // 'g'
	{0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00}, // 'h'
	{0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00}, // 'i'
	{0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E}, // 'j'
	{0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00}, // 'k'
	{0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // 'l'
	{0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00}, // 'm'
	{0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00}, // 'n'
	{0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00}, // 'o'
	{0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F}, // 'p'
	{0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78}, // 'q'
	{0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00}, // 'r'
	{0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00}, // 's'
	{0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00}, // 't'
	{0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00}, // 'u'
	{0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00}, // 'v'
	{0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00}, // 'w'
	{0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00}, // 'x'
	{0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F}, // 'y'
	{0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00}, // 'z'
	{0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00}, // '{'
	{0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, // '|'
	{0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00}, // '}'
	{0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00}, // '~'
};


// Plot a single opaque pixel, clipped to the screen.
static inline void	putPixel(FrameBuffer* fb, uint8_t* dest, int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
	if(x < 0 || y < 0 || x >= fb->width || y >= fb->height)
		return;

	FrameBufferStore(fb, dest + (size_t)y * fb->lineLength + (size_t)x * fb->bytesPerPixel,
		FrameBufferPack(fb, r, g, b));
}


// Alpha-blend a solid gray rectangle over the existing contents.
//   'gray' is the 0..255 grey level, 'alpha' is its 0..255 opacity.
static void		blendRect(FrameBuffer* fb, uint8_t* dest, int x, int y, int w, int h, uint8_t gray, uint8_t alpha)
{
	int x1 = x + w, y1 = y + h;
	if(x < 0) x = 0;
	if(y < 0) y = 0;
	if(x1 > fb->width)  x1 = fb->width;
	if(y1 > fb->height) y1 = fb->height;

	unsigned int inv = 255u - alpha;

	for(int yy = y; yy < y1; yy++)
	{
		uint8_t* row = dest + (size_t)yy * fb->lineLength;
		for(int xx = x; xx < x1; xx++)
		{
			uint8_t* p = row + (size_t)xx * fb->bytesPerPixel;

			uint8_t r, g, b;
			FrameBufferUnpack(fb, FrameBufferLoad(fb, p), &r, &g, &b);

			r = (uint8_t)((r * inv + gray * alpha) / 255u);
			g = (uint8_t)((g * inv + gray * alpha) / 255u);
			b = (uint8_t)((b * inv + gray * alpha) / 255u);

			FrameBufferStore(fb, p, FrameBufferPack(fb, r, g, b));
		}
	}
}


// Draw one glyph at (x, y), each font pixel expanded to a scale x scale block.
static void		drawGlyph(FrameBuffer* fb, uint8_t* dest, int x, int y, char c, int scale, uint8_t r, uint8_t g, uint8_t b)
{
	unsigned char ch = (unsigned char)c;
	if(ch < 0x20 || ch > 0x7E)
		ch = ' ';

	unsigned char const* glyph = gFont8x8[ch - 0x20];

	for(int row = 0; row < 8; row++)
	{
		unsigned char bits = glyph[row];
		for(int col = 0; col < 8; col++)
		{
			if(bits & (1 << col))	// LSB is the leftmost pixel
			{
				for(int sy = 0; sy < scale; sy++)
					for(int sx = 0; sx < scale; sx++)
						putPixel(fb, dest, x + col * scale + sx, y + row * scale + sy, r, g, b);
			}
		}
	}
}

static void		drawText(FrameBuffer* fb, uint8_t* dest, int x, int y, char const* text, int scale, uint8_t r, uint8_t g, uint8_t b)
{
	for(int cx = x; *text; text++, cx += 8 * scale)
		drawGlyph(fb, dest, cx, y, *text, scale, r, g, b);
}

// Draw white text with a black outline so it reads over any background.
static void		drawTextOutlined(FrameBuffer* fb, uint8_t* dest, int x, int y, char const* text, int scale, int stroke)
{
	for(int dy = -stroke; dy <= stroke; dy++)
		for(int dx = -stroke; dx <= stroke; dx++)
			if(dx != 0 || dy != 0)
				drawText(fb, dest, x + dx, y + dy, text, scale, 0, 0, 0);		// black halo

	drawText(fb, dest, x, y, text, scale, 255, 255, 255);						// white core
}

// Copy 'in' into 'out' truncated to at most maxChars glyphs, appending
//   ".." when it had to be shortened (URLs can be long).
static void		fitText(char* out, size_t outSize, char const* in, int maxChars)
{
	if(maxChars < 1)
		maxChars = 1;

	if((int)strlen(in) <= maxChars)
	{
		snprintf(out, outSize, "%s", in);
		return;
	}

	int keep = (maxChars > 2)? maxChars - 2 : 0;
	if((size_t)keep >= outSize)
		keep = (int)outSize - 1;

	memcpy(out, in, (size_t)keep);
	out[keep] = '\0';
	strncat(out, "..", outSize - strlen(out) - 1);
}


typedef struct Hud
{
	int				visible;
	char			condition[96];		// the unusual condition being reported
	char const*		url;				// the configured server URL
	char			ip[64];				// this device's IP address

} Hud;


// Draw the HUD panel and its three lines of text over the bottom of the frame.
static void		drawHud(FrameBuffer* fb, uint8_t* dest, Hud const* hud)
{
	char lineServer[600];
	char lineIP[80];
	snprintf(lineServer, sizeof(lineServer), "Server: %s", (hud->url && hud->url[0])? hud->url : "(none)");
	snprintf(lineIP, sizeof(lineIP), "IP: %s", hud->ip);

	char const* lines[3] = { hud->condition, lineServer, lineIP };
	int const lineCount = 3;

	// the panel covers roughly the bottom 1/6 of the screen
	int panelHeight = fb->height / 6;
	if(panelHeight < 24)
		panelHeight = (fb->height < 24)? fb->height : 24;
	int panelTop = fb->height - panelHeight;

	// 50%-opaque gray panel
	blendRect(fb, dest, 0, panelTop, fb->width, panelHeight, 128, 128);

	int pad = panelHeight / 10;
	if(pad < 2)
		pad = 2;

	// pick a font scale that fits the three lines within the panel height
	int scale = (panelHeight - 2 * pad) / (lineCount * 11);
	if(scale < 1)
		scale = 1;

	int stroke = scale / 2;
	if(stroke < 1)
		stroke = 1;

	int glyphH = 8 * scale;
	int lineStep = glyphH + 3 * scale;
	int maxChars = (fb->width - 2 * pad) / (8 * scale);

	int ty = panelTop + pad;
	for(int i = 0; i < lineCount; i++)
	{
		char buffer[640];
		fitText(buffer, sizeof(buffer), lines[i], maxChars);
		drawTextOutlined(fb, dest, pad, ty, buffer, scale, stroke);

		ty += lineStep;
		if(ty + glyphH > fb->height)
			break;
	}
}


////////////////////////////////////////////////////////////////
//
// Frame presentation: acquire a drawing surface (the hidden page when
//   page-flipping, otherwise the off-screen back buffer), let the
//   caller render the image and optional HUD into it, then make it
//   visible (page flip, or one blit to the framebuffer).
//
////////////////////////////////////////////////////////////////

static uint8_t*	FrameBufferBeginFrame(FrameBuffer* fb)
{
	if(fb->pageCount == 2)
	{
		fb->pendingPage = fb->currentPage ^ 1;
		return(fb->mem + (size_t)fb->pendingPage * fb->var.yres * fb->lineLength);
	}

	return(fb->backBuffer);
}

static void		FrameBufferEndFrame(FrameBuffer* fb)
{
	if(fb->pageCount == 2)
	{
		// flip to the freshly drawn page on the next vertical blank
		fb->var.yoffset = fb->pendingPage * fb->var.yres;
		fb->var.activate = FB_ACTIVATE_VBL;
		ioctl(fb->fd, FBIOPAN_DISPLAY, &fb->var);

		uint32_t dummy = 0;
		ioctl(fb->fd, FBIO_WAITFORVSYNC, &dummy);

		fb->currentPage = fb->pendingPage;
	}
	else
	{
		// composite off-screen then copy to the visible buffer in one go
		memcpy(fb->mem, fb->backBuffer, (size_t)fb->lineLength * fb->height);
	}
}

// Scale and display an image (page-flipped where available), drawing the
//   HUD over it when 'hud' is non-null and visible.  'image' may be null,
//   in which case the frame is cleared to black before the HUD.
static void		FrameBufferShow(FrameBuffer* fb, Image const* image, Hud const* hud)
{
	uint8_t* dest = FrameBufferBeginFrame(fb);

	if(image != 0)
		renderImage(fb, image, dest);
	else
		fillFrame(fb, dest, 0, 0, 0);

	if(hud != 0 && hud->visible)
		drawHud(fb, dest, hud);

	FrameBufferEndFrame(fb);
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


// Find this device's primary IPv4 address (first non-loopback, up
//   interface) for display in the HUD.  Writes "(no network)" if none
//   is found - which is itself useful diagnostic information.
static void		getDeviceIPAddress(char* out, size_t outSize)
{
	snprintf(out, outSize, "(no network)");

	struct ifaddrs* interfaces = 0;
	if(getifaddrs(&interfaces) != 0)
		return;

	for(struct ifaddrs* ifa = interfaces; ifa != 0; ifa = ifa->ifa_next)
	{
		if(ifa->ifa_addr == 0)							continue;
		if(ifa->ifa_addr->sa_family != AF_INET)			continue;	// IPv4 only
		if(ifa->ifa_flags & IFF_LOOPBACK)				continue;
		if(!(ifa->ifa_flags & IFF_UP))					continue;

		struct sockaddr_in* addr = (struct sockaddr_in*)ifa->ifa_addr;
		char ip[INET_ADDRSTRLEN];
		if(inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip)) != 0)
		{
			snprintf(out, outSize, "%s (%s)", ip, ifa->ifa_name);
			break;
		}
	}

	freeifaddrs(interfaces);
}


// Turn a libcurl result code into a short, human-readable description of
//   the unusual condition, suitable for the HUD.
static char const*	describeCurlError(int code)
{
	switch(code)
	{
	case CURLE_COULDNT_RESOLVE_HOST:
	case CURLE_COULDNT_RESOLVE_PROXY:	return("Cannot resolve server name");
	case CURLE_COULDNT_CONNECT:			return("Server unreachable");
	case CURLE_OPERATION_TIMEDOUT:		return("Connection timed out");
	case CURLE_SSL_CONNECT_ERROR:
	case CURLE_SSL_CERTPROBLEM:
	case CURLE_SSL_CACERT:				return("HTTPS / TLS error");	// modern curl merges peer-verification failures into CURLE_SSL_CACERT (60)
	case CURLE_WEIRD_SERVER_REPLY:		return("Unexpected server reply");
	case CURLE_GOT_NOTHING:
	case CURLE_PARTIAL_FILE:
	case CURLE_RECV_ERROR:				return("Empty or truncated response");
	default:							return(curl_easy_strerror(code));
	}
}


// Populate the HUD with the given condition plus the current IP address.
//   The server URL is set once at startup and left in place.
static void		hudReport(Hud* hud, char const* condition)
{
	hud->visible = 1;
	snprintf(hud->condition, sizeof(hud->condition), "%s", condition);
	getDeviceIPAddress(hud->ip, sizeof(hud->ip));

	fprintf(stderr, "piframe: %s | server=%s | ip=%s\n", hud->condition, hud->url, hud->ip);
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

	// the HUD reports the configured server URL and, on demand, the
	//   device IP and whatever unusual condition just occurred
	Hud hud;
	memset(&hud, 0, sizeof(hud));
	hud.url = options.serviceURL;

	// show the startup screen from a local file, then dwell for a
	//   few seconds (mirrors the GTK+ edition's 5s initial delay).
	//   We keep the most recently shown image around so the HUD always
	//   has something to sit on top of when an error occurs.
	Image* lastImage = decodeImageFile(options.startupImage);
	if(lastImage == 0)
		fprintf(stderr, "piframe: continuing without startup image \"%s\"\n", options.startupImage);

	FrameBufferShow(&fb, lastImage, 0);

	DebugPrintf("*Using url=\"%s\", fb=\"%s\", delay=%i\n",
		options.serviceURL, options.framebufferDevice, options.delayMS);

	interruptibleSleepMS(5000);

	// main loop: download -> decode -> scale & display -> repeat.
	//   Timing is governed by the server, which may hold the
	//   connection open before delivering the next photo.  Any unusual
	//   condition raises the HUD over the last good image.
	while(!gShouldExit)
	{
		unsigned int delay = options.delayMS;

		MemoryBuffer buffer;
		long httpStatus = 0;
		int result = downloadURL(options.serviceURL, &buffer, &httpStatus);

		if(result == CURLE_OK)
		{
			Image* image = decodeImage(buffer.data, buffer.length);
			if(image != 0)
			{
				// success: show the new photo (no HUD) and remember it
				FrameBufferShow(&fb, image, 0);

				if(lastImage != 0)
					ImageFree(lastImage);
				lastImage = image;
			}
			else
			{
				// the response wasn't a PNG/JPEG we could decode
				hudReport(&hud, "Malformed image data from server");
				FrameBufferShow(&fb, lastImage, &hud);
				delay = 10000;	// 10 seconds
			}
		}
		else if(result == CURLE_HTTP_RETURNED_ERROR)
		{
			// the server answered, but with an HTTP error status
			char condition[96];
			snprintf(condition, sizeof(condition), "Server returned HTTP %ld", httpStatus);
			hudReport(&hud, condition);
			FrameBufferShow(&fb, lastImage, &hud);
			delay = 10000;	// 10 seconds
		}
		else
		{
			// network / TLS / protocol level failure
			hudReport(&hud, describeCurlError(result));
			FrameBufferShow(&fb, lastImage, &hud);
			delay = 10000;	// 10 seconds
		}

		free(buffer.data);

		interruptibleSleepMS(delay);
	}

	// tidy up
	if(lastImage != 0)
		ImageFree(lastImage);

	consoleRestore();
	FrameBufferClose(&fb);
	curl_global_cleanup();

	DebugPrintf("-main clean exit\n");
	return(0);
}
