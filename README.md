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

There is also a framebuffer edition (`client/main-fb.c`) for headless embedded systems with no window manager or GTK+ available (for example the original Raspberry Pi Zero).  It behaves identically but uses only libcurl, libjpeg and libpng, decoding each image itself and rendering directly to the Linux framebuffer (e.g. `/dev/fb0`).  It supports PNG and JPEG images.

    ./piframe-fb "http://example.server.url/nextPhoto"

## Building

### Setup (Linux / Raspberry Pi OS)

Install the build tools and the development packages PiFrame depends on.  On Debian-based systems (including Raspberry Pi OS / Raspbian):

    sudo apt-get update
    sudo apt-get install build-essential pkg-config

For the framebuffer edition (`client/main-fb.c`):

    sudo apt-get install libpng-dev libjpeg-dev libcurl4-openssl-dev

For the GTK+ edition (`client/main.c`), also install:

    sudo apt-get install libgtk-3-dev

### Compiling

A `Makefile` builds both editions:

    make            # builds both: piframe (GTK+) and piframe-fb (framebuffer)
    make piframe    # GTK+ edition only
    make piframe-fb # framebuffer edition only

Or compile a single source file by hand.  The GTK+ client:

    gcc -o piframe -O3 $(pkg-config --cflags gtk+-3.0) $(pkg-config --libs gtk+-3.0) $(pkg-config --cflags libcurl) $(pkg-config --libs libcurl) client/main.c

The framebuffer client (on Raspbian, which ships libjpeg-turbo and libpng):

    gcc -o piframe-fb -O3 client/main-fb.c $(pkg-config --cflags --libs libcurl libpng) -ljpeg -lm

The framebuffer edition needs no X server or GTK+.  It puts the active console into graphics mode while running (restoring it on exit) and accepts a couple of extra options: `-f <device>` selects the framebuffer (default `/dev/fb0`) and `-s <file>` selects the startup image (default `startup.jpg`).


## Running at startup (systemd)

The `Makefile` can install either edition as a systemd service so the frame starts automatically at boot.  For a headless Pi (e.g. the Pi Zero), install the framebuffer edition:

    sudo make install-fb

For a Pi booting to the desktop (with autologin), you can instead install the GTK+ edition:

    sudo make install-gtk

Either command builds the chosen edition, installs the binary to `/usr/local/bin`, the startup image to `/usr/local/share/piframe/`, a config file to `/etc/default/piframe`, and a `piframe.service` unit to `/etc/systemd/system/`, then enables it (so it starts on boot).  The two editions share the single unit name `piframe.service`, so installing one replaces the other.

Set the photo service URL (and any extra options) by editing the config file, then start the service:

    sudo nano /etc/default/piframe      # set PIFRAME_URL=...
    sudo systemctl start piframe

    journalctl -u piframe -f            # follow its log output

To override the install prefix, pass `PREFIX` (e.g. `sudo make install-fb PREFIX=/usr`).  To remove everything:

    sudo make uninstall

### Hiding the boot console (optional)

On a dedicated frame you'll usually want a clean boot with no login prompt, blinking cursor or kernel messages bleeding through.  The included `setup.sh` script disables the `tty1` getty, quiets the kernel command line and silences `dmesg` on the console.  Review it first, then run it with `sudo sh setup.sh` and reboot.


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

