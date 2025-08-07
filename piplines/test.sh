#!/bin/bash

RTSP_URI="rtsp://localhost:8554/test"
OUTPUT_PATH="/tmp/dash-output"


mkdir -p "$OUTPUT_PATH"

gst-launch-1.0 -v \
  rtspsrc location="$RTSP_URI" retry=10 timeout=5000000 protocols=tcp ! \
  rtph264depay ! \
  h264parse ! \
  avdec_h264 ! \
  videoconvert ! \
  videoscale ! \
  "video/x-raw,width=1280,height=720,framerate=25/1" ! \
  openh264enc bitrate=2000 ! \
  h264parse ! \
  dashsink mpd-filename="manifest.mpd" target-duration=4  dynamic=true
