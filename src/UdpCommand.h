//          Copyright Boston University SESA Group 2013 - 2014.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
#ifndef UDP_COMMAND_H
#define UDP_COMMAND_H

#include <memory>
#include <mutex>

#include <ebbrt/AtomicUniquePtr.h>
#include <ebbrt/CacheAligned.h>
#include <ebbrt/SharedIOBufRef.h>
#include <ebbrt/SpinLock.h>
#include <ebbrt/StaticSharedEbb.h>
#include <ebbrt/native/Net.h>
#include <ebbrt/native/NetTcpHandler.h>
#include <ebbrt/native/RcuTable.h>

#define IA32_MISC_ENABLE 0x1A0
#define IA32_PERF_CTL    0x199

extern int msgs;

namespace ebbrt {
  class UdpCommand: public StaticSharedEbb<UdpCommand>, public CacheAligned {
  public:
    UdpCommand();
    void Start(uint16_t port);

  private:
    void ReceiveCommand(Ipv4Address from_addr, uint16_t from_port,
			std::unique_ptr<MutIOBuf> buf);
    ebbrt::NetworkManager::UdpPcb udp_pcb;
  };
  
} // namespace ebbrt

#endif // UDP_COMMAND_H
