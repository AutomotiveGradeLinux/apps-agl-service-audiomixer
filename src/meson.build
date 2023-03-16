boost_dep = dependency('boost',
                       version : '>=1.72',
                       modules : [ 'thread', 'filesystem', 'program_options', 'log', 'system' ])
openssl_dep = dependency('openssl')
thread_dep = dependency('threads')
wp_dep = dependency('wireplumber-0.4')

src =  [ 'vis-config.cpp',
         'vis-session.cpp',
         'audiomixer-service.cpp',
         'audiomixer.c',
         'main.cpp'
]
executable('agl-service-audiomixer',
           src,
           dependencies: [boost_dep, openssl_dep, thread_dep, systemd_dep, wp_dep],
           install: true,
           c_args : [
           '-D_XOPEN_SOURCE=700',
           ],
           install_dir : get_option('sbindir'))
