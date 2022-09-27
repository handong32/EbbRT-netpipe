#include <cfloat>
#include <cstdlib>
#include <sstream>

#include <ebbrt/native/Cpu.h>
#include <ebbrt/native/Clock.h>
#include <ebbrt/EventManager.h>
#include <ebbrt/native/NetTcpHandler.h>
#include <ebbrt/native/Msr.h>
#include <ebbrt/SharedIOBufRef.h>
#include <ebbrt/StaticIOBuf.h>
#include <ebbrt/UniqueIOBuf.h>
#include <ebbrt/Future.h>
#include <ebbrt/native/Net.h>
#include <ebbrt/native/IxgbeDriver.h>

namespace {
const constexpr char sync_string[] = "SyncMe";
}

#define IA32_MISC_ENABLE 0x1A0
#define IA32_PERF_CTL    0x199

struct data {
  double t;
  double bps;
  double variance;
  uint64_t bits;
  uint64_t repeat;
};

// MAX TCP per send length, need to manually segment the data
//#define MAXS 262144
#define MAXS 900000

namespace ebbrt {
  class TcpSender : public ebbrt::TcpHandler {
  public:
    TcpSender(ebbrt::NetworkManager::TcpPcb pcb)
      : ebbrt::TcpHandler(std::move(pcb)) {
    }
    
    void Connected() override {
      ebbrt::kprintf_force("TcpSender Connected()\n");
    }
    
    void Receive(std::unique_ptr<ebbrt::MutIOBuf> b) override {
      switch(state_) {
      case RPC: {
	//ebbrt::kprintf_force("RPC len=%d\n", b->ComputeChainDataLength());
	if(buf_) {
	  buf_->PrependChain(std::move(b));
	} else {
	  buf_ = std::move(b);
	}

	auto chain_len = buf_->ComputeChainDataLength();	
	if(chain_len == buf_size_) {	  
	  ebbrt::event_manager->ActivateContext(std::move(context_));
	}
	break;
      }
      default: {
	ebbrt::clock::SleepMilli(1000);
	ebbrt::event_manager->ActivateContext(std::move(context_));
      }
      }
      
      //ebbrt::event_manager->ActivateContext(std::move(context_));
    }        
    
    void Sync(size_t i, size_t msg_sizes, size_t itr, size_t dvfs, size_t rapl) {
      //ebbrt::kprintf_force("i=%u MSG=%u ITR=%u DVFS=0x%x RAPL=%u Sync() start sleeping 15s\n",
//			   i, msg_sizes, itr, dvfs, rapl);
      //    ebbrt::clock::SleepMilli(15000);
      state_ = SYNC;
      auto buf = ebbrt::IOBuf::Create<ebbrt::StaticIOBuf>(sync_string);
      Send(std::move(buf));
      Pcb().Output();
      ebbrt::event_manager->SaveContext(context_);
      //ebbrt::kprintf_force("Sync() complete\n");
    }

    void SendRepeat(size_t msg_sizes, size_t iters, size_t dvfs, size_t rapl, size_t itr,
		    size_t iter) {
      state_ = REPEAT;
      auto buf = ebbrt::MakeUniqueIOBuf(sizeof(msg_sizes) + sizeof(iters)
					+ sizeof(dvfs) + sizeof(rapl)
					+ sizeof(itr) + sizeof(iter));
      auto dp = buf->GetMutDataPointer();
      dp.Get<size_t>() = msg_sizes;
      dp.Get<size_t>() = iters;
      dp.Get<size_t>() = dvfs;
      dp.Get<size_t>() = rapl;
      dp.Get<size_t>() = itr;
      dp.Get<size_t>() = iter;
      Send(std::move(buf));
      Pcb().Output();
      ebbrt::event_manager->SaveContext(context_);
      //ebbrt::kprintf_force("SendRepeat() complete\n");
    }
    
    void SendData(float tput, float lat, uint64_t tdiff) {
      state_ = DATA;
      auto buf = ebbrt::MakeUniqueIOBuf(sizeof(tput) + sizeof(lat) + sizeof(tdiff));
      auto dp = buf->GetMutDataPointer();
      dp.Get<float>() = tput;
      dp.Get<float>() = lat;
      dp.Get<uint64_t>() = tdiff;      
      Send(std::move(buf));
      Pcb().Output();
      ebbrt::event_manager->SaveContext(context_);
    }
    
