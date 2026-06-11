/*
 * fg_audio.h — PipeWire audio stack lifecycle management.
 *
 * frostedglass is the session: just as fg_wine.c owns the Wine desktop
 * process, this module owns the audio daemons.  YetiOS is
 * PipeWire-exclusive — winepipewire.drv inside Moonshine is a PipeWire
 * client, so the daemons must be up before Wine's mmdevapi probes its
 * driver list, or the driver reports Priority_Unavailable and mmdevapi
 * falls back to raw ALSA.
 *
 * Two processes are spawned, in order:
 *
 *   pipewire     — the graph/daemon itself.  Creates the client socket
 *                  at $XDG_RUNTIME_DIR/pipewire-0.
 *   wireplumber  — the session manager.  Without it nothing links
 *                  streams to devices and the "default" metadata object
 *                  (which SetDefaultAudioEndpoint writes through) does
 *                  not exist.
 */

#ifndef FG_AUDIO_H
#define FG_AUDIO_H

#include "fg_types.h"

/*
 * Spawn pipewire, wait for its socket to appear, then spawn wireplumber.
 * Call after the backend is started and before launch_wine(), so the
 * socket exists by the time Wine's audio driver probes it.
 *
 * If a PipeWire socket already exists in XDG_RUNTIME_DIR (an external
 * session already provides audio), nothing is spawned.
 *
 * Daemon stdout/stderr go to /tmp/pipewire.log and /tmp/wireplumber.log.
 */
void launch_audio(struct fg_server *server);

/*
 * Terminate the audio daemons (SIGTERM, then reaped by the SIGCHLD
 * handler).  Call from server_finish() after Wine has been shut down.
 */
void shutdown_audio(struct fg_server *server);

#endif /* FG_AUDIO_H */