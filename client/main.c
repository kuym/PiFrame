////////////////////////////////////////////////////////////////
//
// PiFrame
// The Networked Digital Picture Frame for Raspberry Pi
//
// (c) Kuy Mainwaring, January 2016. All rights reserved.
// 
//   PiFrame requests images from a HTTP server and scales them
// to fill the screen.  After each image is downloaded and
// displayed, the application starts the next request
// immediately.  Therefore, timing is controlled by the HTTP
// server, which can wait for some time with an open connection
// before sending the image data.  Crucially, this allows for
// simple, flexible central coordination of many PiFrame
// instances.
//
// ./piframe "http://10.0.0.84:3000/v1/nextPhoto"
//
////////////////////////////////////////////////////////////////


////////////////////////////////////////////////////////////////
//
// Useful tricks for development:
//
// Turn HDMI on or off (respectively):
//   tvservice [-p|-o]
//
// Wake the display by exiting the screensaver (which can sometimes be just a black screen):
//   xset s reset
//
// Remotely run a GUI app:
//   export DISPLAY=:0
//
// Compile this file:
//   gcc -o piframe -O3 $(pkg-config --cflags gtk+-3.0) $(pkg-config --libs gtk+-3.0) $(pkg-config --cflags libcurl) $(pkg-config --libs libcurl) main.c
//
// Run:
//   ./piframe "http://10.0.0.84:3000/v1/nextPhoto"
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
// ./piframe "http://10.0.0.84:3000/v1/nextPhoto?id=$DEVICEID"
//
// For example, you could easily derive $DEVICEID from the
//   WiFi adapter's MAC address:
//
// ./piframe "http://10.0.0.84:3000/v1/nextPhoto?id=$(tr ':' '_' < /sys/class/net/wlan0/address)"
//
////////////////////////////////////////////////////////////////


#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <gtk/gtk.h>
#include <curl/curl.h>


#if defined(DEBUG)

	int DebugPrintf(char const* fmt, ...)
	{
		if((0))	// runtime switch
		{
			va_list v;
			va_start(v, fmt);

			vprintf(fmt, v);

			va_end(v);
		}
	}

#else
	
	#define DebugPrintf(x, ...) do{}while(0)

#endif

////////////////////////////////////////////////////////////////
// This patch was taken from the GNOME source and is the fusion
//   of g_async_queue_push_front() and
//   g_async_queue_push_front_unlocked(), combined for simplicity

// PATCH if(glib version < 2.46)
#if (GLIB_MAJOR_VERSION <= 2) && (GLIB_MINOR_VERSION < 46)

	typedef struct _GAsyncQueue
	{
		GMutex mutex;
		GCond cond;
		GQueue queue;
		GDestroyNotify item_free_func;
		guint waiting_threads;
		gint ref_count;

	} GAsyncQueue;

	void g_async_queue_push_front(GAsyncQueue* queue, gpointer item)
	{
		g_return_if_fail(queue != NULL);
		g_return_if_fail(item != NULL);

		g_mutex_lock(&queue->mutex);
		
		g_queue_push_tail(&queue->queue, item);
		if(queue->waiting_threads > 0)
			g_cond_signal(&queue->cond);

		g_mutex_unlock(&queue->mutex);
	}

#endif
// END PATCH


////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////
//
// Download subsystem: downloads a URL with cURL asynchronously
//   with callback progress, completion and failure notification
//   on the main thread.
//
////////////////////////////////////////////////////////////////

static size_t onCURLDownloadSegment(void* segment, size_t count, size_t elements, void* user);

typedef struct DownloadOptions
{
	char const*		url;		// duplicated, owned by DownloadOptions
	
	void			(*progressCallback)(struct DownloadOptions const* download, unsigned char const* data, size_t length, size_t received, size_t expected);
	void			(*completeCallback)(struct DownloadOptions const* download, int result, char const* reason);
	
	void*			context;	// context for callbacks

} DownloadOptions;

struct DownloadProgressItem;

typedef struct Download
{
	DownloadOptions					options;

	int								result;
	char const*						reason;	// static permanent strings only

	size_t							bytesExpected;
	size_t							bytesLoaded;

	struct DownloadProgressItem*	currentProgressItem;

	int								outstandingProgressItems;

} Download;

