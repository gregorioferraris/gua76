# --- General Project Settings ---
# Plugin Bundle Name (matches folder name)
BUNDLE_NAME := gua76.lv2

# URI for the core plugin (MUST match gua76.cpp and gua76.ttl)
LV2_URI := http://moddevices.com/plugins/mod-devel/gua76

# URI for the GUI (MUST match gua76_gui.cpp and gua76.ttl)
LV2_GUI_URI := http://moddevices.com/plugins/mod-devel/gua76_ui

# Compiler (GCC/G++)
CXX = g++
CC = gcc # For C files like glad.c

# Standard C++ version (Dear ImGui requires C++11 or newer)
CXXSTANDARD = c++11
CSTANDARD = c99 # For glad.c

# --- Compiler Flags ---
COMMON_CXXFLAGS = -fPIC -Wall -O2 -std=$(CXXSTANDARD)
COMMON_CFLAGS = -fPIC -Wall -O2 -std=$(CSTANDARD)

# Include Paths (common to both core and GUI, or specific sections)
# LV2 headers are needed by both
LV2_HEADERS = $(shell pkg-config --cflags lv2)

# GUI specific includes
# Make sure these paths are correct relative to the Makefile
# For example, if imgui.h is in gua76.lv2/gui/imgui/, then -I./gui/imgui is correct
IMGUI_INC = -I./gui/imgui -I./gui/glad
GLFW_INC = $(shell pkg-config --cflags glfw3)

# All includes for C++ files
CORE_INCLUDES = $(LV2_HEADERS)
GUI_INCLUDES = $(LV2_HEADERS) $(IMGUI_INC) $(GLFW_INC)

# --- Linker Flags ---
# -shared to build a shared library (plugin)
# -lm for math functions (e.g., powf, log10f, expf)
# -ldl for dynamic linking (important for LV2)
# -lrt for real-time extensions (often useful on Linux)
COMMON_LIBS = -shared -lm -ldl -lrt

# GUI specific libraries
# -lGL explicitly for OpenGL
GLFW_LIBS = $(shell pkg-config --libs glfw3) -lGL

# --- Core Plugin Build ---
CORE_SRCS = gua76.cpp
CORE_OBJS = $(CORE_SRCS:.cpp=.o) # Objects will be in the current directory
CORE_TARGET = $(BUNDLE_NAME)/$(shell basename $(BUNDLE_NAME)).so # e.g. gua76.lv2/gua76.so

# --- GUI Build ---
# Source files for the GUI, relative to the Makefile
GUI_SRCS = gui/gua76_gui.cpp \
           gui/imgui/imgui.cpp \
           gui/imgui/imgui_draw.cpp \
           gui/imgui/imgui_widgets.cpp \
           gui/imgui/imgui_impl_opengl3.cpp \
           gui/imgui/imgui_impl_glfw.cpp \
           gui/glad/glad.c

# Object files for GUI. We'll place them in a 'build/' directory to keep the root clean.
GUI_OBJS_DIR = build
GUI_OBJS = $(patsubst gui/%.cpp, $(GUI_OBJS_DIR)/%.o, $(filter %.cpp, $(GUI_SRCS)))
GUI_OBJS += $(patsubst gui/%.c, $(GUI_OBJS_DIR)/%.o, $(filter %.c, $(GUI_SRCS)))

GUI_TARGET = $(BUNDLE_NAME)/$(shell basename $(BUNDLE_NAME))_ui.so # e.g. gua76.lv2/gua76_ui.so

# --- Targets ---
.PHONY: all clean deploy

all: $(CORE_TARGET) $(GUI_TARGET)

# Rule for building the core plugin shared library
$(CORE_TARGET): $(CORE_OBJS)
	@mkdir -p $(BUNDLE_NAME) # Ensure bundle directory exists
	$(CXX) $(COMMON_CXXFLAGS) $(CORE_INCLUDES) $^ $(COMMON_LIBS) -o $@

# Rule for building the GUI shared library
$(GUI_TARGET): $(GUI_OBJS)
	@mkdir -p $(BUNDLE_NAME) $(GUI_OBJS_DIR) # Ensure bundle and build directories exist
	$(CXX) $(COMMON_CXXFLAGS) $(GUI_INCLUDES) $^ $(COMMON_LIBS) $(GLFW_LIBS) -o $@

# Rule for compiling C++ files for the core (objects in current dir)
%.o: %.cpp
	$(CXX) $(COMMON_CXXFLAGS) $(CORE_INCLUDES) -c $< -o $@

# Rule for compiling C++ files for the GUI (objects in build/ dir)
$(GUI_OBJS_DIR)/%.o: gui/%.cpp
	@mkdir -p $(dir $@) # Ensure the object directory exists
	$(CXX) $(COMMON_CXXFLAGS) $(GUI_INCLUDES) -c $< -o $@

# Rule for compiling C files for the GUI (like glad.c, objects in build/ dir)
$(GUI_OBJS_DIR)/%.o: gui/%.c
	@mkdir -p $(dir $@)
	$(CC) $(COMMON_CFLAGS) $(GUI_INCLUDES) -c $< -o $@

# --- Clean Target ---
clean:
	rm -rf $(BUNDLE_NAME)/*.so $(CORE_OBJS) $(GUI_OBJS_DIR)/ *.o

# --- Deployment Target (Optional, for easy installation) ---
# This assumes you want to install it to your user's LV2 path.
# Change this path if you install system-wide or elsewhere.
LV2_INSTALL_PATH := ~/.lv2

deploy: all
	@echo "Deploying $(BUNDLE_NAME) to $(LV2_INSTALL_PATH)/$(BUNDLE_NAME)..."
	@mkdir -p $(LV2_INSTALL_PATH)
	@cp -r $(BUNDLE_NAME) $(LV2_INSTALL_PATH)/
	@echo "Deployment complete."
	@echo "You might need to refresh your DAW's plugin list."