    void DoTest(){
      size_t inc = (start > 1) ? start / 2 : 1;
      size_t nq = (start > 1) ? 1 : 0;
      size_t bufflen = start;
      
      for (int n = 0, len = start; n < kNSamp - 3 && len <= end;
	   len = len + inc, nq++) {
	if (nq > 2)
	  inc = ((nq % 2)) ? inc + inc : inc;
	
	for (int pert = ((perturbation > 0) && (inc > perturbation + 1))
	       ? -perturbation
	       : 0;
	     pert <= perturbation;
	     n++, pert += ((perturbation > 0) && (inc > perturbation + 1))
	       ? perturbation
	       : perturbation + 1) {
	  bufflen = len + pert;
	  msg_sizes.push_back(bufflen);
	}	
      }
    }
    
    void DoSend(size_t buf_size, size_t iterations) {
      state_ = RPC;
      total_bytes = 0;
      size_t j = 0;
      iterations_ = iterations;
      buf_size_ = buf_size;
      //ebbrt::kprintf_force("DoSend() =%d %d\n", buf_size, iterations);
      
      for(j = 0; j < iterations; j++) {
	if(j == 0) {
	  if(buf_size_ > MAXS) {
	    size_t tmp_len = buf_size_;
	    //ebbrt::kprintf_force("tmp_len=%d\n", tmp_len);
	    while(tmp_len > MAXS) {
	      auto buf = ebbrt::MakeUniqueIOBuf(MAXS);
	      memset(buf->MutData(), 'a', MAXS);
	      Send(std::move(buf));
	      //ebbrt::kprintf_force("sending length=%d\n", MAXS);
	      tmp_len -= MAXS;
	    }
	    
	    if(tmp_len) {
	      auto buf = ebbrt::MakeUniqueIOBuf(tmp_len);
	      memset(buf->MutData(), 'a', tmp_len);
	      Send(std::move(buf));
	      //ebbrt::kprintf_force("sending length=%d\n", tmp_len);
	      tmp_len -= tmp_len;
	    }
	  } else {
	    auto buf = ebbrt::MakeUniqueIOBuf(buf_size);
	    memset(buf->MutData(), 'a', buf_size);
	    Send(std::move(buf));
	  }
	} else {
	  Send(std::move(buf_));
	}
	
	Pcb().Output();
	//ebbrt::kprintf_force("Sending j=%d ", j);
	//total_bytes += buf_size;
	ebbrt::event_manager->SaveContext(context_);
      }
      buf_ = nullptr;
      //ebbrt::kprintf_force("total bytes: %llu ", total_bytes);
    }    
    
    void Wait() {
      ebbrt::event_manager->SaveContext(context_);
      ebbrt::kprintf_force("Wait() finished\n");
    }
    
    void Close() override { Shutdown(); }
    void Abort() override {}

    std::vector<uint32_t> msg_sizes;
    std::vector<uint32_t> itrs;
    std::vector<uint32_t> dvfss;
    std::vector<uint32_t> rapls;
    
  private:
    static const constexpr double runtm = 0.10;
    static const constexpr double stoptm = 1.0;
    static const constexpr int trials = 1;
    static const constexpr int kNSamp = 100;
    static const constexpr size_t nbuff = 3;
    static const constexpr int perturbation = 0;
    static const constexpr size_t start = 64;    
    static const constexpr int end = 900000;
    //static const constexpr int end = 786433;
    //static const constexpr size_t start = 200000;

    enum states { SYNC, RPC, REPEAT, DATA};
    enum states state_;

    std::unique_ptr<ebbrt::MutIOBuf> buf_;
    size_t iterations_{0};
    size_t buf_size_{0};

    uint64_t time0{0};
    uint64_t time1{0};
    uint64_t total_bytes{0};
    double tdiff{0.0};
    //double t_{0.0};
    size_t repeat_{0};
    //ebbrt::clock::Wall::time_point t0_;    
    
    ebbrt::NetworkManager::TcpPcb pcb_;
    ebbrt::EventManager::EventContext context_;
  };  
}

