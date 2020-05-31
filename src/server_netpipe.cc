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
#include <ebbrt/native/Net.h>
#include <ebbrt/native/Msr.h>
#include <ebbrt/native/RcuTable.h>
#include <ebbrt/native/IxgbeDriver.h>
//#include <sys/socket.h>

//#include 
#include "UdpCommand.h"

#define MAXS 262144
#define MCPU 1
//#define WITHLOGGING

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

namespace ebbrt {

  //extern void NsPerTick();
  
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
      //std::string s(reinterpret_cast<const char*>(b->MutData()));
      
      switch(state_) {
      case SYNC: {
#ifdef WITHLOGGING
	network_manager->Config("stop_stats", MCPU);
	struct IxgbeLog *il;
	union IxgbeLogEntry *ile;
	uint64_t rxb = 0, txb = 0;
	uint32_t v = MCPU;	
	
	ebbrt::kprintf_force("++++++++++ PRINT_STATS START +++++++++++\n");
	il= &ixgbe_logs[v];
	ebbrt::kprintf_force("i rx_desc rx_bytes tx_desc tx_bytes instructions cycles llc_miss joules timestamp\n");
	for (int i = 0; i < il->itr_cnt; i++) {
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

	  ebbrt::clock::SleepMilli(2);	  
	}	
	ebbrt::kprintf_force("Core=%u itr_cnt=%lu total_rx_bytes=%lu rxb=%lu total_tx_bytes=%lu txb=%lu work_start=%llu work_end=%llu\n", v, il->itr_cnt, ixgbe_dev->ixgmq[v]->total_rx_bytes, rxb, ixgbe_dev->ixgmq[v]->total_tx_bytes, txb, work_start, work_end);
	ebbrt::kprintf_force("++++++++++ PRINT_STATS END +++++++++++\n");
	    
	//network_manager->Config("print_stats", MCPU);
	//ebbrt::kprintf_force("%d\n", ixgbe_logs[1].itr_cnt);
	ebbrt::clock::SleepMilli(5000);
	network_manager->Config("clear_stats", MCPU);
#endif
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
	count_ = 0;
	
	ebbrt::kprintf_force("++++++++++++++++++ Core %u: repeat=%d msg_sizes=%d+++++++++++++++++\n", static_cast<uint32_t>(ebbrt::Cpu::GetMine()), repeat_, msg_sizes_);
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
	  if(count_ == 0) { work_start = ebbrt::rdtsc(); }
	  if(count_ == repeat_-1) { work_end = ebbrt::rdtsc(); }
	  
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
	    //ebbrt::kprintf_force("total_len=%d\n", total_len);
	    
	    /*size_t tmp_len = msg_sizes_;
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
	      }*/	    
	  } else {
	    Send(std::move(buf_));
	  }
	  buf_ = nullptr;
	}
	
	if(repeat_ == count_) {
	  //network_manager->Config("print_stats", MCPU);
	  //auto re = static_cast<std::vector<ITR_STATS>>(network_manager->ReadItr());
	  //ebbrt::kprintf_force("len=%d\n", re.size());	  
	  //network_manager->Config("clear_stats", MCPU);

	  /*ebbrt::kprintf_force("Sending udp packet\n");
	    ebbrt::NetworkManager::UdpPcb up;
	    up.Bind(0);
	  auto newbuf = MakeUniqueIOBuf(6, false);
	  memset(newbuf->MutData(), 'c', 6);
	  up.SendTo(ebbrt::Ipv4Address({192, 168, 1, 153}), 8888, std::move(newbuf));*/
	  
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
    uint64_t work_start;
    uint64_t work_end;
  };

  NetworkManager::ListeningTcpPcb listening_pcb_;  
}; 
}

void AppMain() {

  //ebbrt::NsPerTick();
    
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

  /*char send_buffer[4096];
  
  void SendData() {
    ebbrt::NetworkManager::UdpPcb up;
    up.Bind(0);
    
    snprintf(send_buffer, sizeof(send_buffer), "Core=%u itr_cnt=%lu total_rx_bytes=%lu total_tx_bytes=%lu\n", MCPU, ixgbe_dev->ixgmq[MCPU]->itr_stats.size(), ixgbe_dev->ixgmq[MCPU]->total_rx_bytes, ixgbe_dev->ixgmq[MCPU]->total_tx_bytes);
    
    int len = strlen(send_buffer);    
    if((len & 0x1)) {
      kassert(len+1 < 4096);      
      char c = send_buffer[len];      
      send_buffer[len] = ' ';
      len ++;
      send_buffer[len] = c;
    }
    
    auto newbuf = MakeUniqueIOBuf(len, false);
    std::memcpy(static_cast<void*>(newbuf->MutData()), send_buffer, len);
    ebbrt::kprintf_force("send_buffer = %s\n", send_buffer);
    up.SendTo(ebbrt::Ipv4Address({192, 168, 1, 153}), 8888, std::move(newbuf));

    uint32_t c = 0;
    for(auto a : ixgbe_dev->ixgmq[MCPU]->itr_stats) {
      snprintf(send_buffer, sizeof(send_buffer),"%u %u %lu %u %lu %lu %lu %lu %lu %lu\n",
	       c,
	       a.rx_desc,
	       a.rx_bytes,
	       a.tx_desc,
	       a.tx_bytes,
	       a.ninstructions,
	       a.ncycles,
	       a.nllc_miss,
	       a.joules,
	       a.itr_time_us);

      len = strlen(send_buffer);    
      if((len & 0x1)) {
	kassert(len+1 < 4096);      
	c = send_buffer[len];      
	send_buffer[len] = ' ';
	len ++;
	send_buffer[len] = c;
      }
      
      newbuf = MakeUniqueIOBuf(len, false);
      std::memcpy(static_cast<void*>(newbuf->MutData()), send_buffer, len);
      ebbrt::kprintf_force("send_buffer = %s\n", send_buffer);
      up.SendTo(ebbrt::Ipv4Address({192, 168, 1, 153}), 8888, std::move(newbuf));
      
      c ++;
      if(c == 10) break;
      }*/
	  
    /*static int sockfd = -1;

    ebbrt::kprintf_force("SendData\n");

    if(sockfd == -1) {
      struct sockaddr_in localAddr;
      struct sockaddr_in dest_addr;
      
      memset(&localAddr, 0, sizeof(localAddr));
      memset(&dest_addr, 0, sizeof(struct sockaddr_in));
      
      // assign IP, PORT
      dest_addr.sin_family = AF_INET;
      dest_addr.sin_addr.s_addr = inet_addr("192.168.1.153");
      dest_addr.sin_port = htons(12345);
      
      sockfd = socket(AF_INET, SOCK_STREAM, 0);
      kassert(sockfd>=0);
      
      localAddr.sin_family = AF_INET;
      localAddr.sin_addr.s_addr = htonl(INADDR_ANY);
      localAddr.sin_port = htons(0);
      
      int rc = bind(sockfd, (struct sockaddr *)&localAddr, sizeof(localAddr));
      kassert(rc>=0);
    
    
      // connect the client socket to server socket
      if (connect(sockfd, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) < 0) {
	ebbrt::kabort("connection with the server failed...\n");
      }
      ebbrt::kprintf_force("Connected\n");
    }
    
    int n=write(sockfd, send_buffer, len);
    kassert(n==len);    */
//  }
