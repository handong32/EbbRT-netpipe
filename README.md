####
A badly written netpipe implementation for EbbRT

## Build (requires EbbRT sysroot to be built first)

```
$ mkdir build && cd build
$ EBBRT_SYSROOT=~/sysroot/native cmake -DCMAKE_TOOLCHAIN_FILE=~/sysroot/native/usr/misc/ebbrt.cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=EbbRT ..
$ make -j8 server_netpipe.elf
$ make -j8 client_netpipe.elf
```

## Running
`client_netpipe.cc` encodes in its main loop the possible combinations of message size, DVFS, and ITR settings to run. `server_netpipe.cc` just echos back the message size. After each experiment of a single message size, another computer can use `socat` to retrieve the trace logs from `server_netpipe.cc`.
