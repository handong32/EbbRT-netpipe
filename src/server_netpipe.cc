//          Copyright Boston University SESA Group 2013 - 2020.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
#include <cfloat>
#include <cstdlib>
#include <sstream>
#include <memory>
#include <mutex>

#include <ebbrt/native/Cpu.h>
#include <ebbrt/native/Clock.h>
#include <ebbrt/EventManager.h>
#include <ebbrt/native/NetTcpHandler.h>
#include <ebbrt/SharedIOBufRef.h>
#include <ebbrt/StaticIOBuf.h>
#include <ebbrt/UniqueIOBuf.h>
#include <ebbrt/AtomicUniquePtr.h>
#include <ebbrt/CacheAligned.h>
#include <ebbrt/SpinLock.h>
#include <ebbrt/native/Net.h>
#include <ebbrt/native/Msr.h>
#include <ebbrt/native/RcuTable.h>

#include "UdpCommand.h"

#define MAXS 262144
#define MCPU 1

namespace {
const constexpr char sync_string[] = "SyncMe";
}

namespace ebbrt {
class TcpCommand : public StaticSharedEbb<TcpCommand>, public CacheAligned {
public:
  TcpCommand() {};
  void Start(uint16_t port) {
    listening_pcb_.Bind(port, [this](NetworkManager::TcpPcb pcb) {
	// new connection callback
	static std::atomic<size_t> cpu_index{MCPU};
	pcb.BindCpu(MCPU);
	auto connection = new TcpSession(this, std::move(pcb));
	connection->Install();
	//ebbrt::kprintf_force("Core %u: Connection created\n", static_cast<uint32_t>(ebbrt::Cpu::GetMine()));
  });
  }

private:
  class TcpSession : public ebbrt::TcpHandler {
  public:
    TcpSession(TcpCommand *mcd, ebbrt::NetworkManager::TcpPcb pcb)
        : ebbrt::TcpHandler(std::move(pcb)), mcd_(mcd) {
      state_ = SYNC;
      repeat_ = 0;
      msg_sizes_ = 0;
      buf_ = nullptr;
    }
    void Close() {}
    void Abort() {}
    
    void Receive(std::unique_ptr<MutIOBuf> b) {
      kassert(b->Length() != 0);
      std::string s(reinterpret_cast<const char*>(b->MutData()));
      
      switch(state_) {
      case SYNC: {
	network_manager->Config("stop_stats", MCPU);
	network_manager->Config("print_stats", MCPU);
	network_manager->Config("clear_stats", MCPU);
	ebbrt::kprintf_force("+++++++++++ PRINT COMPLETE ++++++++++\n");
	
	if (b->ComputeChainDataLength() < strlen(sync_string)) {
	  ebbrt::kabort("Didn't receive full sync string\n");
	}
	auto dp = b->GetDataPointer();
	auto str = reinterpret_cast<const char*>(dp.Get(strlen(sync_string)));
	if (strncmp(sync_string, str, strlen(sync_string))) {
	  ebbrt::kabort("Synchronization String Incorrect!\n");
	}

	///ebbrt::kprintf_force("Received %s\n", s.c_str());
	//auto buf = ebbrt::IOBuf::Create<ebbrt::StaticIOBuf>(sync_string);
	Send(std::move(b));
	Pcb().Output();
	state_ = REPEAT;
	break;
      }
      case REPEAT: {
	if (b->ComputeChainDataLength() < (sizeof(msg_sizes_)+sizeof(repeat_))) {
	  ebbrt::kabort("Didn't receive full repeat\n");
	}
	auto dp = b->GetDataPointer();
	msg_sizes_ = dp.Get<size_t>();
	repeat_ = dp.Get<size_t>();

	ebbrt::kprintf_force("++++++++++++++++++ Core %u: repeat=%d msg_sizes=%d+++++++++++++++++\n", static_cast<uint32_t>(ebbrt::Cpu::GetMine()), repeat_, msg_sizes_);
	Send(std::move(b));
	Pcb().Output();

	network_manager->Config("start_stats", MCPU);
	
	state_ = RPC;
	break;
      }
      case RPC: {
	//ebbrt::kprintf_force("RPC len=%d\n", b->ComputeChainDataLength());
	if(buf_) {
	  buf_->PrependChain(std::move(b));	  
	} else {
	  buf_ = std::move(b);
	}

	auto chain_len = buf_->ComputeChainDataLength();
	if(chain_len == msg_sizes_) {
	  repeat_ -= 1;
	  //ebbrt::kprintf_force("repeat=%d chain_len=%d chain_elements=%d\n", repeat_, chain_len, buf_->CountChainElements());
	  //
	  //Send(std::move(buf_));
	  //buf_ = nullptr;

	  if(msg_sizes_ > MAXS) {
	    size_t tmp_len = msg_sizes_;
	    while(tmp_len > MAXS) {
	      auto buf = ebbrt::MakeUniqueIOBuf(MAXS);
	      memset(buf->MutData(), 'b', MAXS);
	      Send(std::move(buf));
	      tmp_len -= MAXS;
	    }
	    
	    if(tmp_len) {
	      auto buf = ebbrt::MakeUniqueIOBuf(tmp_len);
	      memset(buf->MutData(), 'b', tmp_len);
	      Send(std::move(buf));
	      tmp_len -= tmp_len;
	    }	    
	  } else {
	    //auto rbuf = ebbrt::MakeUniqueIOBuf(msg_sizes_);
	    //memset(rbuf->MutData(), 'b', msg_sizes_);
	    //Send(std::move(rbuf));
	    Send(std::move(buf_));
	  }
	  buf_ = nullptr;
	}
	
	if(repeat_ == 0) {
	  state_ = SYNC;	  	  
	}
	
	break;
      }
      }
    }
      
    
  private:
    std::unique_ptr<ebbrt::MutIOBuf> buf_;
    ebbrt::NetworkManager::TcpPcb pcb_;
    TcpCommand *mcd_;
    enum states { SYNC, RPC, REPEAT};
    enum states state_;
    size_t repeat_;
    size_t msg_sizes_;
  };

