project('ffmpeg-cpp', 'cpp', default_options: [ 'cpp_std=c++14' ])

deps = [ dependency('libavformat'),
	 dependency('libavcodec'),
	 dependency('libswresample'),
	 dependency('libavutil') ]

lib = library('ffmpeg-cpp',
	      sources: [ 'src/ffmpeg.hpp',
			 'src/ffmpeg.cpp' ],
	      dependencies: deps, install: true)

install_headers('src/ffmpeg.hpp', subdir: 'ffmpeg')

import('pkgconfig').generate(name: meson.project_name(),
			     description: 'Simple C++ API for ffmpeg',
			     version: meson.project_version(),
			     libraries: lib)

library_video_test = executable('library_video_test', 'src/video_test.cpp',
				dependencies: deps, link_with: lib)
test('library video test', library_video_test)

# examples

threads_dep = dependency('threads')

executable('rtsp_muxer', 'example/rtsp_muxer.cpp',
	   include_directories: include_directories('src'),
	   dependencies: [ deps,  threads_dep ],
	   link_with: lib)

executable('pcm_to_mp3', 'example/pcm_to_mp3.cpp',
	   include_directories: include_directories('src'),
	   dependencies: deps,
	   link_with: lib)

executable('multi_stream', 'example/multi_stream.cpp',
	   include_directories: include_directories('src'),
	   dependencies: [ deps,  threads_dep ],
	   link_with: lib)
