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
//#include <sys/socket.h>

//#include 
#include "UdpCommand.h"

#define MAXS 262144
#define MCPU 1

#define WITHLOGGING

//struct IxgbeLog ixgbe_logs[16];

/*#define SENDER_PORT_NUM 12344
#define SENDER_IP_ADDR "192.168.1.8"

#define SERVER_PORT_NUM 12345
#define SERVER_IP_ADDRESS "192.168.1.153"

struct ITR_STATS {
  uint64_t joules;
  uint64_t itr_time_us;
  
  uint32_t rx_desc;
  uint64_t rx_bytes;
  uint32_t tx_desc;
  uint64_t tx_bytes;
  
  uint64_t ninstructions;
  uint64_t ncycles;
  uint64_t nllc_miss;
};
*/

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
    ebbrt::kprintf_force("TcpSender Connected()\n");
  }
  void TestSend() {
    char send_buffer[32];
    snprintf(send_buffer, sizeof(send_buffer), "hello");
    auto newbuf = ebbrt::MakeUniqueIOBuf(sizeof(send_buffer), false);
    std::memcpy(static_cast<void*>(newbuf->MutData()), send_buffer, strlen(send_buffer));
    ebbrt::kprintf_force("send_buffer = %s\n", send_buffer);
    Send(std::move(newbuf));
    Pcb().Output();
  }
  
  std::unique_ptr<ebbrt::IOBuf> SendLarge(std::unique_ptr<ebbrt::IOBuf> buf, size_t& slen) {
    std::unique_ptr<ebbrt::IOBuf> split;
    size_t length = 0;
    size_t nchains = 0;
    
    for(auto& b : *buf) {
      length += b.Length();
      nchains ++;
      if (length > MAXS || nchains > 37) {
	auto tmp = static_cast<ebbrt::MutIOBuf*>(buf->UnlinkEnd(b).release());
	split = std::unique_ptr<ebbrt::MutIOBuf>(tmp);
	break;
      }
    }
    
    slen = buf->ComputeChainDataLength();
    //ebbrt::kprintf_force("sending chains=%d len=%d\n", buf->CountChainElements(), slen);
    Send(std::move(buf));
    Pcb().Output();
    return split;
  }
  
  void SendLog() {
    uint32_t v = MCPU;	  
    uint8_t* re = (uint8_t*)&ixgbe_logs[v];    
    uint64_t msg_size = sizeof(ixgbe_logs[v]);
    uint64_t sum = 0;
    
    for(uint64_t i = 0; i < msg_size; i++) {
      sum += re[i];
    }
    
    /*for(uint64_t i = 51200000; i < msg_size; i+=8) {
      ebbrt::kprintf_force("0x%X 0x%X 0x%X 0x%X 0x%X 0x%X 0x%X 0x%X\n",
        		   re[i], re[i+1], re[i+2], re[i+3], re[i+4], re[i+5], re[i+6], re[i+7]);
			   }*/
    
    struct IxgbeLog *il = (struct IxgbeLog *)re;
    ebbrt::kprintf_force("SendLog() il->itr_cnt=%d msg_size=%d sum=%lu\n", il->itr_cnt, msg_size, sum);
    
    /*auto buf = std::make_unique<ebbrt::StaticIOBuf>(re, msg_size);
    Send(std::move(buf));
    Pcb().Output();*/
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
    

    //auto newbuf = ebbrt::MakeUniqueIOBuf(sizeof(ixgbe_logs[v]), false);
    //std::memcpy(static_cast<void*>(newbuf->MutData()), re, sizeof(ixgbe_logs[v]));    
    //Send(std::move(newbuf));
    //Pcb().Output();
    
    /*size_t total_len = 0;
    size_t slen = 0;
    
    std::unique_ptr<ebbrt::IOBuf> tmp;
    tmp = SendLarge(std::move(newbuf), slen)    total_len = slen;
    
    while(total_len < msg_size) {
      tmp = SendLarge(std::move(tmp), slen);
      total_len += slen;
      }*/
  }
  
  void Receive(std::unique_ptr<ebbrt::MutIOBuf> b) override {
  }
  
  void Close() override {
    Pcb().Disconnect();
  }
  void Abort() override {}
  
