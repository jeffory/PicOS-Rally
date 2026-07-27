# Headless host build (CI: unit tests, determinism replay)
CC      = gcc
CFLAGS  = -std=gnu99 -O2 -Wall -Wextra -I../.. -I../../core

SRCS    = main.c ../../core/mathx.c ../../core/tuning.c ../../core/sim.c \
          ../../core/camera.c ../../core/render.c ../../core/surface.c ../../core/track.c ../../core/ai.c \
          ../../core/gfx.c ../../core/render_track.c ../../core/audio_synth.c ../../core/effects.c

rally_headless: $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $@

.PHONY: clean
clean:
	rm -f rally_headless