typedef struct DownloadProgressItem
{
	Download*		context;

	void*			chunk;
	size_t			length;

	size_t			bytesExpected;
	size_t			bytesLoaded;

} DownloadProgressItem;

static struct
{
	// Download instances flow main -> curl
	GAsyncQueue*	jobQueue;
	
	// Download instances flow main <- curl
	GAsyncQueue*	resultsQueue;

	// DownloadProgressItem instances flow main <- curl for dispatch
	GAsyncQueue*	progressQueue;

	// DownloadProgressItem instances flow main -> curl for recycling
	GAsyncQueue*	progressRecycleQueue;

	unsigned int	count;

} gDownload;


static gboolean		onDownloadQueuePoll(gpointer user);

void				DownloadInit(void)
{
	DebugPrintf("+DownloadInit\n");
	curl_global_init(CURL_GLOBAL_ALL);

	gDownload.jobQueue = g_async_queue_new();
	gDownload.resultsQueue = g_async_queue_new();
	gDownload.progressQueue = g_async_queue_new();
	gDownload.progressRecycleQueue = g_async_queue_new();
	gDownload.count = 0;
	DebugPrintf("-DownloadInit\n");
}

Download*			DownloadNew(DownloadOptions const* options)
{
	DebugPrintf("+DownloadNew\n");
	Download* download = malloc(sizeof(Download));
	DebugPrintf("malloc(Download) %p\n", download);

	download->options.url = strdup(options->url);
	DebugPrintf("strdup(options->url), %p\n", download->options.url);

	download->options.progressCallback = options->progressCallback;
	download->options.completeCallback = options->completeCallback;
	download->options.context = options->context;

	download->result = 0;
	download->reason = 0;
	download->bytesExpected = 0;
	download->bytesLoaded = 0;

	download->currentProgressItem = 0;

	download->outstandingProgressItems = 0;

	g_async_queue_push(gDownload.jobQueue, download);

	if(gDownload.count == 0)
	{
		DebugPrintf("+Download adding queue poller\n");
		gdk_threads_add_timeout(100, &onDownloadQueuePoll, 0);
	}

	gDownload.count++;

	DebugPrintf("-DownloadNew\n");
	return(download);
}

static gboolean		onDownloadQueuePoll(gpointer user)
{
	DebugPrintf("+onDownloadQueuePoll\n");
	
	// handle progress items
	DownloadProgressItem* progressItem;
	while((progressItem = (DownloadProgressItem*)g_async_queue_try_pop(gDownload.progressQueue)) != 0)
	{
		DebugPrintf("+onDownloadQueuePoll progressItem\n");
		if(progressItem->context->options.progressCallback)
		{
			DebugPrintf("*onDownloadQueuePoll progress callback\n");
			progressItem->context->options.progressCallback(&progressItem->context->options, progressItem->chunk, progressItem->length, progressItem->bytesLoaded, progressItem->bytesExpected);
		}

		DebugPrintf("*onDownloadQueuePoll outstandingProgressItems %i--\n", progressItem->context->outstandingProgressItems);
		progressItem->context->outstandingProgressItems--;
		progressItem->context = 0;	// burn the reference just in case

		g_async_queue_push(gDownload.progressRecycleQueue, progressItem);
		DebugPrintf("-onDownloadQueuePoll progressItem\n");
	}

	// handle completion items
	Download* completeItem;
	while((completeItem = (Download*)g_async_queue_try_pop(gDownload.resultsQueue)) != 0)
	{
		DebugPrintf("+onDownloadQueuePoll completeItem\n");
		if(completeItem->outstandingProgressItems == 0)
		{
			DebugPrintf("+onDownloadQueuePoll outstandingProgressItems == 0\n");
			
			// call completion callback indicating success
			if(completeItem->options.completeCallback)
			{
				DebugPrintf("+onDownloadQueuePoll complete callback\n");
				completeItem->options.completeCallback(&completeItem->options, completeItem->result, completeItem->reason);
				DebugPrintf("-onDownloadQueuePoll complete callback\n");
			}

			DebugPrintf("free((void*)completeItem->options.url) %p\n", (void*)completeItem->options.url);
			free((void*)completeItem->options.url);
			DebugPrintf("free(completeItem) %p\n", completeItem);
			free(completeItem);

			gDownload.count--;
			DebugPrintf("-onDownloadQueuePoll outstandingProgressItems == 0\n");
		}
		else
		{
			DebugPrintf("*onDownloadQueuePoll outstandingProgressItems > 0\n");
			// defer it until later to allow progress items to be processed first
			g_async_queue_push_front(gDownload.resultsQueue, completeItem);
		}
	}
	
	// handle queue empty: return false when it's empty to stop this callback from being called periodically
	DebugPrintf("-onDownloadQueuePoll %s\n", (gDownload.count > 0)? "continuing" : "shutting down");
	return(gDownload.count > 0);
}

