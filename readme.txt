This program is a simple alarm system for the raspberry pi, one GPIO
input sensor (e.g. an infrared sensor), an optional GPIO output device
(e.g. a relay based light bulb) and, also optional, a process to start
(e.g. mjpeg-streamer that streams mjpeg from a USB camera, using a
http server)

It mainly serves as an example of how to create an event based C program.

Build it by typing `make`. It's usage is

usage: ./alarm -i input-pin [-alarm 0|1][-o output-pin] [-c camera-app] [-min ms] [-extra ms] [-max ms]

where the -i stands for an input GPIO pin, -o for an output pin. These
pin numbers are the BCM GPIO number (4, 17, 18, 22, 23, 24, 25).
The -alarm is default 0, but if the sensor is 1 when no alarm, use this
option '-alarm 1'.
A process can be started when passing '-c path-to-program'.
And finally three timers determine how long the alarm should be up at
minimum, maximum and -extra stands for the time the alarm stays off after
being on.

When the alarm sensor goes to alarm value, the output pin is set to 1 and
the program is started. When after the timeouts the alarm goes off, the
program is interupted. It should therefore handle SIGINT (Ctrl+C) and
terminate gracefully.

To setup the GPIO's run the following as root (e.g. add it to rc.local),
the alarm program can run as normal user.
Example, using pin 18 as input, 23 as output for user 'pi'

echo 18 > /sys/class/gpio/export
echo 23 > /sys/class/gpio/export
echo out > /sys/class/gpio/gpio23/direction
echo both > /sys/class/gpio/gpio18/edge
chown pi /sys/class/gpio/gpio23/value

Have fun!
