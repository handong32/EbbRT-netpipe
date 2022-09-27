#include <cstdlib>
#include <sstream>
#include <ebbrt/UniqueIOBuf.h>

#include "TcpServer.h"

ebbrt::TcpServer::TcpServer() {}

void ebbrt::TcpServer::Start(uint16_t port) {
  listening_pcb_.Bind(port, [this](NetworkManager::TcpPcb pcb) {
    // new connection callback
    static std::atomic<size_t> cpu_index{0};
    auto index = MCPU; //cpu_index.fetch_add(1) % ebbrt::Cpu::Count();
    pcb.BindCpu(index);
    auto connection = new TcpSession(this, std::move(pcb));
    connection->Install();
    ebbrt::kprintf_force("Core %u: TcpServer connection created\n", static_cast<uint32_t>(ebbrt::Cpu::GetMine()));
  });
}

void ebbrt::TcpServer::TcpSession::Receive(std::unique_ptr<MutIOBuf> b) {
  kassert(b->Length() != 0);

  //uint32_t ncores = static_cast<uint32_t>(ebbrt::Cpu::Count());  
  //uint32_t mcore = static_cast<uint32_t>(Cpu::GetMine());
  std::string s(reinterpret_cast<const char*>(b->Data()));
  std::string delimiter = ",";
  uint32_t param = 0;
  std::string token1, token2;  
  
  std::size_t pos = s.find(delimiter);
  token1 = s.substr(0, pos);
  token2 = s.substr(pos+1, s.length());
  param = static_cast<uint32_t>(atoi(token2.c_str()));
  //ebbrt::kprintf_force("Core: %u TcpServer::Receive() s=%s token1=%s param=%u\n", mcore, s.c_str(), token1.c_str(), param);
  
  if (token1 == "start") {
    ebbrt::kprintf_force("start()\n");
    //for (uint32_t i = 0; i < ncores; i++) {
    //  network_manager->Config("start_stats", i);
    //}
    network_manager->Config("start_stats", 1);    
  } else if (token1 == "stop") {
    ebbrt::kprintf_force("stop()\n");
    //for (uint32_t i = 0; i < ncores; i++) {
    //  network_manager->Config("stop_stats", i);
    //}
    network_manager->Config("stop_stats", 1);    
  } else if (token1 == "clear") {
    ebbrt::kprintf_force("clear()\n");
    //for (uint32_t i = 0; i < ncores; i++) {
    //  network_manager->Config("clear_stats", i);
    // }
    network_manager->Config("clear_stats", 1);    
  } else if (token1 == "rx_usecs") {
    ebbrt::kprintf_force("itr %u\n", param);
    /*for (uint32_t i = 0; i < ncores; i++) {
      event_manager->SpawnRemote(
	[token1, param, i] () mutable {
	  network_manager->Config(token1, param);
	}, i);
	}*/
    event_manager->SpawnRemote(
      [token1, param] () mutable {
	network_manager->Config(token1, param);
      }, MCPU);
    
  } else if (token1 == "dvfs") {
    ebbrt::kprintf_force("dvfs %u\n", param);
    //for (uint32_t i = 0; i < ncores; i++) {
    event_manager->SpawnRemote(
      [param] () mutable {
	// same p state as Linux with performance governor
	ebbrt::msr::Write(IA32_PERF_CTL, param);    
      }, MCPU);
      //}
    
  } else if (token1 == "rapl") {
    ebbrt::kprintf_force("rapl %u\n", param);
    
    for (uint32_t i = 0; i < 2; i++) {
      event_manager->SpawnRemote(
	[token1, param, i] () mutable {
	  network_manager->Config(token1, param);
	}, i);
    }
    
  } else if (token1 == "rdtsc") {
    /*uint64_t tstart, tclose;
    std::stringstream ss;

    tstart = ixgbe_stats[mcore].rdtsc_start;
    tclose = ixgbe_stats[mcore].rdtsc_end;
    
    ss << tstart << ' ' << tclose;
    std::string s = ss.str();
    ebbrt::kprintf_force("rdtsc %s\n", s.c_str());
    auto rbuf = MakeUniqueIOBuf(s.length(), false);
    auto dp = rbuf->GetMutDataPointer();
    std::memcpy(static_cast<void*>(dp.Data()), s.data(), s.length());
    Send(std::move(rbuf));
    */
  } else if (token1 == "get") {
    uint8_t* re = (uint8_t*)(&ixgbe_logs);
    uint64_t msg_size = sizeof(ixgbe_logs);
    uint64_t sum = 0;
    uint64_t maxs = 262144;

    if (readyToSocat == 1) {
      for(uint64_t i = 0; i < msg_size; i++) {
	sum += re[i];
      }
      ebbrt::kprintf_force("get param=%u re=0x%X msg_size=%lu sum=%llu\n", param, (void*)re, msg_size, sum);
      while(msg_size > maxs) {
	auto buf = std::make_unique<ebbrt::StaticIOBuf>(re, maxs);
	Send(std::move(buf));      
	msg_size -= maxs;
	re += maxs;
      }
      if(msg_size) {
	auto buf = std::make_unique<ebbrt::StaticIOBuf>(re, msg_size);
	Send(std::move(buf));
      }
      readyToSocat = 0;
    } else {
      ebbrt::kprintf_force("TcpServer get: readyToSocat = %d\n", readyToSocat);
    }
  } else {
    ebbrt::kprintf_force("Unknown command %s\n", token1.c_str());
  }
}


