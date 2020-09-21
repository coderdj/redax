#include "WFSim.hh"
#include "MongoLog.hh"
#include <chrono>
#include <cmath>

using std::vector;
using std::pair;
constexpr double PI() {return std::acos(-1.);}
const int PMTsPerDigi = 8;

// redeclare all the static members
std::thread WFSim::sGeneratorThread;
std::mutex WFSim::sMutex;
std::random_device WFSim::sRD;
std::mt19937_64 WFSim::sGen;
std::uniform_real_distribution<> WFSim::sFlatDist;
long WFSim::sClock;
int WFSim::sEventCounter;
std::atomic_bool WFSim::sRun, WFSim::sReady;
fax_options_t WFSim::sFaxOptions;
int WFSim::sNumPMTs;
vector<WFSim*> WFSim::sRegistry;
vector<pair<double, double>> WFSim::sPMTxy;
std::condition_variable WFSim::sCV;
std::shared_ptr<MongoLog> WFSim::sLog;

pair<double, double> PMTiToXY(int i) {
  if (i == 0) return std::make_pair(0., 0.);
  if (i < 7) return std::make_pair(std::cos((i-1)*PI()/3.), std::sin((i-1)*PI()/3.));
  int ring = 2;
  // how many total PMTs are contained in a radius r? aka which ring is this PMT in
  while (i > 3*ring*(ring+1)) ring++;
  int i_in_ring = i - (1 + 3*ring*(ring-1));
  int side = i_in_ring / ring;
  int side_i = i_in_ring % ring;

  double ref_angle = PI()/3*side;
  double offset_angle = ref_angle + 2*PI()/3;
  double x_c = ring*std::cos(ref_angle), y_c = ring*std::sin(ref_angle);
  return std::make_pair(x_c + side_i*std::cos(offset_angle), y_c + side_i*std::sin(offset_angle));
}

WFSim::WFSim(std::shared_ptr<ThreadPool>& tp, std::shared_ptr<Processor>& next, std::shared_ptr<Options>& opts, std::shared_ptr<MongoLog>& log) : V1724(tp, next, opts, log) {
  fLog->Entry(MongoLog::Warning, "Initializing fax digitizer");
  fGen = std::mt19937_64(fRD());
  fFlatDist = std::uniform_real_distribution<>(0., 1.);
  fSPEtemplate = {0.0, 0.0, 0.0, 2.81e-2, 7.4, 6.07e1, 3.26e1, 1.33e1, 7.60, 5.71,
    7.75, 4.46, 3.68, 3.31, 2.97, 2.74, 2.66, 2.48, 2.27, 2.15, 2.03, 1.93, 1.70,
    1.68, 1.26, 7.86e-1, 5.36e-1, 4.36e-1, 3.11e-1, 2.15e-1};
  fEventCounter = 0;
  fSeenUnder5 = true;
  fSeenOver15 = false;
}

WFSim::~WFSim() {
  End();
}

void WFSim::End() {
  AcquisitionStop(true);
}

int WFSim::WriteRegister(unsigned int reg, unsigned int val) {
  if (reg == 0x8020 || (reg & 0x1020) == 0x1020) { // min record length

  } else if (reg == 0x8038 || (reg & 0x1038) == 0x1038) { // pre-trigger

  } else if (reg == 0x8060 || (reg & 0x1060) == 0x1060) { // trigger threshold

  } else if (reg == 0x8078 || (reg & 0x1078) == 0x1078) { // samples under threshold

  } else if (reg == 0x807C || (reg & 0x107C) == 0x107C) { // max tail

  } else if (reg == 0x8098 || (reg & 0x1098) == 0x1098) { // DC offset
    if (reg == 0x8098) std::fill_n(fBaseline.begin(), fBaseline.size(), val&0xFFFF);
    else fBaseline[(reg>>8)&0xF] = (val&0xFFFF);
  }
  return 0;
}

unsigned int WFSim::ReadRegister(unsigned int) {
  return 0;
}

