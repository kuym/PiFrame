# PiFrame
The Networked Digital Picture Frame for Raspberry Pi

(c) Kuy Mainwaring, January 2016. All rights reserved.

PiFrame requests images from a HTTP server and scales them
to fill the screen.  After each image is downloaded and
displayed, the application starts the next request
immediately.  Therefore, timing is controlled by the HTTP
server, which can wait for some time with an open connection
before sending the image data.  Crucially, this allows for
simple, flexible central coordination of many PiFrame
instances.

Usage:

    ./piframe "http://example.server.url/nextPhoto"

Note that PiFrame uses GTK+ and is intended for the Raspberry Pi but is not exclusive to that platform.  With trivial adjustment it should work on any Linux/GTK+ platform.

## Building

Right now, the PiFrame client is a single C source file.  Compile it as so:

    gcc -o piframe -O3 $(pkg-config --cflags gtk+-3.0) $(pkg-config --libs gtk+-3.0) $(pkg-config --cflags libcurl) $(pkg-config --libs libcurl) client/main.c


## Useful tricks for Raspberry Pi:

Turn HDMI on or off (respectively):

    tvservice [-p|-o]

Wake the display by exiting the screensaver (which can sometimes be just a black screen):

    xset s reset

Remotely run a GUI app:

    export DISPLAY=:0


Tips:

It's useful to identify the piframe instance to the server if
  you have multiple displays set up (e.g. on a single wall or
  multiple rooms.)  To do this, parameterize the URL at
  launch time:

    ./piframe "http://example.server.url/nextPhoto?id=$DEVICEID"

For example, you could easily derive `$DEVICEID` from the
  WiFi adapter's MAC address:

    ./piframe "http://example.server.url/nextPhoto?id=$(tr ':' '_' < /sys/class/net/wlan0/address)"