private:
  ebbrt::NetworkManager::TcpPcb pcb_;
};


ebbrt::NetworkManager::TcpPcb tpcb;
std::unique_ptr<TcpSender> handler;
//TcpSender* handler;

namespace ebbrt {
  
  class TcpStat : public StaticSharedEbb<TcpStat>, public CacheAligned {
  public:
    TcpStat() {};
    void Start(uint16_t port) {
      listening_pcb_.Bind(port, [this](NetworkManager::TcpPcb pcb) {
	  // new connection callback
	  static std::atomic<size_t> cpu_index{MCPU};
	  pcb.BindCpu(MCPU);
	  auto connection = new TcpStatSession(this, std::move(pcb));
	  connection->Install();
	});
    }

  private:
    class TcpStatSession : public ebbrt::TcpHandler {
    public:
      TcpStatSession(TcpStat *mcd, ebbrt::NetworkManager::TcpPcb pcb)
        : ebbrt::TcpHandler(std::move(pcb)), mcd_(mcd) {	
      }
      void Close() {}
      void Abort() {}
      
      void Receive(std::unique_ptr<MutIOBuf> b) {
	kassert(b->Length() != 0);		  
	std::string s(reinterpret_cast<const char*>(b->MutData()));
	char com = (s.c_str())[0];
	
	switch(com) {
	case 'a':
	{
	  network_manager->Config("start_stats", MCPU);
	  Send(std::move(b));
	  break;
	}
	case 'b':
	{
	  network_manager->Config("stop_stats", MCPU);
	  Send(std::move(b));
	  break;
	}
	case 'c':
	{
	  network_manager->Config("clear_stats", MCPU);
	  Send(std::move(b));
	  break;
	}
	case 'd':
	{
	  //ebbrt::kprintf_force("Received %c %d\n", com, s.length());
	  Send(std::move(b));
	  break;
	}
	case 'e':
	{
	  uint32_t v = MCPU;	  
	  uint8_t* re = (uint8_t*)&ixgbe_logs[v];
	  for(size_t i = 0; i < 128; i+=8) {
	    ebbrt::kprintf_force("0x%X 0x%X 0x%X 0x%X 0x%X 0x%X 0x%X 0x%X \n",
				 re[i], re[i+1], re[i+2], re[i+3], re[i+4], re[i+5], re[i+6], re[i+7]);
	  }
	  struct IxgbeLog *il = (struct IxgbeLog *)re;
	  ebbrt::kprintf_force("il->itr_cnt = %d\n", il->itr_cnt);	  
	  for (uint32_t i = 0; i < il->itr_cnt; i++) {
	    union IxgbeLogEntry *ile = &il->log[i];   
	    ebbrt::kprintf_force("%d %d %d %d %d %llu %llu %llu %llu %llu\n",
				 i,
				 ile->Fields.rx_desc, ile->Fields.rx_bytes,
				 ile->Fields.tx_desc, ile->Fields.tx_bytes,
				 ile->Fields.ninstructions,
				 ile->Fields.ncycles,
				 ile->Fields.nllc_miss,		   
				 ile->Fields.joules,		   
				 ile->Fields.tsc);
	  }
	  
	  auto vnum = std::make_unique<StaticIOBuf>(re, sizeof(ixgbe_logs[v]));
	  Send(std::move(vnum));
	  break;
	}
	case 'f':
	{
	  size_t i = 0;
	  ebbrt::event_manager->SpawnRemote(
	    [this, i] () mutable {	      	      
	      handler->TestSend();
	    }, i);	  
	  break;
	}
	case 'g':
	{
	  /*size_t i = 0;
	  ebbrt::event_manager->SpawnRemote(
	    [this, i] () mutable {	      	      
	      handler->SendLog();
	      }, i);*/
	  handler->SendLog();
	  Send(std::move(b));
	  break;
	}
	}
      }
      
