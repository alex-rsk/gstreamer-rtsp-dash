#### Setup

```
mkdir rtsp-dash-streamer
cd rtsp-dash-streamer
mkdir src
```

#### Build
# Generate build system
autoreconf -fiv
## if you've GNOME autoconf:
#./autogen.sh

#### Configure the build
./configure

#### Compile
make

#### The binary will be in src/rtsp-dash-streamer

### Usage

#### Create output directory for DASH manifests
mkdir -p /tmp/dash-output

./src/rtsp-dash-streamer rtsp://your.camera.ip:554/stream /tmp/dash-output
