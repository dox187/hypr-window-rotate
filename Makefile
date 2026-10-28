VERSION   ?= 0.1.0

CXX       ?= g++
CXXFLAGS  ?= -O2 -g

# Debian ships Lua 5.4 as lua5.4.pc, Fedora and Arch as lua.pc
LUA_PC    ?= $(shell pkg-config --exists lua5.4 && echo lua5.4 || echo lua)

# kept apart from CXXFLAGS/CPPFLAGS so distro flags passed on the command line do not drop them
PLUGIN_FLAGS = -shared -fPIC -std=c++26 -Wall -Wno-unused-parameter -DPLUGIN_VERSION='"$(VERSION)"' \
               $(shell pkg-config --cflags hyprland pixman-1 libdrm $(LUA_PC))

PREFIX    ?= /usr/local
LIBDIR    ?= $(PREFIX)/lib
PLUGINDIR ?= $(LIBDIR)/hyprland/plugins

TARGET     = hypr-window-rotate.so
SRC        = src/main.cpp

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(PLUGIN_FLAGS) $(CPPFLAGS) $(CXXFLAGS) $(SRC) -o $@ $(LDFLAGS)

# install(1) writes a new inode, so a copy already loaded by Hyprland is not overwritten in place
install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(PLUGINDIR)/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(PLUGINDIR)/$(TARGET)

# verifies that the installed Hyprland exports every hooked function
check:
	./scripts/check-symbols.sh

clean:
	rm -f $(TARGET)

.PHONY: all install uninstall check clean