void AppMain() {
  ebbrt::kprintf_force("Core %u: NetPIPE TX mode\n", static_cast<uint32_t>(ebbrt::Cpu::GetMine()));
    
  for (uint32_t i = 0; i < static_cast<uint32_t>(ebbrt::Cpu::Count()); i++) {
    ebbrt::Promise<void> p;
    auto f = p.GetFuture();
    ebbrt::event_manager->SpawnRemote(
      [i, &p] () mutable {
	// disables turbo boost, thermal control circuit
	ebbrt::msr::Write(IA32_MISC_ENABLE, 0x4000850081);
	// same p state as Linux with performance governor
	ebbrt::msr::Write(IA32_PERF_CTL, 0x1D00);
	if(i == 0 || i == 1) {
	  ebbrt::rapl::RaplCounter powerMeter;
	  powerMeter.SetLimit(135);		
	}
	ebbrt::kprintf_force("Core %u: performance applied\n", i);	
	p.SetValue();
      }, i);
    f.Block();
  }
  
  ebbrt::event_manager->SpawnLocal(
    [] () mutable {           
      auto s1 = ebbrt::clock::Wall::Now().time_since_epoch();
      uint64_t t1 = std::chrono::duration_cast<std::chrono::microseconds>(s1).count();      
      ebbrt::clock::SleepMilli(100);
      auto s2 = ebbrt::clock::Wall::Now().time_since_epoch();
      uint64_t t2 = std::chrono::duration_cast<std::chrono::microseconds>(s2).count();
      uint64_t tdiff = t2 - t1;
      float tdiff2 = static_cast<float>(tdiff) / 1000000.0;      
      ebbrt::kprintf_force("sleep 100 ms: t1=%llu t2=%llu tdiff=%llu us, %.2lf s\n", t1, t2, tdiff, tdiff2);      

      uint64_t bits = 0;
      float bps = 0.0;
      
      ebbrt::NetworkManager::TcpPcb tpcb;      
      tpcb.Connect(ebbrt::Ipv4Address({192, 168, 1, 9}), 5002);
      
      auto handler = new ebbrt::TcpSender(std::move(tpcb));
      handler->Install();     
      ebbrt::kprintf_force("handler->Install\n");

      // warm up
      size_t tsz = 666;
      size_t iters = 10;
      size_t dvfs = 0x1d00;
      size_t rapl = 135;
      size_t itr = 3;
      size_t iter = 1;      
      for(size_t c=0;c<iter;c++) {
	handler->Sync(c, tsz, itr*2, dvfs, rapl);
	handler->SendRepeat(tsz, iters, dvfs, rapl, itr, c);
	s1 = ebbrt::clock::Wall::Now().time_since_epoch();
	t1 = std::chrono::duration_cast<std::chrono::microseconds>(s1).count();
	handler->DoSend(tsz, iters);
	s2 = ebbrt::clock::Wall::Now().time_since_epoch();
	t2 = std::chrono::duration_cast<std::chrono::microseconds>(s2).count();
	tdiff = t2 - t1;
	tdiff2 = static_cast<float>(tdiff) / 1000000.0;
	tdiff2 = tdiff2 / static_cast<float>(iters) / 2.0;      
	bits = tsz * 8;
	bps = static_cast<float>(bits) / (tdiff2 * 1024 * 1024);
	handler->SendData(bps, (tdiff2 * 1000000.0), t2-t1);
	ebbrt::kprintf_force("%7u bytes %6d times in %8.2f Mbps in %10.6f secs\n", tsz, iters, bps, ((t2-t1) / 1000000.0));
	//ebbrt::kprintf_force("%7u bytes %6d times in %8.2f Mbps in %10.6f usec\n", tsz, iters, bps, (tdiff2 * 1000000.0));
      }
      
      // for logging
      
      // itr == 3 * 2 == 6 us, minimum for RSC to work
      //handler->itrs.push_back(0);
      //handler->itrs.push_back(1);
      //handler->itrs.push_back(2);
      handler->itrs.push_back(3);
      //handler->itrs.push_back(4);
      handler->itrs.push_back(5);
      //handler->itrs.push_back(6);
      handler->itrs.push_back(7);
      //handler->itrs.push_back(8);
      handler->itrs.push_back(9);
      //handler->itrs.push_back(10);
      handler->itrs.push_back(11);
      //handler->itrs.push_back(12);
      handler->itrs.push_back(13);
      //handler->itrs.push_back(14);
      handler->itrs.push_back(15);
      //handler->itrs.push_back(16);
      handler->itrs.push_back(17);
      //handler->itrs.push_back(18);
      handler->itrs.push_back(19);
      //handler->itrs.push_back(20);
      
      handler->dvfss.push_back(0x1d00);
      handler->dvfss.push_back(0x1b00);
      handler->dvfss.push_back(0x1900);
      handler->dvfss.push_back(0x1700);
      handler->dvfss.push_back(0x1500);
      handler->dvfss.push_back(0x1300);
      handler->dvfss.push_back(0x1100);
      handler->dvfss.push_back(0xf00);
      handler->dvfss.push_back(0xd00);
      handler->dvfss.push_back(0xc00);
      
      handler->rapls.push_back(135);

      //handler->msg_sizes.push_back(64);
      //handler->msg_sizes.push_back(128);
      //handler->msg_sizes.push_back(256);
      //handler->msg_sizes.push_back(512);
      //handler->msg_sizes.push_back(1024);
      //handler->msg_sizes.push_back(2048);
      //handler->msg_sizes.push_back(4096);
      //handler->msg_sizes.push_back(8192);
      //handler->msg_sizes.push_back(16384);
      //handler->msg_sizes.push_back(32768);
      handler->msg_sizes.push_back(65536);
      //handler->msg_sizes.push_back(131072);
      //handler->msg_sizes.push_back(262144);
      handler->msg_sizes.push_back(524288);
      
      iters = 5000;
      // number of times to repeat experiment
      iter = 2;
      
      //ebbrt::kprintf_force("iter msg_size iterations dvfs rapl itr tput lat\n");
      for(size_t j = 0; j < handler->itrs.size(); j++) {
	itr = handler->itrs[j];	
	
	for(size_t i = 0; i < handler->msg_sizes.size(); i++) {
	  for(size_t r = 0; r < handler->rapls.size(); r++) {
	    rapl = handler->rapls[r];
	    for(size_t d = 0; d < handler->dvfss.size(); d++) {
	      dvfs = handler->dvfss[d];	    
	      tsz = handler->msg_sizes[i];
	      
	      for (uint32_t cpu = 0; cpu < static_cast<uint32_t>(ebbrt::Cpu::Count()); cpu++) {
		ebbrt::Promise<void> p;
		auto f = p.GetFuture();
		ebbrt::event_manager->SpawnRemote(
		  [itr, &p] () mutable {
		    ebbrt::network_manager->Config("rx_usecs", itr);
		    p.SetValue();
		  }, cpu);
		f.Block();
	      }
	      
	      for(size_t c=0;c<iter;c++) {
		//ebbrt::kprintf_force
		//  ("%u %u %u 0x%x %u %u ",
		//   c, tsz, iters, dvfs, rapl, itr*2);

		//auto ss1 = ebbrt::clock::Wall::Now().time_since_epoch();
		//uint64_t tt1 = std::chrono::duration_cast<std::chrono::microseconds>(ss1).count();
      
		handler->Sync(c, tsz, itr*2, dvfs, rapl);
		handler->SendRepeat(tsz, iters, dvfs, rapl, itr, c);
		s1 = ebbrt::clock::Wall::Now().time_since_epoch();
		t1 = std::chrono::duration_cast<std::chrono::microseconds>(s1).count();
		handler->DoSend(tsz, iters);
		s2 = ebbrt::clock::Wall::Now().time_since_epoch();
		t2 = std::chrono::duration_cast<std::chrono::microseconds>(s2).count();
		tdiff = t2 - t1;
		tdiff2 = static_cast<float>(tdiff) / 1000000.0;
		tdiff2 = tdiff2 / static_cast<float>(iters) / 2.0;		
		bits = tsz * 8;
		bps = static_cast<float>(bits) / (tdiff2 * 1024 * 1024);
		
		handler->SendData(bps, (tdiff2 * 1000000.0), t2-t1);
		ebbrt::kprintf_force("itr=%4u: %7u bytes %6d times in %8.2f Mbps in %10.6f secs\n", itr*2, tsz, iters, bps, ((t2-t1) / 1000000.0));
		
		//ebbrt::kprintf_force("i=%u MSG=%u ITR=%u DVFS=0x%x RAPL=%u\n",
		//		     c, tsz, itr*2, dvfs, rapl);
		//ebbrt::clock::SleepMilli(2000);
		//auto ss2 = ebbrt::clock::Wall::Now().time_since_epoch();
		//uint64_t tt2 = std::chrono::duration_cast<std::chrono::microseconds>(ss2).count();
		//uint64_t ttdiff = t2 - t1;
		//float ttdiff2 = static_cast<float>(tdiff) / 1000000.0;
		//ebbrt::kprintf_force("t1=%llu t2=%llu tdiff=%llu us, %.2lf s\n", tt1, tt2, ttdiff, ttdiff2);
		
		//ebbrt::kprintf_force("%.4lf %.10lf\n", bps, tdiff2);
	      }
	    }
	  }
	}
      }
      
      ebbrt::kprintf_force("Finished \n");
      handler->Shutdown();      
    }, true);
}
