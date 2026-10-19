#ifndef FTP_H
#define FTP_H

/*
 * Minimal passive-mode FTP server used to copy large *.img files onto the
 * device before boot/install (the WebUI's HTTP upload is fine for small
 * files, but FTP is far more robust for multi-GB transfers).
 *
 * Runs as a forked child so it can serve concurrently with the HTTP WebUI.
 * Anonymous login is always allowed (user "anonymous"/"ps5", any password).
 * All paths are chroot-like rooted at `root`: clients can only reach files
 * underneath it, and ".." is clamped.
 *
 * Returns the child PID on success, 0 if disabled, -1 on error.
 */
int ftp_server_start(const char *root, int port);

#endif
