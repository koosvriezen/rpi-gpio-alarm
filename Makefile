MAIN_OBJECTS=main.o loop.o
MONITOR_OBJECTS=monitor.o loop.o

all: alarm monitor

alarm: $(MAIN_OBJECTS)
	gcc -s -o $@ $^

monitor: $(MONITOR_OBJECTS)
	gcc -s -o $@ $^

%.o: %.c
	gcc -c -O2 -Wall $< -o $@

%.o: %.cpp
	g++ -c -O2 -Wall $< -o $@