    private:
      ebbrt::NetworkManager::TcpPcb pcb_;
      TcpStat *mcd_;
    };
    NetworkManager::ListeningTcpPcb listening_pcb_;  
  };
  
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
      case SYNC: {
#ifdef WITHLOGGING
	network_manager->Config("stop_stats", MCPU);
	//struct IxgbeLog *il;
	//union IxgbeLogEntry *ile;
	//uint64_t rxb = 0, txb = 0;
	//uint32_t v = MCPU;	

	//ebbrt::kprintf_force("STATS_NAME ITER ITR MSG DVFS RAPL WORK_START WORK_END\n");
	//ebbrt::kprintf_force("i rx_desc rx_bytes tx_desc tx_bytes instructions cycles llc_miss joules timestamp\n");
	//ebbrt::kprintf_force("PRINT_STATS_START %d %d %d 0x%X %d %llu %llu\n", iter_, itr_*2, msg_sizes_, dvfs_, rapl_, work_start, work_end);
	//il= &ixgbe_logs[v];
	
	/*for (int i = 0; i < il->itr_cnt; i++) {
	  ile = &il->log[i];
	  rxb += ile->Fields.rx_bytes;
	  txb += ile->Fields.tx_bytes;
	  
	  ebbrt::kprintf_force("%d %d %d %d %d %llu %llu %llu %llu %llu\n",
			       i,
			       ile->Fields.rx_desc, ile->Fields.rx_bytes,
			       ile->Fields.tx_desc, ile->Fields.tx_bytes,
			       ile->Fields.ninstructions,
			       ile->Fields.ncycles,
			       ile->Fields.nllc_miss,		   
			       ile->Fields.joules,		   
			       ile->Fields.tsc);

			       ebbrt::clock::SleepMilli(3);
	  }*/
	//ebbrt::kprintf_force("Core=%u itr_cnt=%lu total_rx_bytes=%lu rxb=%lu total_tx_bytes=%lu txb=%lu work_start=%llu work_end=%llu\n", v, il->itr_cnt, ixgbe_dev->ixgmq[v]->total_rx_bytes, rxb, ixgbe_dev->ixgmq[v]->total_tx_bytes, txb, work_start, work_end);
	//ebbrt::kprintf_force("Core=%u itr_cnt=%lu\n", v, il->itr_cnt);
	//ebbrt::kprintf_force("PRINT_STATS_END %d %d %d 0x%X %d\n", iter_, itr_*2, msg_sizes_, dvfs_, rapl_);

	/*size_t i = 0;
	ebbrt::Promise<void> p;
	auto f = p.GetFuture();
	ebbrt::event_manager->SpawnRemote(
	  [this, i, &p] () mutable {
	  handler->SendLog();
	  p.SetValue();
	  }, i);
	  f.Block();*/
	handler->SendLog();
	ebbrt::clock::SleepMilli(5000);
	
	//network_manager->Config("print_stats", MCPU);
	//ebbrt::kprintf_force("%d\n", ixgbe_logs[1].itr_cnt);
	//ebbrt::clock::SleepMilli(5000);
	/*ebbrt::Promise<void> p;
	auto f = p.GetFuture();
	event_manager->SpawnLocal(
	  [this, &p] () mutable {	      	    
	    //uint8_t* re = (uint8_t*)&ixgbe_logs[v];
	    //auto vnum = std::make_unique<StaticIOBuf>(re, sizeof(ixgbe_logs[v]));
	    //up.SendTo(ebbrt::Ipv4Address({192, 168, 1, 153}), 8080, std::move(vnum));
	    ebbrt::NetworkManager::UdpPcb up;
	    up.Bind(0);		    
	    int len = sizeof(ixgbe_logs[MCPU]);
	    auto newbuf = ebbrt::MakeUniqueIOBuf(len, false);
	    std::memcpy(static_cast<void*>(newbuf->MutData()), static_cast<void*>(&ixgbe_logs[v]), len);
	    ebbrt::kprintf_force("len = %d\n", len);
	    up.SendTo(ebbrt::Ipv4Address({192, 168, 1, 153}), 8888, std::move(newbuf));
	    uint8_t* re = (uint8_t*)&ixgbe_logs[v];
	    for(size_t i = 0; i < 1024; i+=8) {
	      ebbrt::kprintf_force("0x%X 0x%X 0x%X 0x%X 0x%X 0x%X 0x%X 0x%X \n",
				   re[i], re[i+1], re[i+2], re[i+3], re[i+4], re[i+5], re[i+6], re[i+7]);
	    }	    	  
	    p.SetValue();
	  });
	f.Block();      		
	*/
	//network_manager->Config("clear_stats", MCPU);
#endif
	//ebbrt::kprintf_force("pk0=%lf pk1=%lf work_start=%llu work_end=%llu pk0_start=%llu pk0_end=%llu pk1_start=%llu pk1_end=%llu\n\n", (pk0j_end_-pk0j_start_)*rapl_cpu_energy_units, (pk1j_end_-pk1j_start_)*rapl_cpu_energy_units, work_start, work_end, pk0j_start_, pk0j_end_, pk1j_start_, pk1j_end_);
	
	if (b->ComputeChainDataLength() != strlen(sync_string)) {
	  std::string s(reinterpret_cast<const char*>(b->MutData()));
	  ebbrt::kabort("Didn't receive full sync string %d %s\n",
			b->ComputeChainDataLength(), s.c_str());
	} 
	/*auto dp = b->GetDataPointer();
	  auto str = reinterpret_cast<const char*>(dp.Get(strlen(sync_string)));
	if (strncmp(sync_string, str, strlen(sync_string))) {
	  std::string s(reinterpret_cast<const char*>(b->MutData()));
	  ebbrt::kabort("%d Synchronization String Incorrect! %s\n", b->ComputeChainDataLength(), s.c_str());
	  }*/

	///ebbrt::kprintf_force("Received %s\n", s.c_str());
	//auto buf = ebbrt::IOBuf::Create<ebbrt::StaticIOBuf>(sync_string);
	Send(std::move(b));
	//Pcb().Output();
	state_ = REPEAT;
	break;
      }
      case REPEAT: {
	if (b->ComputeChainDataLength() < (sizeof(msg_sizes_)
					   +sizeof(repeat_)
					   +sizeof(dvfs_)+sizeof(rapl_)
					   +sizeof(itr_) +sizeof(iter_)
					   + (2*sizeof(float)))) {
	  ebbrt::kabort("Didn't receive full repeat\n");
	}
	//ebbrt::kprintf_force("+++ repeat=%d msg_sizes=%d dvfs=0x%X rapl=%d itr=%d pk0=%lf pk1=%lf ", repeat_, msg_sizes_, dvfs_, rapl_, itr_*2, (pk0j_end_-pk0j_start_)*rapl_cpu_energy_units, (pk1j_end_-pk1j_start_)*rapl_cpu_energy_units);
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
	
	//ebbrt::kprintf_force("%d %d 0x%X %d ", itr_*2, msg_sizes_, dvfs_, rapl_);
	ebbrt::kprintf_force("CLIENT_STATS %d ", iter_);
	auto dp = b->GetDataPointer();
	msg_sizes_ = dp.Get<size_t>();
	repeat_ = dp.Get<size_t>();
	dvfs_ = dp.Get<size_t>();
	rapl_ = dp.Get<size_t>();
	itr_ = dp.Get<size_t>();
	iter_ = dp.Get<size_t>();
	float tput = dp.Get<float>();
	float lat = dp.Get<float>();
	
	count_ = 0;
	//ebbrt::kprintf_force("tput=%f lat=%f\n", tput, lat);
	ebbrt::kprintf_force("%f %f %lf %lf %llu %llu\n", tput, lat, pk0*rapl_cpu_energy_units, pk1*rapl_cpu_energy_units);
	ebbrt::kprintf_force("CLIENT_NAME ITER TPUT LAT PK0J PK1J\n\n");
	//ebbrt::kprintf_force("+++ Core %u: repeat=%d msg_sizes=%d dvfs=0x%X rapl=%d itr=%d +++\n", static_cast<uint32_t>(ebbrt::Cpu::GetMine())
	//, repeat_, msg_sizes_, dvfs_, rapl_, itr_*2);

	for (uint32_t i = 0; i < static_cast<uint32_t>(ebbrt::Cpu::Count()); i++) {
	  ebbrt::Promise<void> p;
	  auto f = p.GetFuture();
	  event_manager->SpawnRemote(
	    [this, i, &p] () mutable {	      	      
	      if(i == 0 || i == 1) {
		ebbrt::rapl::RaplCounter powerMeter;
		powerMeter.SetLimit(rapl_);		
	      }
	      if(i == 1) {
		// same p state as Linux with performance governor
		ebbrt::msr::Write(IA32_PERF_CTL, dvfs_);		
	      }
	      network_manager->Config("rx_usecs", itr_);
	      p.SetValue();
	    }, i);
	  f.Block();
	}	
	
	Send(std::move(b));
	Pcb().Output();
	
#ifdef WITHLOGGING
	network_manager->Config("start_stats", MCPU);
#endif	
	state_ = RPC;
	break;
      }
      case RPC: {
	// packet has already received
	//ebbrt::kprintf_force("RPC len=%d\n", b->ComputeChainDataLength());
	if(buf_) {
	  buf_->PrependChain(std::move(b));	  
	} else {
	  buf_ = std::move(b);
	}

	auto chain_len = buf_->ComputeChainDataLength();	
	if(chain_len == msg_sizes_) {
	  
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
	  
	  count_ += 1;
	  //ebbrt::kprintf_force("repeat=%d chain_len=%d chain_elements=%d\n", repeat_, chain_len, buf_->CountChainElements());
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
	
	if(repeat_ == count_) {
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
    size_t count_;
    size_t msg_sizes_;
    size_t dvfs_{0};
    size_t rapl_{0};
    size_t itr_{0};
    size_t iter_{0};
    uint64_t work_start;
    uint64_t work_end;
    uint64_t pk0j_start_{0};
    uint64_t pk1j_start_{0};
    uint64_t pk0j_end_{0};
    uint64_t pk1j_end_{0};
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
	p.SetValue();
      }, i);
    f.Block();
  }
  
