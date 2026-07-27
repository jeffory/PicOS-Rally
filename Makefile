# PicOS Rally — native app build
CC      = arm-none-eabi-gcc
CFLAGS  = -mcpu=cortex-m33 -mthumb -std=gnu99 \
          -fpie -fno-plt -ffunction-sections -fdata-sections \
          -O2 -g -Wall -Wextra -Wno-unused-parameter \
          -I. -I../../sdk/native
LDFLAGS = -T ../../sdk/native/linker.ld \
          -Wl,--entry=picos_main \
          -Wl,-pie \
          -Wl,--gc-sections \
          -Wl,--no-warn-rwx-segments \
          -nostartfiles -nodefaultlibs -lc -lm -lgcc

SRCS    = app/main.c app/stubs.c \
          core/mathx.c core/tuning.c core/sim.c core/camera.c core/render.c \
          core/surface.c
TARGET  = main.elf

.PHONY: all clean

all: $(TARGET)

HDRS    = $(wildcard core/*.h) app/../core/font6x8.h

$(TARGET): $(SRCS) $(HDRS) ../../sdk/native/linker.ld ../../sdk/native/os.h ../../sdk/native/app_abi.h
	$(CC) $(CFLAGS) $(SRCS) $(LDFLAGS) -o $@
	arm-none-eabi-strip $@
	arm-none-eabi-size $@

clean:
	rm -f $(TARGET)