void			downloadRecycleProgressItems(void)
{
	DebugPrintf("+downloadRecycleProgressItems\n");
	DownloadProgressItem* progressItem;
	while((progressItem = g_async_queue_try_pop(gDownload.progressRecycleQueue)) != 0)
	{
		DebugPrintf("*downloadRecycleProgressItems item\n");
		DebugPrintf("free(progressItem->chunk) %p\n", progressItem->chunk);
		free(progressItem->chunk);
		DebugPrintf("free(progressItem) %p\n", progressItem);
		free(progressItem);
	}
	DebugPrintf("-downloadRecycleProgressItems\n");
}

#define kMaxChunksize (128 * 1024)

static void		downloadPushProgressItem(Download* context)
{
	if(context->currentProgressItem != 0)
	{
		DebugPrintf("+downloadPushProgressItem len=%i loaded=%i / %i\n", context->currentProgressItem->length, context->bytesLoaded, context->bytesExpected);
		context->currentProgressItem->bytesExpected = context->bytesExpected;
		context->currentProgressItem->bytesLoaded = context->bytesLoaded;

		context->currentProgressItem->context = context;
		context->outstandingProgressItems++;

		g_async_queue_push(gDownload.progressQueue, context->currentProgressItem);
		context->currentProgressItem = 0;
		DebugPrintf("-downloadPushProgressItem\n");
	}

	g_thread_yield();
}

static size_t	onCURLDownloadSegment(void* segment, size_t count, size_t elements, void* user)
{
	DebugPrintf("+onCURLDownloadSegment len=%i\n", count * elements);
	downloadRecycleProgressItems();

	Download* context = (Download*)user;
	size_t segmentLength = count * elements;

	while(segmentLength > 0)
	{
		DebugPrintf("+onCURLDownloadSegment inner len=%i\n", segmentLength);
		if(context->currentProgressItem != 0)
		{
			size_t chunkSize = MIN(kMaxChunksize - context->currentProgressItem->length, segmentLength);
			
			DebugPrintf("+onCURLDownloadSegment fill chunkSize=%i\n", chunkSize);
			
			memcpy(context->currentProgressItem->chunk + context->currentProgressItem->length, segment, chunkSize);
			context->currentProgressItem->length += segmentLength;
			context->bytesLoaded += chunkSize;
			segmentLength -= chunkSize;
			segment = (void*)(((unsigned char*)segment) + segmentLength);

			if(context->currentProgressItem->length == kMaxChunksize)
			{
				DebugPrintf("*onCURLDownloadSegment push\n");
				downloadPushProgressItem(context);
			}
			
			DebugPrintf("-onCURLDownloadSegment fill\n");
		}

		if(context->currentProgressItem == 0)
		{
			DebugPrintf("+onCURLDownloadSegment reload\n");
			
			context->currentProgressItem = (DownloadProgressItem*)malloc(sizeof(DownloadProgressItem));
			DebugPrintf("malloc(DownloadProgressItem) %p\n", context->currentProgressItem);

			context->currentProgressItem->chunk = malloc(kMaxChunksize);
			DebugPrintf("malloc(kMaxChunksize) %p\n", context->currentProgressItem->chunk);
			context->currentProgressItem->length = 0;

			DebugPrintf("-onCURLDownloadSegment reload\n");
		}

		DebugPrintf("-onCURLDownloadSegment inner len=%i\n", segmentLength);
	}

	DebugPrintf("-onCURLDownloadSegment\n");
	return(count * elements);
}

