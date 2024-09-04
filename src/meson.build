boost_dep = dependency('boost',
                       version : '>=1.72',
                       modules : [ 'thread', 'filesystem', 'program_options', 'log', 'system' ])
wireplumber_dep = dependency('wireplumber-0.5')

cpp = meson.get_compiler('cpp')
grpcpp_reflection_dep = cpp.find_library('grpc++_reflection')

service_dep = [
    boost_dep,
    wireplumber_dep,
    dependency('openssl'),
    dependency('threads'),
    dependency('libsystemd'),
    dependency('protobuf'),
    dependency('grpc'),
    dependency('grpc++'),
    grpcpp_reflection_dep
]

protoc = find_program('protoc')
grpc_cpp = find_program('grpc_cpp_plugin')

protos_base_dir = get_option('protos')
protos_dir = protos_base_dir / 'kuksa/val/v1'
protoc_gen = generator(protoc, \
                       output : ['@BASENAME@.pb.cc', '@BASENAME@.pb.h'],
                       arguments : ['-I=' + protos_base_dir,
                         '--cpp_out=@BUILD_DIR@',
                         '@INPUT@'])
generated_protoc_sources = [ \
    protoc_gen.process(protos_dir / 'types.proto', preserve_path_from : protos_base_dir),
    protoc_gen.process(protos_dir / 'val.proto', preserve_path_from : protos_base_dir),
]

grpc_gen = generator(protoc, \
                     output : ['@BASENAME@.grpc.pb.cc', '@BASENAME@.grpc.pb.h'],
                     arguments : ['-I=' + protos_base_dir,
                       '--grpc_out=@BUILD_DIR@',
                       '--plugin=protoc-gen-grpc=' + grpc_cpp.path(),
                       '@INPUT@'])
generated_grpc_sources = [ \
    grpc_gen.process(protos_dir / 'val.proto', preserve_path_from : protos_base_dir),
]

src =  [
    'KuksaConfig.cpp',
    'KuksaClient.cpp',
    'AudiomixerService.cpp',
    'audiomixer.c',
    'main.cpp',
    generated_protoc_sources,
    generated_grpc_sources,
]

executable('agl-service-audiomixer',
           src,
           dependencies: service_dep,
           install: true,
           c_args : [
           '-D_XOPEN_SOURCE=700',
           ],
           install_dir : get_option('sbindir'))

executable('audio-mixer-test',
           ['audiomixertest.c', 'audiomixer.c'],
           dependencies: wireplumber_dep,
           install: true,
           c_args : [
           '-D_XOPEN_SOURCE=700',
           ],
           install_dir : get_option('bindir'))
