CFLAGS=-c -Wall `pkg-config --cflags libdaemon libconfig` -I/usr/local/Cellar/libebur128/1.0.2/include -g -ggdb -I/usr/local/include
LDFLAGS=`pkg-config --libs libdaemon libconfig libpulse-simple`  -L/usr/local/Cellar/libebur128/1.0.2/lib -lebur128 -lpthread
SOURCES=shusherd.c
OBJECTS=$(SOURCES:.c=.o)
EXECUTABLE=shusherd

all: $(SOURCES) $(EXECUTABLE)

$(EXECUTABLE): $(OBJECTS)
	$(CC) $(OBJECTS) -o $@ $(LDFLAGS)

.c.o:
	$(CC) $(CFLAGS) $< -o $@

sndfile-jackplay: jackplay.o
	$(CC) $(LDFLAGS) $< -o $@

clean:
	rm -f *.o $(EXECUTABLE)
