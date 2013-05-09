#!/bin/sh

cd ~/mjpg-streamer

bin/mjpg_streamer -o "lib/output_http.so -w ./www -p 8080" -i 'lib/input_uvc.so -f 2 --no_dynctrl'
