#include "f1724.hh"
#include "MongoLog.hh"
#include <chrono>
#include <cmath>

using std::vector;
constexpr double PI() {return std::acos(-1.);}

// redeclare all the static members
std::thread f1724::sGeneratorThread;
std::mutex f1724::sMutex;
std::random_device f1724::sRD;
std::mt19937_64 f1724::sGen;
std::uniform_real_distribution<> f1724::sFlatDist;
long f1724::sClock;
int f1724::sEventCounter;
std::atomic_bool f1724::sRun, f1724::sReady;
fax_options_t f1724::sFaxOptions;
int f1724::sNumPMTs;
vector<f1724*> f1724::sRegistry;
vector<pair<double, double>> f1724::sPMTxy;
std::condition_variable f1724::sCV;
std::shared_ptr<MongoLog> f1724::sLog;

f1724::pmt_pos_t f1724::PMTiToXY(int i) {
  pmt_pos_t ret;
  if (i == 0) {
    ret.x = ret.y = 0;
    return ret;
  }
  if (i < 7) {
    ret.x = std::cos((i-1)*PI()/3.);
    ret.y = std::sin((i-1)*PI()/3.);
    return ret;
  }
  int ring = 2;
  // how many total PMTs are contained in a radius r? aka which ring is this PMT in
  while (i > 3*ring*(ring+1)) ring++;
  int i_in_ring = i - (1 + 3*ring*(ring-1));
  int side = i_in_ring / ring;
  int side_i = i_in_ring % ring;

  double ref_angle = PI()/3*side;
  double offset_angle = ref_angle + 2*PI()/3;
  double x_c = ring*std::cos(ref_angle), y_c = ring*std::sin(ref_angle);
  ret.x = x_c + side_i*std::cos(offset_angle);
  ret.y = y_c + side_i*std::sin(offset_angle);
  return ret;
}

f1724::f1724(std::shared_ptr<Options>& opts, std::shared_ptr<MongoLog>& log, int, int, int bid, unsigned) : V1724(opts, log, 0, 0, bid, 0){
  //fLog->Entry(MongoLog::Warning, "Initializing fax digitizer");
  fSPEtemplate = {0.0, 0.0, 0.0, 2.81e-2, 7.4, 6.07e1, 3.26e1, 1.33e1, 7.60, 5.71,
    7.75, 4.46, 3.68, 3.31, 2.97, 2.74, 2.66, 2.48, 2.27, 2.15, 2.03, 1.93, 1.70,
    1.68, 1.26, 7.86e-1, 5.36e-1, 4.36e-1, 3.11e-1, 2.15e-1};
  fEventCounter = 0;
  fSeenUnder5 = true;
  fSeenOver15 = false;
}

f1724::~f1724() {
  End();
}

int f1724::Init(int, int) {
  if (fOptions->GetFaxOptions(fFaxOptions)) {
    return -1;
  }
  fGen = std::mt19937_64(fRD());
  fFlatDist = std::uniform_real_distribution<>(0., 1.);
  GlobalInit(fFaxOptions, fLog);
  Reset();
  sRegistry.emplace_back(this);
  unsigned n_chan = GetNumChannels();
  fBLoffset = fBLslope = fNoiseRMS = fBaseline = vector<double>(n_chan, 0);
  std::generate_n(fBLoffset.begin(), n_chan, [&]{return 17000 + 400*fFlatDist(fGen);});
  std::generate_n(fBLslope.begin(), n_chan, [&]{return -0.27 + 0.01*fFlatDist(fGen);});
  std::exponential_distribution<> noise(1);
  std::generate_n(fNoiseRMS.begin(), n_chan, [&]{return 4*noise(fGen);});
  std::generate_n(fBaseline.begin(), n_chan, [&]{return 13600 + 50*fFlatDist(fGen);});
  return 0;
}

void f1724::End() {
  AcquisitionStop(true);
}