static int		onCURLDownloadProgress(void* user, double dlTotal, double dlCurrent, double ulTotal, double ulCurrent)
{
	DebugPrintf("+onCURLDownloadProgress\n");
	(void)ulTotal;
	(void)ulCurrent;

	Download* context = (Download*)user;

	context->bytesExpected = (size_t)dlTotal;
	context->bytesLoaded = (size_t)dlCurrent;

	DebugPrintf("-onCURLDownloadProgress\n");
	return(0);
}

static gpointer	curlThread(gpointer info)
{
	DebugPrintf("+curlThread\n");
	g_async_queue_ref(gDownload.jobQueue);
	g_async_queue_ref(gDownload.resultsQueue);
	g_async_queue_ref(gDownload.progressQueue);
	g_async_queue_ref(gDownload.progressRecycleQueue);

	while(1)
	{
		DebugPrintf("+curlThread wait\n");

		// wait for a new job
		Download* download = g_async_queue_pop(gDownload.jobQueue);

		DebugPrintf("-curlThread wait\n");
		
		if(download == 0)	// shutdown token
		{
			DebugPrintf("*curlThread shutdown\n");
			break;
		}

		CURL* curl = curl_easy_init();
		if(curl == 0)
		{
			DebugPrintf("*curlThread curl init error\n");
			download->result = 0;
			download->reason = "curl-init-error";
			g_async_queue_push(gDownload.resultsQueue, download);
			continue;
		}

		// set up the curl session
		curl_easy_setopt(curl, CURLOPT_URL, (void*)download->options.url);

		// register progressive download (write) callback
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &onCURLDownloadSegment);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)download);

		curl_easy_setopt(curl, CURLOPT_USERAGENT, "piframe-1.0/libcurl");

		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, &onCURLDownloadProgress);
		curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, (void*)download);

		DebugPrintf("+curlThread active\n");
		int result = curl_easy_perform(curl);
		DebugPrintf("-curlThread active, result=%i\n", result);

		downloadPushProgressItem(download);

		curl_easy_cleanup(curl);

		download->result = result;
		download->reason = "curl-done";

		g_async_queue_push(gDownload.resultsQueue, download);

		downloadRecycleProgressItems();
	}

	downloadRecycleProgressItems();

	g_async_queue_unref(gDownload.jobQueue);
	g_async_queue_unref(gDownload.resultsQueue);
	g_async_queue_unref(gDownload.progressQueue);
	g_async_queue_unref(gDownload.progressRecycleQueue);

	DebugPrintf("-curlThread\n");
	return(0);
}



////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////
//
// ImageDownload subsystem: uses Download to fetch an image
//   and incrementally load it into a GdkPixbuf for use in the
//   application UI.
//
////////////////////////////////////////////////////////////////

typedef struct ImageDownloadOptions
{
	char const*			url;
	void				(*completeCallback)(void* context, GdkPixbuf* pixels, GError* error);
	void*				context;

} ImageDownloadOptions;


typedef struct ImageDownload
{
	ImageDownloadOptions	options;
	GdkPixbufLoader*		loader;
	Download*				download;
	
	int						refcount;

} ImageDownload;


void	onImageDownloadProgress(DownloadOptions const* download, unsigned char const* data, size_t length, size_t received, size_t expected);
void	onImageDownloadComplete(DownloadOptions const* download, int result, char const* reason);


ImageDownload*		ImageDownloadNew(ImageDownloadOptions const* options)
{
	DebugPrintf("+ImageDownloadInit\n");
	ImageDownload* imageDownload = (ImageDownload*)malloc(sizeof(ImageDownload));
	DebugPrintf("malloc(ImageDownload) %p\n", imageDownload);
	
	imageDownload->options.url = strdup(options->url);
	DebugPrintf("strdup(url), %p\n", imageDownload->options.url);
	imageDownload->options.completeCallback = options->completeCallback;
	imageDownload->options.context = options->context;

	imageDownload->loader = gdk_pixbuf_loader_new();

	imageDownload->refcount = 2;	// own reference and 

	DownloadOptions downloadOptions =
	{
		.url = imageDownload->options.url,
		.progressCallback = &onImageDownloadProgress,
		.completeCallback = &onImageDownloadComplete,
		.context = (void*)imageDownload
	};
	
	imageDownload->download = DownloadNew(&downloadOptions);
	DebugPrintf("-ImageDownloadInit\n");
	return(imageDownload);
}