int WFSim::Init(int, int, int bid, unsigned int) {
  fBID = bid;
  if (fOptions->GetFaxOptions(fFaxOptions)) {
    fLog->Entry(MongoLog::Message, "Using default fax options");
    fFaxOptions.rate = 10; // Hz
    fFaxOptions.tpc_size = 2; // radius in PMTs
    fFaxOptions.e_absorbtion_length = (fFlatDist(fGen)+1)*fFaxOptions.tpc_size; // TPC lengths
    fFaxOptions.drift_speed = 1e-4; // pmts/ns
  }
  GlobalInit(fFaxOptions, fLog);
  Reset();
  sRegistry.push_back(this);
  fBLoffset = fBLslope = fNoiseRMS = fBaseline = vector<double>(PMTsPerDigi, 0);
  std::generate_n(fBLoffset.begin(), PMTsPerDigi, [&]{return 17000 + 400*fFlatDist(fGen);});
  std::generate_n(fBLslope.begin(), PMTsPerDigi, [&]{return -0.27 + 0.01*fFlatDist(fGen);});
  std::exponential_distribution<> noise(1);
  std::generate_n(fNoiseRMS.begin(), PMTsPerDigi, [&]{return 4*noise(fGen);});
  std::generate_n(fBaseline.begin(), PMTsPerDigi, [&]{return 13600 + 50*fFlatDist(fGen);});
  return 0;
}

int WFSim::SWTrigger() {
  ConvertToDigiFormat(GenerateNoise(fSPEtemplate.size(), 0xFF), 0xFF, sClock);
  return 0;
}

void WFSim::GlobalInit(fax_options_t& fax_options, std::shared_ptr<MongoLog>& log) {
  if (sReady == false) {
    sGen = std::mt19937_64(sRD());
    sFlatDist = std::uniform_real_distribution<>(0., 1.);
    sFaxOptions = fax_options;
    sLog = log;
    sLog->Entry(MongoLog::Local, "WFSim global init");

    sReady = true;
    sRun = false;
    sClock = 0;
    sEventCounter = 0;
    sNumPMTs = (1+3*fax_options.tpc_size*(fax_options.tpc_size+1))*2;
    int PMTsPerArray = sNumPMTs/2;
    sPMTxy.reserve(sNumPMTs);
    for (int p = 0; p < sNumPMTs; p++)
      sPMTxy.emplace_back(PMTiToXY(p % PMTsPerArray));

    sGeneratorThread = std::thread(&WFSim::GlobalRun);
  } else
    sLog->Entry(MongoLog::Local, "WFSim global already init");
}

void WFSim::GlobalDeinit() {
  if (sGeneratorThread.joinable()) {
    sLog->Entry(MongoLog::Local, "WFSim::deinit");
    sRun = sReady = false;
    sCV.notify_one();
    sGeneratorThread.join();
    sLog.reset();
    sRegistry.clear();
    sPMTxy.clear();
  }
}

uint32_t WFSim::GetAcquisitionStatus() {
  uint32_t ret = 0;
  ret |= 0x4*(sRun == true); // run status
  ret |= 0x8*(fBufferSize > 0); // event ready
  ret |= 0x80; // no PLL unlock
  ret |= 0x100*(sRun == true || sReady == true); // board is ready
  ret |= 0x8000*(sRun == true); // S-IN

  return ret;
}

int WFSim::SoftwareStart() {
  fLastClockTime = std::chrono::high_resolution_clock::now();
  if (sReady == true) {
    sRun = true;
    sReady = false;
    sCV.notify_one();
  }
  return 0;
}

int WFSim::SINStart() {
  return SoftwareStart();
}

int WFSim::AcquisitionStop(bool i_mean_it) {
  if (!i_mean_it) return 0;
  GlobalDeinit();
  sRun = false;
  Reset();
  return 0;
}

int WFSim::Reset() {
  const std::lock_guard<std::mutex> lg(fBufferMutex);
  fBuffer.clear();
  fEventCounter = 0;
  fBufferSize = 0;
  return 0;
}