  NetworkManager::ListeningTcpPcb listening_pcb_;  
}; 
}

void AppMain() {
  for (uint32_t i = 0; i < static_cast<uint32_t>(ebbrt::Cpu::Count()); i++) {
    ebbrt::event_manager->SpawnRemote(
      [i] () mutable {
	// disables turbo boost, thermal control circuit
	ebbrt::msr::Write(IA32_MISC_ENABLE, 0x4000850081);
	// same p state as Linux with performance governor
	ebbrt::msr::Write(IA32_PERF_CTL, 0x1D00);
	ebbrt::kprintf_force("Core %u: applied 0x1D00\n", i);
      }, i);
  }
  ebbrt::clock::SleepMilli(1000);
  
  ebbrt::event_manager->SpawnRemote(
    [] () mutable {
      auto uid = ebbrt::ebb_allocator->AllocateLocal();
      auto udpc = ebbrt::EbbRef<ebbrt::UdpCommand>(uid);
      udpc->Start(6666);
      ebbrt::kprintf("Core %u: UdpCommand server listening on port %d\n", static_cast<uint32_t>(ebbrt::Cpu::GetMine()), 6666);
      
      auto id = ebbrt::ebb_allocator->AllocateLocal();
      auto mc = ebbrt::EbbRef<ebbrt::TcpCommand>(id);
      mc->Start(5002);
      ebbrt::kprintf("TcpCommand server listening on port %d\n", 5002);
    }, MCPU);
}

	
// 	// Device level packet fragmentation logic
// 	/*size_t tmp_len = msg_sizes_;	
// 	if(tmp_len > MAXS) {
	  
	  
// 	  while(tmp_len > MAXS) {
// 	    auto buf = ebbrt::MakeUniqueIOBuf(MAXS, true);
// 	    //memset(buf->MutData(), 'b', MAXS);
// 	    Send(std::move(buf));
// 	    tmp_len -= MAXS;
// 	  }
	  
// 	  if(tmp_len) {
// 	    auto buf = ebbrt::MakeUniqueIOBuf(tmp_len, true);
// 	    //memset(buf->MutData(), 'b', tmp_len);
// 	    Send(std::move(buf));
// 	    tmp_len -= tmp_len;
// 	  }	    
// 	  }*/
	
	
	
// 	/*if(repeat_ == 0) {	  
// 	  state_ = SYNC;
// 	  ebbrt::kprintf_force("In SYNC state again\n");
// 	  }*/
//       }
 

      /*if (b->ComputeChainDataLength() > 4) {
	if(s == "SyncMe" || s == "QUIT") {
	Send(std::move(b));
	} else {	
	  auto rbuf = MakeUniqueIOBuf(b->ComputeChainDataLength(), false);
	  auto dp = rbuf->GetMutDataPointer();
	
	  std::memcpy(static_cast<void*>(dp.Data()), b->MutData(), b->ComputeChainDataLength());
	  //ebbrt::kprintf_force("b->len=%d rbuf->len=%d s=%s\n", b->ComputeChainDataLength(), rbuf->ComputeChainDataLength(), s.c_str());
	  Send(std::move(rbuf));
	}
	}*/


/*


*/

/*
      //auto dp = b->GetDataPointer();
      //auto str = reinterpret_cast<const char*>(dp.Get(b->Length()));
      //std::string s(reinterpret_cast<const char*>(b->MutData()));
      auto len = b->ComputeChainDataLength();
      if(len > 4 && len < 60) {
	Send(std::move(b));
	Pcb().Output();
	msg_sizes_ = msgs;
	buf_ = nullptr;
      } else {	
	if(buf_) {	  
	  // more to receive, append to chain
	  buf_->PrependChain(std::move(b));	  
	} else {
	  // first packet in
	  buf_ = std::move(b);
	}

	auto chain_len = buf_->ComputeChainDataLength();
	
	// application packet size fragmentation logic
	if(chain_len >= msg_sizes_) { 
	  if (unlikely(chain_len > msg_sizes_)) {	    
	    //ebbrt::kprintf_force("chain_len = %d msg_sizes_ = %d\n", chain_len, msg_sizes_);
	    kassert(chain_len == (msg_sizes_ + strlen(sync_string)));
	    // mark us as having received the sync and trim off that part
	    //synced_ = true;
	    auto tail = buf_->Prev();
	    auto to_trim = strlen(sync_string);

	    // TODO: Throwing out tail
	    while (to_trim > 0) {
	      auto trim = std::max(tail->Length(), to_trim);
	      tail->TrimEnd(trim);
	      to_trim -= trim;
	      tail = tail->Prev();
	    }
	  }
	  kassert(chain_len < MAXS);
	  Send(std::move(buf_));
	  Pcb().Output();
	}
      }            

 */
