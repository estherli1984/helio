add_executable(ping_iouring_server ping_iouring_server.cc resp_parser.cc)
cxx_link(ping_iouring_server base uring_fiber_lib tls_lib http_server_lib)
