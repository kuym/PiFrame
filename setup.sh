#!/bin/sh

# Disable getty on tty1
sudo systemctl stop getty@tty1.service
sudo systemctl disable getty@tty1.service
sudo systemctl mask getty@tty1.service

# Tune the kernel command line for a clean, quiet, cursor-free boot.
#   cmdline.txt must remain a single line of space-separated parameters.
CMDLINE=/boot/firmware/cmdline.txt

# Read the current single line of parameters.
line=$(cat "$CMDLINE")

# Append "quiet splash loglevel=0 logo.nologo vt.global_cursor_default=0",
#   skipping any parameter that is already present.
for param in quiet splash loglevel=0 logo.nologo vt.global_cursor_default=0; do
	case " $line " in
		*" $param "*) ;;			# already present, leave it alone
		*) line="$line $param" ;;	# not present, append it
	esac
done

# Remove "console=tty1" so the kernel/getty don't draw on our display.
new_line=""
for token in $line; do
	case "$token" in
		console=tty1) ;;					# drop it
		*) new_line="${new_line:+$new_line }$token" ;;
	esac
done

# Write the rebuilt single line back out.
echo "$new_line" | sudo tee "$CMDLINE" >/dev/null

# Append "dmesg --console-off" to /etc/rc.local so kernel messages stop
#   printing to the console after boot.  Insert it before the trailing
#   "exit 0" (if present) so it actually runs; otherwise just append.
RC_LOCAL=/etc/rc.local

if ! grep -qF "dmesg --console-off" "$RC_LOCAL"; then
	if grep -qF "exit 0" "$RC_LOCAL"; then
		sudo sed -i 's/^exit 0/dmesg --console-off\nexit 0/' "$RC_LOCAL"
	else
		echo "dmesg --console-off" | sudo tee -a "$RC_LOCAL" >/dev/null
	fi
fi