void				ImageDownloadStop(ImageDownload* download)
{
	// @@signal Download instance to stop, when it does (asynchronously), --refcount and free if 0
}


void				onImageDownloadProgress(DownloadOptions const* download, unsigned char const* data, size_t length, size_t received, size_t expected)
{
	DebugPrintf("+onImageDownloadProgress length=%i\n", length);
	ImageDownload* imageDownload = (ImageDownload*)download->context;

	GError* error = 0;
	if(!gdk_pixbuf_loader_write(imageDownload->loader, data, length, &error))
	{
		DebugPrintf("*onImageDownloadProgress loader err\n");
		// @@stop download
	}
	else
	{
		DebugPrintf("*onImageDownloadProgress loader happy\n");
	}
	DebugPrintf("-onImageDownloadProgress\n");
}

void				onImageDownloadComplete(DownloadOptions const* download, int result, char const* reason)
{
	DebugPrintf("+onImageDownloadComplete result=%i reason=%s\n", result, reason);
	ImageDownload* imageDownload = (ImageDownload*)download->context;
	
	GError* error = 0;
	if((result == CURLE_OK) && gdk_pixbuf_loader_close(imageDownload->loader, &error))
	{
		DebugPrintf("+onImageDownloadComplete Download ok\n");
		GdkPixbuf* pixels = gdk_pixbuf_loader_get_pixbuf(imageDownload->loader);
		
		// invoke callback
		imageDownload->options.completeCallback(imageDownload->options.context, pixels, 0);
		DebugPrintf("-onImageDownloadComplete Download ok\n");
	}
	else
	{
		DebugPrintf("+onImageDownloadComplete Download error %i, \"%s\", loader error: %p\n", result, curl_easy_strerror(result), error);

		if(result != CURLE_OK)
		{
			gdk_pixbuf_loader_close(imageDownload->loader, 0);	// (ignore any errors from this)

			error = g_error_new_literal(g_quark_from_static_string("piframe-image-download-error-quark"), result, curl_easy_strerror(result));
		}

		// invoke callback
		imageDownload->options.completeCallback(imageDownload->options.context, 0, error);

		g_error_free(error);

		DebugPrintf("-onImageDownloadComplete Download error\n");
	}

	g_object_unref(imageDownload->loader);

	DebugPrintf("free((void*)imageDownload->options.url) %p\n", (void*)imageDownload->options.url);
	free((void*)imageDownload->options.url);
	DebugPrintf("free(imageDownload) %p\n", imageDownload);
	free(imageDownload);
	
	DebugPrintf("-onImageDownloadComplete\n");
}



////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////
//
// PiFrame application: requests images from a HTTP server and
//   scales them to fill the screen.  After each image is
//   downloaded and displayed, the application makes the next
//   request immediately.  Therefore, timing is controlled by
//   the HTTP server, which can wait for some time with an open
//   connection before sending the image data.  Crucially, this
//   allows for simple, flexible central coordination of many
//   PiFrame instances.
//
// The application uses two GtkImage widgets that each fill the
//   screen - one behind and one in front.  The widget behind
//   is filled with new image data when a download is complete,
//   the two widgets are swapped in z-order then the cycle
//   starts again.
//
// The screen-filling policy chosen crops part of the source
//   image (if necessary) in order to ensure all screen pixels
//   are filled.
//
////////////////////////////////////////////////////////////////


