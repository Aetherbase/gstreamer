mplex_opt = get_option('mplex').require(gpl_allowed, error_message: '''
  Plugin mplex explicitly required via options but GPL-licensed plugins disabled via options.
  Pass option -Dgpl=enabled to Meson to allow GPL-licensed plugins to be built.
  ''')

# See: ext/mplex for note about mjpegtools dep
mjpegtools_dep = dependency('mjpegtools', version : '>=2.0.0', required : mplex_opt)
mplex2_dep = cxx.find_library('mplex2', required : mplex_opt)

mplex_sources = [
  'gstmplex.cc',
  'gstmplexibitstream.cc',
  'gstmplexjob.cc',
  'gstmplexoutputstream.cc',
]

mplex_headers = [
  'gstmplex.hh',
  'gstmplexibitstream.hh',
  'gstmplexoutputstream.hh',
  'gstmplexjob.hh',
]

doc_sources = []
foreach s: mplex_sources + mplex_headers
  doc_sources += meson.current_source_dir() / s
endforeach

plugin_sources += {
  'mplex': pathsep.join(doc_sources)
}

if mjpegtools_dep.found() and mplex2_dep.found()
  gstmplex2 = library('gstmplex',
    mplex_sources,
    cpp_args : gst_plugins_bad_args + ['-DGST_MJPEGTOOLS_API=20000'],
    link_args : noseh_link_args,
    include_directories : [configinc, libsinc],
    dependencies : [gstaudio_dep, mjpegtools_dep, mplex2_dep],
    install : true,
    install_dir : plugins_install_dir,
  )
  plugins += [gstmplex2]
endif