  ebbrt::event_manager->SpawnRemote(
    [] () mutable {
      //ebbrt::NetworkManager::TcpPcb tpcb;            
      //auto handler = new ebbrt::TcpSender(std::move(tpcb));
      //handler = new ebbrt::TcpSender(std::move(tpcb));
      //handler->Install();
      
      //auto id2 = ebbrt::ebb_allocator->AllocateLocal();
      //auto mc2 = ebbrt::EbbRef<ebbrt::TcpStat>(id2);
      //mc2->Start(8080);
      //ebbrt::kprintf("TcpStat server listening on port %d\n", 8080);
      
      /*ebbrt::NetworkManager::UdpPcb up;
      up.Bind(0);
      char send_buffer[32];
      snprintf(send_buffer, sizeof(send_buffer), "start_hello");
      auto newbuf = ebbrt::MakeUniqueIOBuf(sizeof(send_buffer), false);
      std::memcpy(static_cast<void*>(newbuf->MutData()), send_buffer, sizeof(send_buffer));
      ebbrt::kprintf_force("send_buffer = %s\n", send_buffer);
      up.SendTo(ebbrt::Ipv4Address({192, 168, 1, 153}), 8888, std::move(newbuf));
      */
      auto id = ebbrt::ebb_allocator->AllocateLocal();
      auto mc = ebbrt::EbbRef<ebbrt::TcpCommand>(id);
      mc->Start(5002);
      ebbrt::kprintf("TcpCommand server listening on port %d\n", 5002);

      tpcb.Connect(ebbrt::Ipv4Address({192, 168, 1, 153}), 8888);
      handler.reset(new TcpSender(std::move(tpcb)));
      handler->Install();
      //handler->TestSend();
    }, MCPU);
}