GdkPixbuf*	scaleToFillScreen(GdkPixbuf* pixels)
{
	DebugPrintf("+scaleToFillScreen\n");
	
	// calculate scale and crop dimensions
	GdkScreen* screen = gdk_screen_get_default();
	
	int		screenWidth = gdk_screen_get_width(screen),
			screenHeight = gdk_screen_get_height(screen);

	double	scale, xOff, yOff,
			pWidth = (double)gdk_pixbuf_get_width(pixels),
			pHeight = (double)gdk_pixbuf_get_height(pixels),
			pAspect = pWidth / pHeight;

	if(pAspect < ((double)screenWidth / (double)screenHeight))
	{
		scale = (double)screenWidth / pWidth;
		yOff = ((scale * pHeight) - (double)screenHeight) / -2.0;
		xOff = 0.0;
	}
	else
	{
		scale = ((double)screenHeight / pHeight);
		xOff = ((scale * pWidth) - (double)screenWidth) / -2.0;
		yOff = 0.0;
	}

	GdkPixbuf* scaledPixels = gdk_pixbuf_new(gdk_pixbuf_get_colorspace(pixels), FALSE, gdk_pixbuf_get_bits_per_sample(pixels), screenWidth, screenHeight);
	gdk_pixbuf_scale(	pixels, scaledPixels,
						0, 0, screenWidth, screenHeight,
						xOff, yOff, scale, scale,
						GDK_INTERP_BILINEAR
					);

	DebugPrintf("-scaleToFillScreen\n");
	return(scaledPixels);
}


typedef struct AppOptions
{
	char const*		serviceURL;
	unsigned int	delayMS;

} AppOptions;

typedef struct NextImageContext
{
	AppOptions		options;

	GtkWidget*		newImage;
	GdkPixbuf*		newSourcePixbuf;

	GtkWidget*		previousImage;
	GdkPixbuf*		previousSourcePixbuf;	// the previous unscaled pixbuf used to fill the previous image

	ImageDownload*	currentDownload;

} NextImageContext;


static gboolean		onNextDownloadDelay(gpointer user);

void	onNextDownloadComplete(void* context, GdkPixbuf* pixels, GError* error)
{
	DebugPrintf("+onNextDownloadComplete\n");
	NextImageContext* nextImage = (NextImageContext*)context;

	int minimumDelay = nextImage->options.delayMS;

	if(pixels != 0)
	{
		DebugPrintf("+onNextDownloadComplete (pixels != 0)\n");

		// release old pixmap (last photo)
		g_object_unref(nextImage->previousSourcePixbuf);

		nextImage->newSourcePixbuf = pixels;
		g_object_ref(nextImage->newSourcePixbuf);

		GdkPixbuf* scaledPixels = scaleToFillScreen(pixels);

		gtk_image_set_from_pixbuf(GTK_IMAGE(nextImage->newImage), scaledPixels);
		g_object_unref(scaledPixels);

		// reorder widget Z-order so that 'nextImage->newImage' is above 'nextImage->previousImage'
		GdkWindow* newImageWindow = gtk_widget_get_parent_window(nextImage->newImage);
		GdkWindow* previousImageWindow = gtk_widget_get_parent_window(nextImage->previousImage);
		gdk_window_restack(newImageWindow, previousImageWindow, TRUE);

		// swap widget references
		GtkWidget* temp = nextImage->previousImage;
		nextImage->previousImage = nextImage->newImage;
		nextImage->newImage = temp;

		// the current photo is the new old photo (reference conserved)
		nextImage->previousSourcePixbuf = nextImage->newSourcePixbuf;
		nextImage->newSourcePixbuf = 0;

		DebugPrintf("-onNextDownloadComplete (pixels != 0)\n");
	}
	else
	{
		DebugPrintf("+onNextDownloadComplete (pixels == 0) error\n");

		// handle *error
		DebugPrintf("*onNextDownloadComplete: download error, no photo update.\n");

		minimumDelay = 10000;	// 10 seconds

		DebugPrintf("-onNextDownloadComplete (pixels == 0) error\n");
		// we continue, essentially repeating the download (widgets don't swap)
	}

	// kick off next download

	gdk_threads_add_timeout(minimumDelay, &onNextDownloadDelay, (void*)nextImage);
	DebugPrintf("-onNextDownloadComplete\n");
}