int WFSim::Read(std::u32string* outptr) {
  if (fBufferSize == 0) return 0;
  const std::lock_guard<std::mutex> lg(fBufferMutex);
  std::u32string dp;
  dp.reserve(fBuffer.size() + fDPoverhead);
  dp += ThreadPool::TaskCode::UnpackDatapacket;
  uint32_t word = 0;
  word = GetHeaderTime(fBuffer.data(), fBuffer.size());
  int fRolloverCounter = GetClockCounter(word);
  dp += word;
  dp += fRolloverCounter;
  dp += fBuffer;
  fBuffer.clear();
  fBufferSize = 0;
  int ret = dp.size();
  if (outptr != nullptr)
    *outptr = dp.substr(fDPoverhead);
  else
    fTP->AddTask(this, std::move(dp));
  return ret;
}

int WFSim::GetClockCounter(uint32_t timestamp) {
  // Waveform generation is asynchronous, so we need different logic here
  // from a hardware digitizer
  if (timestamp > fLastClock) {
    // Case 1. This is over 15s but fSeenUnder5 is true. Give 1 back
    if(timestamp >= 15e8 && fSeenUnder5 && fRolloverCounter != 0)
      return fRolloverCounter-1;

    // Case 2. This is over 5s and fSeenUnder5 is true.
    else if(fSeenUnder5 && 5e8 <= timestamp && timestamp < 15e8){
      fSeenUnder5 = false;
      fLastClock = timestamp;
      return fRolloverCounter;
    }

    // Case 3. This is over 15s and fSeenUnder5 is false
    else if(timestamp >= 15e8 && !fSeenUnder5){
      fSeenOver15 = true;
      fLastClock = timestamp;
      return fRolloverCounter;
    }

    // Case 4. Anything else where the clock is progressing correctly
    else{
      fLastClock = timestamp;
      return fRolloverCounter;
    }
  }

  // Second, is this number less than the previous?
  else if(timestamp < fLastClock){

    // Case 1. Genuine clock reset. under 5s is false and over 15s is true
    if(timestamp < 5e8 && !fSeenUnder5 && fSeenOver15){
      fSeenUnder5 = true;
      fSeenOver15 = false;
      fLog->Entry(MongoLog::Local, "Bd %i rollover %i (%x/%x)",
              fBID, fRolloverCounter, timestamp, fLastClock);
      fLastClock = timestamp;
      fRolloverCounter++;
      return fRolloverCounter;
    }

    // Case 2: Any other jitter within the 21 seconds, just return
    else{
      return fRolloverCounter;
    }
  }
  // timestamps are the same???
  else
    return fRolloverCounter;
}

void WFSim::Process(std::u32string_view sv) {
  if (sv[0] == ThreadPool::TaskCode::UnpackDatapacket) return DPtoChannels(sv);
  if (sv[0] == ThreadPool::TaskCode::GenerateWaveform) return MakeWaveform(sv);
  fLog->Entry(MongoLog::Warning, "V1724::Process received unknown task code %i, %i words scrapped", sv[0], sv.size());
  return;
}

std::tuple<double, double, double> WFSim::GenerateEventLocation() {
  double offset = 0.5; // min number of PMTs between S1 and S2 to prevent overlap
  double z = -1.*sFlatDist(sGen)*((2*sFaxOptions.tpc_size+1)-offset)-offset;
  double r = sFlatDist(sGen)*sFaxOptions.tpc_size; // no, this isn't uniform
  double theta = sFlatDist(sGen)*2*PI();
  return {r*std::cos(theta), r*std::sin(theta), z};
}

vector<int> WFSim::GenerateEventSize(double, double, double z) {
  int s1 = sFlatDist(sGen)*19+11;
  std::normal_distribution<> s2_over_s1{100, 20};
  double elivetime_loss_fraction = std::exp(z/sFaxOptions.e_absorbtion_length);
  int s2 = s1*s2_over_s1(sGen)*elivetime_loss_fraction;
  return {0, s1, s2};
}

