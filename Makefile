MAIN_OBJECTS=main.o loop.o util.o
MONITOR_OBJECTS=monitor.o loop.o util.o

all: alarm monitor

alarm: $(MAIN_OBJECTS)
	gcc -s -o $@ $^ -lrt

monitor: $(MONITOR_OBJECTS)
	gcc -s -o $@ $^

%.o: %.c
	gcc -c -O2 -Wall $< -o $@

%.o: %.cpp
	g++ -c -O2 -Wall $< -o $@

clean:
	rm -f $(MAIN_OBJECTS) $(MONITOR_OBJECTS)
