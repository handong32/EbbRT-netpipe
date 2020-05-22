#include <cstdlib>
#include <sstream>
#include <ebbrt/SharedIOBufRef.h>
#include <ebbrt/UniqueIOBuf.h>

#include "TcpCommand.h"

ebbrt::TcpCommand::TcpCommand() {}

void ebbrt::TcpCommand::Start(uint16_t port) {
  listening_pcb_.Bind(port, [this](NetworkManager::TcpPcb pcb) {
    // new connection callback
    static std::atomic<size_t> cpu_index{0};
    auto index = 1; //cpu_index.fetch_add(1) % ebbrt::Cpu::Count();
    pcb.BindCpu(index);
    auto connection = new TcpSession(this, std::move(pcb));
    connection->Install();
  });
}

void ebbrt::TcpCommand::TcpSession::Receive(std::unique_ptr<MutIOBuf> b) {
  kassert(b->Length() != 0);
  uint32_t mcore = static_cast<uint32_t>(Cpu::GetMine());  
  std::string s(reinterpret_cast<const char*>(b->Data()));  

  // reply buffer pointer  
  std::string tmp = "test test test";
  auto rbuf = MakeUniqueIOBuf(tmp.length(), false);
  auto dp = rbuf->GetMutDataPointer();
  std::memcpy(static_cast<void*>(dp.Data()), tmp.data(), tmp.length());
  ebbrt::kprintf_force("Core: %u TcpSession::Receive() message:%s\n", mcore, s.c_str());

  Send(std::move(rbuf));
}


