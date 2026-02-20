# Makefile for Triangles Wayland Compositor

CC = gcc
CFLAGS = -Wall -Wextra -g -O2
PKGCONFIG = pkg-config

# Directory structure
SRC_DIR = src
GEN_DIR = generated
BUILD_DIR = build

# Required packages
PACKAGES = wayland-server libinput libudev xkbcommon gbm egl glesv2 libdrm libseat

# Get flags from pkg-config
CFLAGS += $(shell $(PKGCONFIG) --cflags $(PACKAGES))
LDFLAGS += $(shell $(PKGCONFIG) --libs $(PACKAGES))

# Add pixman for region management
CFLAGS += $(shell $(PKGCONFIG) --cflags pixman-1)
LDFLAGS += $(shell $(PKGCONFIG) --libs pixman-1)

# Add include paths
CFLAGS += -I$(SRC_DIR) -I$(GEN_DIR)

# Source files
SOURCES = $(SRC_DIR)/main.c \
          $(SRC_DIR)/compositor.c \
          $(SRC_DIR)/output.c \
          $(SRC_DIR)/renderer.c \
          $(SRC_DIR)/surface.c \
          $(SRC_DIR)/protocol.c \
          $(SRC_DIR)/xdg_shell.c \
          $(SRC_DIR)/input.c \
          $(SRC_DIR)/dmabuf.c

# Object files (in build directory)
OBJECTS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SOURCES))

# Wayland protocol files
PROTOCOL_DIR = /usr/share/wayland-protocols
XDG_SHELL_PROTOCOL = $(PROTOCOL_DIR)/stable/xdg-shell/xdg-shell.xml
DMABUF_PROTOCOL    = $(PROTOCOL_DIR)/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml

# Generated protocol files
PROTOCOLS = $(GEN_DIR)/xdg-shell-protocol.c \
            $(GEN_DIR)/xdg-shell-server-protocol.h \
            $(GEN_DIR)/linux-dmabuf-unstable-v1-protocol.c \
            $(GEN_DIR)/linux-dmabuf-unstable-v1-server-protocol.h

# Target executable
TARGET = triangles

.PHONY: all clean protocols dirs

all: dirs protocols $(TARGET)

dirs:
	@mkdir -p $(BUILD_DIR) $(GEN_DIR)

protocols: dirs $(PROTOCOLS)

$(GEN_DIR)/xdg-shell-protocol.c: $(XDG_SHELL_PROTOCOL)
	wayland-scanner private-code $< $@

$(GEN_DIR)/xdg-shell-server-protocol.h: $(XDG_SHELL_PROTOCOL)
	wayland-scanner server-header $< $@

$(GEN_DIR)/linux-dmabuf-unstable-v1-protocol.c: $(DMABUF_PROTOCOL)
	wayland-scanner private-code $< $@

$(GEN_DIR)/linux-dmabuf-unstable-v1-server-protocol.h: $(DMABUF_PROTOCOL)
	wayland-scanner server-header $< $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c $(SRC_DIR)/compositor.h
	$(CC) $(CFLAGS) -c $< -o $@

# Special handling for xdg_shell.c to include generated header
$(BUILD_DIR)/xdg_shell.o: $(SRC_DIR)/xdg_shell.c $(GEN_DIR)/xdg-shell-server-protocol.h $(SRC_DIR)/compositor.h
	$(CC) $(CFLAGS) -c $(SRC_DIR)/xdg_shell.c -o $@

# dmabuf.c needs the generated linux-dmabuf header
$(BUILD_DIR)/dmabuf.o: $(SRC_DIR)/dmabuf.c $(GEN_DIR)/linux-dmabuf-unstable-v1-server-protocol.h $(SRC_DIR)/compositor.h
	$(CC) $(CFLAGS) -c $(SRC_DIR)/dmabuf.c -o $@

$(TARGET): $(OBJECTS) $(GEN_DIR)/xdg-shell-protocol.c $(GEN_DIR)/linux-dmabuf-unstable-v1-protocol.c
	$(CC) $(OBJECTS) $(GEN_DIR)/xdg-shell-protocol.c $(GEN_DIR)/linux-dmabuf-unstable-v1-protocol.c $(LDFLAGS) -o $(TARGET)

clean:
	rm -rf $(BUILD_DIR) $(GEN_DIR) $(TARGET)

install: $(TARGET)
	install -D -m 755 $(TARGET) $(DESTDIR)/usr/local/bin/$(TARGET)

uninstall:
	rm -f $(DESTDIR)/usr/local/bin/$(TARGET)
