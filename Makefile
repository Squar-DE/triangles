CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -g
LDFLAGS = -lwayland-server -lEGL -lGLESv2 -lgbm -ldrm

# Package config for dependencies
CFLAGS += $(shell pkg-config --cflags wayland-server libdrm gbm egl glesv2)
LDFLAGS += $(shell pkg-config --libs wayland-server libdrm gbm egl glesv2)

SRCDIR = .
SOURCES = $(SRCDIR)/main.c $(SRCDIR)/xdg-shell-protocol.c
OBJECTS = $(SOURCES:.c=.o)
TARGET = triangles

# Protocol files
PROTOCOLS = xdg-shell-protocol.h xdg-shell-protocol.c

.PHONY: all clean protocols

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

protocols: $(PROTOCOLS)

xdg-shell-protocol.h xdg-shell-protocol.c:
	@echo "Generating XDG shell protocol files..."
	@if command -v wayland-scanner >/dev/null 2>&1; then \
		wayland-scanner server-header /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml xdg-shell-protocol.h; \
		wayland-scanner private-code /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml xdg-shell-protocol.c; \
	else \
		echo "wayland-scanner not found. Please install wayland-protocols and wayland-scanner."; \
		exit 1; \
	fi

clean:
	rm -f $(OBJECTS) $(TARGET)

clean-protocols:
	rm -f $(PROTOCOLS)

install-deps:
	@echo "Installing dependencies (Ubuntu/Debian):"
	sudo apt-get update
	sudo apt-get install -y \
		libwayland-dev \
		wayland-protocols \
		libdrm-dev \
		libgbm-dev \
		libegl1-mesa-dev \
		libgles2-mesa-dev \
		libwayland-server0 \
		wayland-scanner \
		pkg-config

install-deps-fedora:
	@echo "Installing dependencies (Fedora):"
	sudo dnf install -y \
		wayland-devel \
		wayland-protocols-devel \
		libdrm-devel \
		mesa-libgbm-devel \
		mesa-libEGL-devel \
		mesa-libGLES-devel \
		wayland-scanner \
		pkgconfig

install-deps-arch:
	@echo "Installing dependencies (Arch Linux):"
	sudo pacman -S --needed \
		wayland \
		wayland-protocols \
		libdrm \
		mesa \
		wayland-scanner \
		pkgconf

run: $(TARGET)
	@echo "Running compositor (requires root or DRM permissions)..."
	sudo ./$(TARGET)

# Development helpers
debug: CFLAGS += -DDEBUG -O0
debug: $(TARGET)

release: CFLAGS += -O2 -DNDEBUG
release: $(TARGET)

# Check if running as root (required for DRM access)
check-permissions:
	@if [ "$$(id -u)" -ne 0 ]; then \
		echo "Warning: This compositor requires root privileges or DRM permissions"; \
		echo "Run with: sudo ./$(TARGET)"; \
		echo "Or add your user to the 'video' group and set up udev rules"; \
	fi
