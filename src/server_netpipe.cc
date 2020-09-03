//          Copyright Boston University SESA Group 2013 - 2020.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
#include <cfloat>
#include <cstdlib>
#include <sstream>
#include <memory>
#include <mutex>
#include <errno.h>

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
#include <ebbrt/Future.h>
#include <ebbrt/native/Net.h>
#include <ebbrt/native/Msr.h>
#include <ebbrt/native/RcuTable.h>
#include <ebbrt/native/IxgbeDriver.h>

#include "TcpServer.h"

char* data_logs[10];

//#define MAXS 16384
#define MAXS 262144
#define MCPU 1

#define WITHLOGGING

namespace {
const constexpr char sync_string[] = "SyncMe";
}

bool send_stat = false;

class TcpSender : public ebbrt::TcpHandler {
public:
  TcpSender(ebbrt::NetworkManager::TcpPcb pcb)
    : ebbrt::TcpHandler(std::move(pcb)) {
  }
  
  void Connected() override {
    ebbrt::kprintf_force("** TcpSender Connected()\n");
  }
    
  void SendLog() {
    uint8_t* re = (uint8_t*)&ixgbe_logs;    
    uint64_t msg_size = sizeof(ixgbe_logs);    
    uint64_t sum = 0;

    for(uint64_t i = 0; i < msg_size; i++) {
      sum += re[i];
    }
    ebbrt::kprintf_force("SendLog: msg_size=%llu sum=%llu\n", msg_size, sum);
    
    while(msg_size > MAXS) {
      auto buf = std::make_unique<ebbrt::StaticIOBuf>(re, MAXS);
      Send(std::move(buf));      
      msg_size -= MAXS;
      re += MAXS;
    }
    
    if(msg_size) {
      auto buf = std::make_unique<ebbrt::StaticIOBuf>(re, msg_size);
      Send(std::move(buf));
    }

    Pcb().Output();
  }
  
  void Receive(std::unique_ptr<ebbrt::MutIOBuf> b) override {}
  
  void Close() override {
    //Pcb().Disconnect();
  }
  void Abort() override {}
  
private:
  ebbrt::NetworkManager::TcpPcb pcb_;
};


