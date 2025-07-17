# av-cpp
Simple C++ API for ffmpeg libraries

## Build and Installation

Dependencies:

```console
# apt install libavformat-dev libavcodec-dev libswresample-dev libavutil-dev catch2 libfmt-dev
```

Meson build system is used:

```console
$ meson setup build
$ meson test -C build
```

## Usage

Check the test/example programs for usage.

## Authors

* **Calmels Clément** - *Initial work*
