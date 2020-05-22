//          Copyright Boston University SESA Group 2013 - 2014.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
#include <cstdlib>
#include <sstream>
#include <string>
#include <ebbrt/SharedIOBufRef.h>
#include <ebbrt/UniqueIOBuf.h>
#include <ebbrt/native/Net.h>
#include <ebbrt/native/Msr.h>

#include "UdpCommand.h"

int msgs = 0;

ebbrt::UdpCommand::UdpCommand() {}

void ebbrt::UdpCommand::Start(uint16_t port) {
  udp_pcb.Bind(port);
  udp_pcb.Receive([this](Ipv4Address from_addr, uint16_t from_port,
			 std::unique_ptr<MutIOBuf> buf) {
		    ReceiveCommand(from_addr, from_port, std::move(buf));
		  });
  uint32_t mcore = static_cast<uint32_t>(Cpu::GetMine());
  ebbrt::kprintf_force("Core: %u %s\n", mcore, __PRETTY_FUNCTION__);
}

void ebbrt::UdpCommand::ReceiveCommand(
    Ipv4Address from_addr, uint16_t from_port, std::unique_ptr<MutIOBuf> buf) {
  uint32_t mcore = static_cast<uint32_t>(Cpu::GetMine());
  std::string s(reinterpret_cast<const char*>(buf->Data()));    
  std::string delimiter = ",";
  uint32_t pos = 0, param = 0;
  std::string token1, token2;

  pos = s.find(delimiter);
  token1 = s.substr(0, pos);
  token2 = s.substr(pos+1, s.length());
  param = static_cast<uint32_t>(atoi(token2.c_str()));
  ebbrt::kprintf_force("Core: %u UdpCommand::ReceiveCommand() from_port=%u message:%s\n", mcore, from_port, s.c_str());

  if(token1 == "max") {
    for (uint32_t i = 0; i < static_cast<uint32_t>(Cpu::Count()); i++) {
      event_manager->SpawnRemote(
	[this, i] () mutable {
	  // disables turbo boost, thermal control circuit
	  ebbrt::msr::Write(IA32_MISC_ENABLE, 0x4000850081);
	  // same p state as Linux with performance governor
	  ebbrt::msr::Write(IA32_PERF_CTL, 0x1D00);
	  ebbrt::kprintf_force("Core %u: applied 0x1D00\n", i);
	}, i);
    }  
  } else if(token1 == "mid") {
    for (uint32_t i = 0; i < static_cast<uint32_t>(Cpu::Count()); i++) {
      event_manager->SpawnRemote(
	[this, i] () mutable {
	  // disables turbo boost, thermal control circuit
	  ebbrt::msr::Write(IA32_MISC_ENABLE, 0x4000850081);
	  // same p state as Linux with performance governor
	  ebbrt::msr::Write(IA32_PERF_CTL, 0x1400);
	  ebbrt::kprintf_force("Core %u: applied 0x1400\n", i);
	}, i);
    }
  } else if(token1 == "min") {
    for (uint32_t i = 0; i < static_cast<uint32_t>(Cpu::Count()); i++) {
      event_manager->SpawnRemote(
	[this, i] () mutable {
	  // disables turbo boost, thermal control circuit
	  ebbrt::msr::Write(IA32_MISC_ENABLE, 0x4000850081);
	  // same p state as Linux with performance governor
	  ebbrt::msr::Write(IA32_PERF_CTL, 0xc00);
	  ebbrt::kprintf_force("Core %u: applied 0xc00\n", i);
	}, i);
    }
  } else if(token1 == "msg") {
    msgs = atoi(token2.c_str());
    ebbrt::kprintf_force("%s %s msgs=%d\n", token1.c_str(), token2.c_str(), msgs);
  }
  else {
    /*for (uint32_t i = 0; i < 2; i++) {
      event_manager->SpawnRemote(
	[this, token1, param, i] () mutable {
	  //ebbrt::kprintf_force("SpawnRemote %u\n", i);
	  network_manager->Config(token1, param);
	}, i);
	}*/
    event_manager->SpawnLocal(
      [this, token1, param] () mutable {
	//ebbrt::kprintf_force("SpawnRemote %u\n", i);
	network_manager->Config(token1, param);
      });
  }
}