ebbrt::NetworkManager::TcpPcb tpcb;
std::unique_ptr<TcpSender> handler;
//TcpSender* handler;

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
      rapl_cpu_energy_units = 0.00001526;
    }
    void Close() {}
    void Abort() {}

    std::unique_ptr<IOBuf> SendLarge(std::unique_ptr<IOBuf> buf, size_t& slen) {
      std::unique_ptr<IOBuf> split;
      size_t length = 0;
      size_t nchains = 0;

      for(auto& b : *buf) {
	length += b.Length();
	nchains ++;
	if (length > MAXS || nchains > 37) {
	  auto tmp = static_cast<MutIOBuf*>(buf->UnlinkEnd(b).release());
	  split = std::unique_ptr<MutIOBuf>(tmp);
	  break;
	}
      }
      
      slen = buf->ComputeChainDataLength();
      //ebbrt::kprintf_force("sending chains=%d len=%d\n", buf->CountChainElements(), slen);
      Send(std::move(buf));
      return split;
    }
    
    void Receive(std::unique_ptr<MutIOBuf> b) {
      kassert(b->Length() != 0);      
      
      switch(state_) {
      case DATA: {
#ifdef WITHLOGGING	
	for (uint32_t i = 0; i < static_cast<uint32_t>(ebbrt::Cpu::Count()); i++) {
	  ebbrt::Promise<void> p;
	  auto f = p.GetFuture();
	  event_manager->SpawnRemote(
	    [this, i, &p] () mutable {
	      if(i == 1) {
		// set DVFS - same p state as Linux with performance governor
		ebbrt::msr::Write(IA32_PERF_CTL, 0x1d00);
	      }
	      // set ITR
	      network_manager->Config("rx_usecs", 4);	      
	      p.SetValue();
	    }, i);
	  f.Block();
	}

	if (b->ComputeChainDataLength() < (2*sizeof(float))) {
	  ebbrt::kabort("Didn't receive full repeat\n");
	}
	struct IxgbeLog *il = (struct IxgbeLog *)&ixgbe_logs;
	auto dp = b->GetDataPointer();
	float tput = dp.Get<float>();
	float lat = dp.Get<float>();
	il->tput = tput;
	il->lat = lat;
	
	uint64_t pk0, pk1;
	if(pk0j_end_ < pk0j_start_) {
	  pk0 = (UINT32_MAX+pk0j_end_) - pk0j_start_;
	} else {
	  pk0 = pk0j_end_ - pk0j_start_;
	}

	if(pk1j_end_ < pk1j_start_) {
	  pk1 = (UINT32_MAX+pk1j_end_) - pk1j_start_;
	} else {
	  pk1 = pk1j_end_ - pk1j_start_;
	}
	
	il->pk0j = pk0*rapl_cpu_energy_units;
	il->pk1j = pk1*rapl_cpu_energy_units;
	il->work_start = work_start;
	il->work_end = work_end;
	
	uint8_t* re = (uint8_t*)(&ixgbe_logs);    
	uint64_t msg_size = sizeof(ixgbe_logs);    
	uint64_t sum = 0;
	uint64_t sum_rxb, sum_txb;
	sum_rxb = sum_txb = 0;	
	for(uint64_t i = 0; i < msg_size; i++) {
	  sum += re[i];
	}
	for(uint64_t i = 0; i < ixgbe_logs.itr_cnt; i++) {
	  sum_rxb += ixgbe_logs.log[i].Fields.rx_bytes;
	  sum_txb += ixgbe_logs.log[i].Fields.tx_bytes;
	}
		       	
	ebbrt::kprintf_force("SendLog: i=%u msg_size=%llu sum=%llu itr_cnt=%u sum_rxb=%llu sum_txb=%llu tput=%f lat=%f pk0=%f pk1=%f\n", il->iter, msg_size, sum, ixgbe_logs.itr_cnt, sum_rxb, sum_txb, il->tput, il->lat, il->pk0j, il->pk1j);

	/*data_logs[il->iter] = (char*)malloc(sizeof(ixgbe_logs) * sizeof(char));
	memcpy(data_logs[il->iter], &ixgbe_logs, sizeof(ixgbe_logs));
	sum = 0;
	re = (uint8_t*)(data_logs[il->iter]);    
	for(uint64_t i = 0; i < msg_size; i++) {
	  sum += re[i];
	}
	ebbrt::kprintf_force("SendLog: i=%u msg_size=%llu sum=%llu re=0x%X\n", il->iter, msg_size, sum, (void*)re);	*/
	ebbrt::clock::SleepMilli(1000);	
#endif	
	Send(std::move(b));
	state_ = SYNC;
	break;
      }
      case SYNC: {
	if (b->ComputeChainDataLength() != strlen(sync_string)) {
	  std::string s(reinterpret_cast<const char*>(b->MutData()));
	  ebbrt::kabort("Didn't receive full sync string %d %s\n",
			b->ComputeChainDataLength(), s.c_str());
	} 

	Send(std::move(b));
	state_ = REPEAT;
	break;
      }
      case REPEAT: {
	if (b->ComputeChainDataLength() < (sizeof(msg_sizes_)
					   +sizeof(repeat_)
					   +sizeof(dvfs_)+sizeof(rapl_)
					   +sizeof(itr_) +sizeof(iter_))) {
	  ebbrt::kabort("Didn't receive full repeat\n");
	}			
	
	auto dp = b->GetDataPointer();
	msg_sizes_ = dp.Get<size_t>();
	repeat_ = dp.Get<size_t>();
	dvfs_ = dp.Get<size_t>();
	rapl_ = dp.Get<size_t>();
	itr_ = dp.Get<size_t>();
	iter_ = dp.Get<size_t>();
	
	count_ = 0;
#ifdef WITHLOGGING	
	for (uint32_t i = 0; i < static_cast<uint32_t>(ebbrt::Cpu::Count()); i++) {
	  ebbrt::Promise<void> p;
	  auto f = p.GetFuture();
	  event_manager->SpawnRemote(
	    [this, i, &p] () mutable {
	      // set RAPL
	      if(i == 0 || i == 1) {
		ebbrt::rapl::RaplCounter powerMeter;
		powerMeter.SetLimit(rapl_);		
	      }
	      if(i == MCPU) {
		// set DVFS - same p state as Linux with performance governor
		ebbrt::msr::Write(IA32_PERF_CTL, dvfs_);
	      }

	      // set ITR
	      network_manager->Config("rx_usecs", itr_);
	      
	      p.SetValue();
	    }, i);
	  f.Block();
	}
#endif	
	Send(std::move(b));
	Pcb().Output();
	
#ifdef WITHLOGGING
	network_manager->Config("clear_stats", MCPU);
	network_manager->Config("start_stats", MCPU);
	struct IxgbeLog *il = (struct IxgbeLog *)&ixgbe_logs;
	il->msg_size = msg_sizes_;
	il->repeat = repeat_;
	il->dvfs = dvfs_;
	il->rapl = rapl_;
	il->itr = itr_*2;
	il->iter = iter_;
#endif	
	state_ = RPC;
	break;
      }
      case RPC: {
	// packet has already received
	if(buf_) {
	  buf_->PrependChain(std::move(b));	  
	} else {
	  buf_ = std::move(b);
	}

	auto chain_len = buf_->ComputeChainDataLength();	
	if(chain_len == msg_sizes_) {
#ifdef WITHLOGGING
	  // we receive a full packet
	  if(count_ == 0) {
	    work_start = ebbrt::rdtsc();
	    for (uint32_t i = 0; i < 2; i++) {
	      ebbrt::Promise<void> p;
	      auto f = p.GetFuture();
	      event_manager->SpawnRemote(
		[this, i, &p] () mutable {	      
		  ebbrt::rapl::RaplCounter powerMeter;
		  if(i == 0) {
		    pk0j_start_ = powerMeter.ReadMsr();
		  } else {
		    pk1j_start_ = powerMeter.ReadMsr();
		  }
		  p.SetValue();
		}, i);
	      f.Block();
	    }
	  }
	  if(count_ == repeat_-1) {
	    work_end = ebbrt::rdtsc();
	    for (uint32_t i = 0; i < 2; i++) {
	      ebbrt::Promise<void> p;
	      auto f = p.GetFuture();
	      event_manager->SpawnRemote(
		[this, i, &p] () mutable {	      
		  ebbrt::rapl::RaplCounter powerMeter;
		  if(i == 0) {
		    pk0j_end_ = powerMeter.ReadMsr();
		  } else {
		    pk1j_end_ = powerMeter.ReadMsr();
		  }
		  p.SetValue();
		}, i);
	      f.Block();
	    }
	  }
#endif
	  count_ += 1;	  
	  if(msg_sizes_ > MAXS) {
	    size_t total_len = 0;
	    size_t slen = 0;
	    std::unique_ptr<IOBuf> tmp;
	    tmp = SendLarge(std::move(buf_), slen);
	    total_len = slen;
	    
	    while(total_len < msg_sizes_) {
	      tmp = SendLarge(std::move(tmp), slen);
	      total_len += slen;
	    }
	  } else {
	    Send(std::move(buf_));
	  }
	  buf_ = nullptr;
	}

	// received all msgs, stop counting
	if(repeat_ == count_) {
#ifdef WITHLOGGING
	  network_manager->Config("stop_stats", MCPU);
#endif
	  state_ = DATA;	  	  
	}	
	break;
      }
      }
    }
          
  private:
    std::unique_ptr<ebbrt::MutIOBuf> buf_;
    ebbrt::NetworkManager::TcpPcb pcb_;
    TcpCommand *mcd_;
    enum states { SYNC, RPC, REPEAT, DATA};
    enum states state_;
    size_t repeat_;
    size_t count_;
    size_t msg_sizes_;
    size_t dvfs_{0};
    size_t rapl_{0};
    size_t itr_{0};
    size_t iter_{0};
    uint64_t work_start{0};
    uint64_t work_end{0};
    uint64_t pk0j_start_{0};
    uint64_t pk1j_start_{0};
    uint64_t pk0j_end_{0};
    uint64_t pk1j_end_{0};
    float pk0j{0.0};
    float pk1j{0.0};
    float rapl_cpu_energy_units;
  };

  NetworkManager::ListeningTcpPcb listening_pcb_;
}; 
}

