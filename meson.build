project('av-cpp', 'cpp', default_options: [ 'cpp_std=c++14' ])

deps = [ dependency('libavformat'),
	 dependency('libavcodec'),
	 dependency('libswresample'),
	 dependency('libavutil'),
	 dependency('libswscale') ]

lib = library('ffmpeg-cpp',
	      sources: [ 'src/ffmpeg.hpp',
			 'src/ffmpeg.cpp' ],
	      dependencies: deps, install: true)

avcpp_dep = declare_dependency(dependencies: deps,
			       include_directories : include_directories('src'),
			       link_with : lib)

install_headers('src/ffmpeg.hpp', subdir: 'ffmpeg')

import('pkgconfig').generate(name: meson.project_name(),
			     description: 'Simple C++ API for ffmpeg',
			     version: meson.project_version(),
			     libraries: lib)

# tests
catch2_dep = dependency('catch2', fallback: [ 'catch2', 'catch2_dep' ])
video_test = executable('video_test', 'tests/video.cpp',
			dependencies: [avcpp_dep, catch2_dep ])
test('video test', video_test)

# examples
threads_dep = dependency('threads')

executable('rtsp_muxer', 'examples/rtsp_muxer.cpp',
	   dependencies: [ avcpp_dep, threads_dep ])

executable('pcm_to_mp3', 'examples/pcm_to_mp3.cpp',
	   dependencies: avcpp_dep)

executable('multi_stream', 'examples/multi_stream.cpp',
	   dependencies: [ avcpp_dep, threads_dep ])

executable('transcode', 'examples/transcode.cpp',
	   dependencies: avcpp_dep)
