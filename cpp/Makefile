CXX = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -pedantic
GTK_CFLAGS = $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS = $(shell pkg-config --libs gtk+-3.0)

SOURCES = torify_gui_modern.cpp tabs.cpp settings.cpp prefs.cpp
TARGET = torify

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CXX) $(CXXFLAGS) $(GTK_CFLAGS) $(SOURCES) -o $(TARGET) $(GTK_LIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
