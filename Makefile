# Simple manual Makefile to bypass autotools for debugging.
# Version 2: Added pthread library for threading.

# --- CONFIGURATION ---
# IMPORTANT: Make sure this path points to your Asterisk source directory.
ASTERISK_SRC_DIR = /usr/src/asterisk-18.26.2

# --- BUILD VARIABLES ---
CC = gcc
# CFLAGS: -g (debug), -Wall (all warnings), -fPIC (position-independent code for shared library)
CFLAGS = -g -Wall -fPIC
# We need to tell the compiler where to find the Asterisk headers.
INCLUDE_FLAGS = -I$(ASTERISK_SRC_DIR)/include
# LDFLAGS: We need to link against the POSIX threads library.
LDFLAGS = -lpthread

# The name of our output module
TARGET = chan_dongle_ng.so
# The source file
SOURCE = chan_dongle_ng.c

# --- TARGETS ---

# The default target to build. This is what runs when you type 'make'.
all: $(TARGET)

# Rule to build the shared object (.so) file
$(TARGET): $(SOURCE)
	@echo "--- Compiling module manually with gcc ---"
	$(CC) $(CFLAGS) $(INCLUDE_FLAGS) -shared -o $(TARGET) $(SOURCE) $(LDFLAGS)
	@echo "--- Compilation finished. Check for errors above. ---"

# Rule to install the module
install: all
	@echo "--- Installing module to /usr/lib/asterisk/modules/ ---"
	sudo install -m 755 $(TARGET) /usr/lib/asterisk/modules/

# Rule to clean up
clean:
	@echo "--- Cleaning up ---"
	rm -f $(TARGET)
