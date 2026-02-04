CC = gcc
WAYLAND_SCANNER = wayland-scanner

CFLAGS = -Wall -Wextra -O2 -pthread \
         $(shell pkg-config --cflags wayland-client cairo pangocairo)
LDFLAGS = -lpcap -pthread \
          $(shell pkg-config --libs wayland-client cairo pangocairo)

TARGET = ../matrix-wallpaper

# Protocol XML sources
LAYER_XML = protocols/wlr-layer-shell-unstable-v1.xml
XDG_XML   = /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml

# Generated protocol files
LAYER_H = wlr-layer-shell-unstable-v1-client-protocol.h
LAYER_C = wlr-layer-shell-unstable-v1-protocol.c
XDG_H   = xdg-shell-client-protocol.h
XDG_C   = xdg-shell-protocol.c

PROTO_HDRS = $(LAYER_H) $(XDG_H)
PROTO_SRCS = $(LAYER_C) $(XDG_C)

# Source files
SRCS = matrix_packets.c capture.c streams.c render_wayland.c $(PROTO_SRCS)
OBJS = $(SRCS:.c=.o)

.PHONY: all clean install

all: $(TARGET)

# Generate protocol headers and sources from XML
$(LAYER_H): $(LAYER_XML)
	$(WAYLAND_SCANNER) client-header $< $@

$(LAYER_C): $(LAYER_XML)
	$(WAYLAND_SCANNER) private-code $< $@

$(XDG_H): $(XDG_XML)
	$(WAYLAND_SCANNER) client-header $< $@

$(XDG_C): $(XDG_XML)
	$(WAYLAND_SCANNER) private-code $< $@

# Object files with dependencies
render_wayland.o: render_wayland.c render_wayland.h capture.h streams.h $(PROTO_HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

wlr-layer-shell-unstable-v1-protocol.o: $(LAYER_C) $(PROTO_HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

xdg-shell-protocol.o: $(XDG_C) $(XDG_H)
	$(CC) $(CFLAGS) -c -o $@ $<

matrix_packets.o: matrix_packets.c capture.h streams.h render_wayland.h
	$(CC) $(CFLAGS) -c -o $@ $<

capture.o: capture.c capture.h
	$(CC) $(CFLAGS) -c -o $@ $<

streams.o: streams.c streams.h capture.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGET) $(OBJS) $(PROTO_HDRS) $(PROTO_SRCS)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/matrix-wallpaper
	@echo "Installed to /usr/local/bin/matrix-wallpaper"
	@echo "Run with: sudo matrix-wallpaper [interface]"
	@echo "Or set capabilities: sudo setcap cap_net_raw=eip /usr/local/bin/matrix-wallpaper"