vector<pair<int, double>> WFSim::MakeHitpattern(int s_i, int photons, double x, double y, double z) {
  double signal_width = s_i == 1 ? 40 : 1000.+200.*std::sqrt(std::abs(z));
  vector<pair<int, double>> ret(photons); // { {pmt id, hit time}, {pmt id, hit time} ... }
  vector<double> hit_prob(sNumPMTs, 0.);
  std::discrete_distribution<> hitpattern;
  double top_fraction(0);
  int TopPMTs = sNumPMTs/2;
  int tpc_length = 2*sFaxOptions.tpc_size + 1;
  if (s_i == 1) {
    top_fraction = (0.4-0.1)/(0+tpc_length)*z+0.4; // 10% at bottom, 40% at top
    std::fill_n(hit_prob.begin(), TopPMTs, top_fraction/TopPMTs);
  } else {
    top_fraction = 0.65;
    // let's go with a Gaussian probability, because why not
    double gaus_width = 2*1.3*1.3; // PMTs wide
    auto gen = [&](std::pair<double, double>& p){
      return std::exp(-(std::pow(p.first-x, 2)+std::pow(p.second-y, 2))/gaus_width);
    };

    std::transform(sPMTxy.begin(), sPMTxy.begin()+TopPMTs, hit_prob.begin(), gen);
    // normalize
    double total_top_prob = std::accumulate(hit_prob.begin(), hit_prob.begin()+TopPMTs, 0.);
    std::transform(hit_prob.begin(), hit_prob.begin()+TopPMTs, hit_prob.begin(),
        [&](double x){return top_fraction*x/total_top_prob;});
  }
  // bottom array probability simpler to calculate
  std::fill(hit_prob.begin()+TopPMTs, hit_prob.end(), (1.-top_fraction)/TopPMTs);

  hitpattern.param(std::discrete_distribution<>::param_type(hit_prob.begin(), hit_prob.end()));

  std::generate_n(ret.begin(), photons,
      [&]{return std::make_pair(hitpattern(sGen), WFSim::sFlatDist(sGen)*signal_width);});
  return ret;
}

void WFSim::SendToWorkers(const vector<pair<int, double>>& hits) {
  vector<std::u32string> hits_per_board(sRegistry.size());
  unsigned int overhead = 3;
  for (auto& s : hits_per_board) {
    s += ThreadPool::TaskCode::GenerateWaveform;
    s.append((char32_t*)&sClock, sizeof(sClock)/sizeof(char32_t));
  }
  int board, ch;
  int t_size = sizeof(double)/sizeof(char32_t);
  for (auto& hit : hits) {
    ch = hit.first % PMTsPerDigi;
    board = hit.first/PMTsPerDigi;
    hits_per_board[board] += ch;
    hits_per_board[board].append((char32_t*)&hit.second, t_size);
  }
  for (unsigned i = 0; i < sRegistry.size(); i++)
    if (hits_per_board[i].size() > overhead)
      sRegistry[i]->fTP->AddTask(sRegistry[i], std::move(hits_per_board[i]));
  return;
}

void WFSim::MakeWaveform(std::u32string_view sv) {
  vector<pair<int, double>> hits;
  hits.reserve((sv.size()-3)/3);
  long timestamp = *(long*)(sv.data()+1);
  sv.remove_prefix(3);
  while (sv.size() > 0) {
    hits.emplace_back(sv[0], *(double*)(sv.data()+1));
    sv.remove_prefix(3);
  }
  int mask = 0;
  double last_hit_time = 0, first_hit_time = 1e9;
  for (auto& hit : hits) {
    mask |= (1<<hit.first);
    last_hit_time = std::max(last_hit_time, hit.second);
    first_hit_time = std::min(first_hit_time, hit.second);
  }
  timestamp += first_hit_time;
  // which channels contribute?
  vector<int> pmt_to_ch(PMTsPerDigi, -1);
  vector<int> ch_to_pmt;
  for (int i = 0; i < PMTsPerDigi; i++) {
    if (mask & (1<<i)) {
      pmt_to_ch[i] = ch_to_pmt.size();
      ch_to_pmt.push_back(i);
    }
  }
  int wf_length = fSPEtemplate.size() + last_hit_time/fSampleWidth;
  wf_length += wf_length % 2 ? 1 : 2; // ensure an even number of samples with room
  auto wf = GenerateNoise(wf_length, mask);
  std::normal_distribution<> hit_scale{1., 0.15};
  int offset;
  double scale;
  for (auto& hit : hits) {
    offset = hit.second/fSampleWidth;
    scale = hit_scale(fGen);
    for (unsigned i = 0; i < fSPEtemplate.size(); i++) {
      wf[pmt_to_ch[hit.first]][offset+i] -= fSPEtemplate[i]*scale;
    }
  }
  return ConvertToDigiFormat(wf, mask, timestamp);
}

