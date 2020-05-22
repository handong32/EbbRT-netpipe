#ifndef TCP_COMMAND_H
#define TCP_COMMAND_H

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

namespace ebbrt {
class TcpCommand : public StaticSharedEbb<TcpCommand>, public CacheAligned {
public:
  TcpCommand();
  void Start(uint16_t port);

private:
  class TcpSession : public ebbrt::TcpHandler {
  public:
    TcpSession(TcpCommand *mcd, ebbrt::NetworkManager::TcpPcb pcb)
        : ebbrt::TcpHandler(std::move(pcb)), mcd_(mcd) {}
    void Close() {}
    void Abort() {}
    void Receive(std::unique_ptr<MutIOBuf> b);
  private:
    std::unique_ptr<ebbrt::MutIOBuf> buf_;
    ebbrt::NetworkManager::TcpPcb pcb_;
    TcpCommand *mcd_;
  };

  NetworkManager::ListeningTcpPcb listening_pcb_;

}; 
}
#endif
