project("netpipe-ebbrt" CXX)
cmake_minimum_required(VERSION 2.6 FATAL_ERROR)
set(CMAKE_CXX_FLAGS_DEBUG          "-O0 -g3")
set(CMAKE_CXX_FLAGS_MINSIZEREL     "-Os -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE        "-O4 -flto -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g3")

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=gnu++14")

include_directories(${PROJECT_SRC_DIR})
#add_executable(server_netpipe.elf src/server_netpipe.cc src/UdpCommand.cc)
add_executable(server_netpipe.elf src/server_netpipe.cc src/TcpServer.cc)
add_executable(client_netpipe.elf src/client_netpipe.cc)

add_custom_command(TARGET server_netpipe.elf POST_BUILD 
  COMMAND objcopy -O elf32-i386 server_netpipe.elf server_netpipe.elf32 )

add_custom_command(TARGET client_netpipe.elf POST_BUILD
  COMMAND objcopy -O elf32-i386 client_netpipe.elf client_netpipe.elf32 )