int f1724::WriteRegister(unsigned int reg, unsigned int val) {
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

unsigned int f1724::ReadRegister(unsigned int) {
  return 0;
}

int f1724::Read(std::unique_ptr<data_packet>& outptr) {
  if (fBufferSize == 0) return 0;
  const std::lock_guard<std::mutex> lk(fBufferMutex);
  int retwords = fBuffer.size();
  auto [ht, cc] = GetClockInfo(fBuffer);
  outptr = std::make_unique<data_packet>(std::move(fBuffer), ht, cc);
  fBufferSize = 0;
  return retwords;
}

int f1724::SWTrigger() {
  ConvertToDigiFormat(GenerateNoise(fSPEtemplate.size(), 0xFF), 0xFF, sClock);
  return 0;
}

void f1724::GlobalInit(fax_options_t& fax_options, std::shared_ptr<MongoLog>& log) {
  if (sReady == false) {
    sGen = std::mt19937_64(sRD());
    sFlatDist = std::uniform_real_distribution<>(0., 1.);
    sFaxOptions = fax_options;
    sLog = log;
    sLog->Entry(MongoLog::Local, "f1724 global init");

    sReady = true;
    sRun = false;
    sClock = 0;
    sEventCounter = 0;
    sNumPMTs = (1+3*fax_options.tpc_size*(fax_options.tpc_size+1))*2;
    int PMTsPerArray = sNumPMTs/2;
    sPMTxy.reserve(sNumPMTs);
    for (int p = 0; p < sNumPMTs; p++)
      sPMTxy.emplace_back(PMTiToXY(p % PMTsPerArray));

    sGeneratorThread = std::thread(&f1724::GlobalRun);
  } else
    sLog->Entry(MongoLog::Local, "f1724 global already init");
}

void f1724::GlobalDeinit() {
  if (sGeneratorThread.joinable()) {
    sLog->Entry(MongoLog::Local, "f1724::deinit");
    sRun = sReady = false;
    sCV.notify_one();
    sGeneratorThread.join();
    sLog.reset();
    sRegistry.clear();
    sPMTxy.clear();
  }
}

uint32_t f1724::GetAcquisitionStatus() {
  uint32_t ret = 0;
  ret |= 0x4*(sRun == true); // run status
  ret |= 0x8*(fBufferSize > 0); // event ready
  ret |= 0x80; // no PLL unlock
  ret |= 0x100*(sRun == true || sReady == true); // board is ready
  ret |= 0x8000*(sRun == true); // S-IN

  return ret;
}

int f1724::SoftwareStart() {
  fLastClockTime = std::chrono::high_resolution_clock::now();
  if (sReady == true) {
    sRun = true;
    sReady = false;
    sCV.notify_one();
  }
  fGeneratorThread = std::thread(&f1724::Run, this);
  return 0;
}

int f1724::SINStart() {
  return SoftwareStart();
}

int f1724::AcquisitionStop(bool i_mean_it) {
  if (!i_mean_it) return 0;
  GlobalDeinit();
  sRun = false;
  fCV.notify_one();
  if (fGeneratorThread.joinable()) fGeneratorThread.join();
  Reset();
  return 0;
}

int f1724::Reset() {
  const std::lock_guard<std::mutex> lg(fBufferMutex);
  fBuffer.clear();
  fEventCounter = 0;
  fBufferSize = 0;
  return 0;
}

int f1724::GetClockCounter(uint32_t timestamp) {
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

std::tuple<double, double, double> f1724::GenerateEventLocation() {
  double offset = 0.5; // min number of PMTs between S1 and S2 to prevent overlap
  double z = -1.*sFlatDist(sGen)*((2*sFaxOptions.tpc_size+1)-offset)-offset;
  double r = sFlatDist(sGen)*sFaxOptions.tpc_size; // no, this isn't uniform
  double theta = sFlatDist(sGen)*2*PI();
  return {r*std::cos(theta), r*std::sin(theta), z};
}

vector<int> f1724::GenerateEventSize(double, double, double z) {
  int s1 = sFlatDist(sGen)*19+11;
  std::normal_distribution<> s2_over_s1{100, 20};
  double elivetime_loss_fraction = std::exp(z/sFaxOptions.e_absorbtion_length);
  int s2 = s1*s2_over_s1(sGen)*elivetime_loss_fraction;
  return {0, s1, s2};
}

vector<hit_t> f1724::MakeHitpattern(int s_i, int photons, double x, double y, double z) {
  double signal_width = s_i == 1 ? 40 : 1000.+200.*std::sqrt(std::abs(z));
  vector<hit_t> ret(photons);
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
    auto gen = [&](auto& p){
      return std::exp(-(std::pow(p.x-x, 2)+std::pow(p.y-y, 2))/gaus_width);
    };

    std::transform(sPMTxy.begin(), sPMTxy.begin()+TopPMTs, hit_prob.begin(), gen);
    // normalize
    double total_top_prob = std::accumulate(hit_prob.begin(),hit_prob.begin()+TopPMTs,0.);
    std::transform(hit_prob.begin(), hit_prob.begin()+TopPMTs, hit_prob.begin(),
        [&](double x){return top_fraction*x/total_top_prob;});
  }
  // bottom array probability simpler to calculate
  std::fill(hit_prob.begin()+TopPMTs, hit_prob.end(), (1.-top_fraction)/TopPMTs);

  hitpattern.param(std::discrete_distribution<>::param_type(hit_prob.begin(), hit_prob.end()));

  std::generate_n(ret.begin(), photons,
      [&]{return hit_t{hitpattern(sGen), f1724::sFlatDist(sGen)*signal_width};});
  return ret;
}

void f1724::SendToWorkers(const vector<hit_t>& hits) {
  vector<vector<hit_t>> hits_per_board(sRegistry.size());
  int n_boards = sRegistry.size();
  for (auto& hit : hits) {
    hits_per_board[hit.pmt_i%n_boards].emplace_back(hit_t{hit.pmt_i/n_boards, hit.time});
  }
  for (unsigned i = 0; i < sRegistry.size(); i++)
    if (hits_per_board[i].size() > 0)
      sRegistry[i]->ReceiveFromGenerator(std::move(hits_per_board[i]));
  return;
}

void f1724::ReceiveFromGenerator(vector<hit_t> hits) {
  {
    std::lock_guard<std::mutex> lk(fMutex);
    fProtoPulse = std::move(hits);
  }
  fCV.notify_one();
}

void f1724::MakeWaveform(std::vector<hit_t>& hits) {
  int mask = 0;
  double last_hit_time = 0, first_hit_time = 1e9;
  for (auto& hit : hits) {
    mask |= (1<<hit.ch_i);
    last_hit_time = std::max(last_hit_time, hit.time);
    first_hit_time = std::min(first_hit_time, hit.time);
  }
  timestamp += first_hit_time;
  // which channels contribute?
  vector<int> pmt_to_ch(GetNumChannels(), -1);
  int j = 0;
  for(unsigned ch = 0; ch < pmt_to_ch.size(); ch++) {
    if (mask & (1<<ch)) {
      pmt_to_ch[ch] = j++;
    }
  }
  int wf_length = fSPEtemplate.size() + last_hit_time/fSampleWidth;
  wf_length += wf_length % 2 ? 1 : 2; // ensure an even number of samples with room
  auto wf = GenerateNoise(wf_length, mask);
  std::normal_distribution<> hit_scale{1., 0.15};
  int offset = 0, sample_width = GetSampleWidth();
  double scale;
  for (auto& hit : hits) {
    offset = hit.time/sample_width;
    scale = hit_scale(fGen);
    for (unsigned i = 0; i < fSPEtemplate.size(); i++) {
      wf[pmt_to_ch[hit.ch_i]][offset+i] -= fSPEtemplate[i]*scale;
    }
  }
  return ConvertToDigiFormat(wf, mask, timestamp);
}

void f1724::ConvertToDigiFormat(const vector<vector<double>>& wf, int mask, long ts) {
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

vector<vector<double>> f1724::GenerateNoise(int length, int mask) {
  vector<vector<double>> ret;
  unsigned n_chan = GetNumChannels();
  ret.reserve(n_chan);
  for (unsigned i = 0; i < n_chan; i++) {
    if (mask & (1<<i)) {
      ret.emplace_back(length, 0);
      std::normal_distribution<> noise{fBaseline[i], fNoiseRMS[i]};
      std::generate(ret.back().begin(), ret.back().end(), [&]{return noise(fGen);});
    }
  }
  return ret;
}

void f1724::Run() {
  while (sRun == true) {
    std::unique_lock<std::mutex> lk(fMutex);
    fCV.wait(lk, []{return fProtoPulse.size() > 0 || sRun == false;});
    if (fProtoPulse.size() > 0 && sRun == true) {
      MakeWaveform(fProtoPulse);
      fProtoPulse.clear();
    } else {
    }
    lk.unlock();
  }
}

void f1724::GlobalRun() {
  std::exponential_distribution<> rate(sFaxOptions.rate/1e9); // Hz to /ns
  double x, y, z, t_max;
  long time_to_next;
  vector<int> photons; // S1 = 1
  vector<hit_t> hits;
  sClock = (0.5+sFlatDist(sGen))*10000;
  sEventCounter = 0;
  {
    std::unique_lock<std::mutex> lg(sMutex);
    sCV.wait(lg, []{return sReady == false;});
  }
  sLog->Entry(MongoLog::Local, "f1724::GlobalRun");
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
        t_max = std::max(t_max, hit.time);
      }

      time_to_next = (s_i == 1 ? std::abs(z/sFaxOptions.drift_speed) : rate(sGen)) + t_max;
      sClock += time_to_next;
      std::this_thread::sleep_for(std::chrono::nanoseconds(time_to_next));
    }
    sEventCounter++;
  }
  auto t_end = std::chrono::high_resolution_clock::now();
  sLog->Entry(MongoLog::Local, "Generation lasted %lx/%lx", sClock,
          std::chrono::duration_cast<std::chrono::nanoseconds>(t_end-t_start).count());
  sLog->Entry(MongoLog::Local, "f1724::GlobalRun finished");
}