void AppMain() {
  for (uint32_t i = 0; i < static_cast<uint32_t>(ebbrt::Cpu::Count()); i++) {
    ebbrt::Promise<void> p;
    auto f = p.GetFuture();
    ebbrt::event_manager->SpawnRemote(
      [i, &p] () mutable {
	// disables turbo boost, thermal control circuit
	ebbrt::msr::Write(IA32_MISC_ENABLE, 0x4000850081);
	// same p state as Linux with performance governor
	ebbrt::msr::Write(IA32_PERF_CTL, 0x1D00);
	ebbrt::kprintf_force("Core %u: applied 0x1D00\n", i);
	if(i == 0 || i == 1) {
	  ebbrt::rapl::RaplCounter powerMeter;
	  powerMeter.SetLimit(135);		
	}
	p.SetValue();
      }, i);
    f.Block();
  }
  
  ebbrt::event_manager->SpawnRemote(
    [] () mutable {
      //tpcb.Connect(ebbrt::Ipv4Address({192, 168, 1, 153}), 8888);
      //handler.reset(new TcpSender(std::move(tpcb)));
      //handler->Install();
      
      auto id = ebbrt::ebb_allocator->AllocateLocal();
      auto mc = ebbrt::EbbRef<ebbrt::TcpCommand>(id);
      mc->Start(5002);
      ebbrt::kprintf("TcpCommand server listening on port %d\n", 5002);
      
      auto id2 = ebbrt::ebb_allocator->AllocateLocal();
      auto tcps = ebbrt::EbbRef<ebbrt::TcpServer>(id2);
      tcps->Start(8889);
      ebbrt::kprintf("TcpServer listening on port %d\n", 8889);
        
    }, MCPU);  
}
