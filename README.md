####
A badly written netpipe implementation for EbbRT

## Build (requires EbbRT sysroot to be built first)

```
$ mkdir build && cd build
$ EBBRT_SYSROOT=~/sysroot/native cmake -DCMAKE_TOOLCHAIN_FILE=~/sysroot/native/usr/misc/ebbrt.cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=EbbRT ..
$ make -j8 server_netpipe.elf
$ make -j8 client_netpipe.elf
```