void WFSim::ConvertToDigiFormat(const vector<vector<double>>& wf, int mask, long ts) {
  fEventCounter++;
  const int overhead_per_channel = 2, overhead_per_event = 4;
  std::u32string buffer;
  char32_t word = 0;
  for (auto& ch : wf) word += ch.size(); // samples
  char32_t words_this_event = word/2 + overhead_per_channel*wf.size() + overhead_per_event;
  buffer.reserve(words_this_event);
  word = words_this_event | 0xA0000000;
  buffer += word;
  buffer += (char32_t)mask;
  word = fEventCounter.load();
  buffer += word;
  char32_t timestamp = (ts/fClockCycle)&0x7FFFFFFF;
  //fLog->Entry(MongoLog::Local, "Bd %i ts %lx/%08x", fBID, ts, timestamp);
  buffer += timestamp;
  int32_t sample;
  for (auto& ch_wf : wf) {
    word = ch_wf.size()/2 + overhead_per_channel; // size is in samples
    buffer += word;
    buffer += timestamp;
    for (unsigned i = 0; i < ch_wf.size(); i += 2) {
      sample = std::max(ch_wf[i], 0.);
      word = sample & 0x3FFF;
      sample = std::max(ch_wf[i+1], 0.);
      word |= (sample << 16)&0x3FFF0000;
      buffer += word;
    } // loop over samples
  } // loop over channels
  {
    const std::lock_guard<std::mutex> lg(fBufferMutex);
    fBuffer.append(buffer);
    fBufferSize = fBuffer.size();
  }
  return;
}

vector<vector<double>> WFSim::GenerateNoise(int length, int mask) {
  vector<vector<double>> ret;
  ret.reserve(PMTsPerDigi);
  for (int i = 0; i < PMTsPerDigi; i++) {
    if (mask & (1<<i)) {
      ret.emplace_back(length, 0);
      std::normal_distribution<> noise{fBaseline[i], fNoiseRMS[i]};
      std::generate(ret.back().begin(), ret.back().end(), [&]{return noise(fGen);});
    }
  }
  return ret;
}

void WFSim::GlobalRun() {
  std::exponential_distribution<> rate(sFaxOptions.rate/1e9);
  double x, y, z, t_max;
  long time_to_next;
  vector<int> photons; // S1 = 1
  vector<pair<int, double>> hits;
  sClock = (0.5+sFlatDist(sGen))*10000;
  sEventCounter = 0;
  {
    std::unique_lock<std::mutex> lg(sMutex);
    sCV.wait(lg, []{return sReady == false;});
  }
  sLog->Entry(MongoLog::Local, "WFSim::GlobalRun");
  auto t_start = std::chrono::high_resolution_clock::now();
  while (sRun == true) {
    std::tie(x,y,z) = GenerateEventLocation();
    photons = GenerateEventSize(x, y, z);
    for (const auto s_i : {1,2}) {
      hits = MakeHitpattern(s_i, photons[s_i], x, y, z);
      // split hitpattern and issue to digis
      SendToWorkers(hits);
      t_max = 0;
      for (auto& hit : hits) {
        t_max = std::max(t_max, hit.second);
      }

      time_to_next = (s_i == 1 ? std::abs(z/sFaxOptions.drift_speed) : rate(sGen)) + t_max;
      sClock += time_to_next;
      //sLog->Entry(MongoLog::Local, "Sleeping for %li ns", time_to_next);
      std::this_thread::sleep_for(std::chrono::nanoseconds(time_to_next));
    }
    sEventCounter++;
  }
  auto t_end = std::chrono::high_resolution_clock::now();
  sLog->Entry(MongoLog::Local, "Generation lasted %lx/%lx", sClock,
          std::chrono::duration_cast<std::chrono::nanoseconds>(t_end-t_start).count());
  sLog->Entry(MongoLog::Local, "WFSim::GlobalRun finished");
}