static gboolean		onNextDownloadDelay(gpointer user)
{
	(void)user;
	DebugPrintf("+onNextDownloadDelay\n");
	NextImageContext* nextImage = (NextImageContext*)user;

	ImageDownloadOptions downloadOptions =
	{
		.url = nextImage->options.serviceURL,
		.completeCallback = &onNextDownloadComplete,
		.context = nextImage,
	};
	nextImage->currentDownload = ImageDownloadNew(&downloadOptions);

	DebugPrintf("-onNextDownloadDelay\n");
	return(FALSE);	// don't repeat, 1-shot only
}



void	parseOptions(AppOptions* outOptions, int argc, char** argv)
{
	optind = 1;

	int c, i, haveURL = 0;
	while((c = getopt(argc, argv, "d:")) != -1)
	{
		switch(c)
		{
		case 'd':	// delay
			outOptions->delayMS =  atoi(optarg);
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

int		main(int argc, char** argv)
{
	// specify defaults options here
	AppOptions options =
	{
		.serviceURL = "",
		.delayMS = 1,
	};
	parseOptions(&options, argc, argv);

	DownloadInit();

	gtk_init(&argc, &argv);

	// Create the main, top level window
	GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);

	// Give it the title
	gtk_window_set_title(GTK_WINDOW(window), "PiFrame");

	// Center the window
	gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);

	// Set the window's default size
	gtk_window_set_default_size(GTK_WINDOW(window), 200, 100);

	// Map the destroy signal of the window to gtk_main_quit;
	//   When the window is about to be destroyed, we get a notification and
	//   stop the main GTK+ loop by returning 0
	g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

	// load the pixels for the startup screen from a local file
	GError* err = 0;
	GdkPixbuf* startupPixels = gdk_pixbuf_new_from_file("startup.jpg", &err);

	GdkPixbuf* scaledPixels = scaleToFillScreen(startupPixels);
	GtkWidget* topImage = gtk_image_new_from_pixbuf(scaledPixels);
	GtkWidget* bottomImage = gtk_image_new_from_pixbuf(scaledPixels);
	g_object_unref(scaledPixels);

	GtkWidget* fixedContainer = gtk_fixed_new();

	gtk_container_add(GTK_CONTAINER(window), fixedContainer);
	gtk_fixed_put(GTK_FIXED(fixedContainer), bottomImage, 0, 0);
	gtk_fixed_put(GTK_FIXED(fixedContainer), topImage, 0, 0);

	
	GdkScreen* screen = gdk_screen_get_default();
	
	int		screenWidth = gdk_screen_get_width(screen),
			screenHeight = gdk_screen_get_height(screen);

	gtk_widget_set_size_request(topImage, screenWidth, screenHeight);
	gtk_widget_set_size_request(bottomImage, screenWidth, screenHeight);

	// make everything visible
	gtk_widget_show_all(window);

	gtk_window_fullscreen(GTK_WINDOW(window));

	GdkCursor* noCursor = gdk_cursor_new_for_display(gdk_display_get_default(), GDK_BLANK_CURSOR);
	gdk_window_set_cursor(gtk_widget_get_parent_window(fixedContainer), noCursor);
	gdk_window_set_cursor(gtk_widget_get_parent_window(bottomImage), noCursor);
	gdk_window_set_cursor(gtk_widget_get_parent_window(topImage), noCursor);

	
	// Start the main loop, and do nothing (block) until
	//   the application is closed
	if(!g_thread_new("curl-thread", &curlThread, 0) != 0)
	{
		g_warning("Can't create a thread for downloading images");
		return(0);
	}


	// set up the download
	NextImageContext* context = malloc(sizeof(NextImageContext));

	// inherit the app options
	memcpy(&context->options, &options, sizeof(AppOptions));
	
	context->previousImage = bottomImage;
	context->newImage = topImage;
	g_object_ref(context->previousImage);
	g_object_ref(context->newImage);
	
	context->previousSourcePixbuf = startupPixels;	// (keeps reference)
	context->newSourcePixbuf = 0;
	context->currentDownload = 0;

	DebugPrintf("*Using url=\"%s\", delay=%i\n\n", context->options.serviceURL, context->options.delayMS);

	// kick off the first download
	gdk_threads_add_timeout(5000, &onNextDownloadDelay, (void*)context);


	gtk_main();

	return(0);
}
