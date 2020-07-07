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
bool WFSim::sInit;
fax_options_t WFSim::sFaxOptions;
int WFSim::sNumPMTs;
vector<WFSim*> WFSim::sRegistry;
vector<pair<double, double>> sPMTxy;
std::condition_variable sCV;

pair<double, double> PMTiToXY(int i) {
  if (i == 0) return std::make_pair(0., 0.);
  if (i < 7) return std::make_pair(std::cos((i-1)*PI()/3.), std::sin((i-1)*PI()/3.));
  int ring = 2;
  // how many total PMTs are contained in a radius r? aka which ring is this PMT im
  while (i > 3*ring*(ring+1)) ring++;
  int i_in_ring = i - (1 + 3*ring*(ring-1));
  int side = i_in_ring / ring;
  int side_i = i_in_ring % ring;

  double ref_angle = PI()/3*side;
  double offset_angle = ref_angle + 2*PI()/3;
  double x_c = ring*std::cos(ref_angle), y_c = ring*std::sin(ref_angle);
  return std::make_pair(x_c + side_i*std::cos(offset_angle), y_c + side_i*std::sin(offset_angle));
}

WFSim::WFSim(MongoLog* log, Options* options) : V1724(log, options) {
  fLog->Entry(MongoLog::Warning, "Initializing fax digitizer");
}

WFSim::~WFSim() {
  End();
}

int WFSim::End() {
  fRun = false;
  if (fGeneratorThread.joinable()) fGeneratorThread.join();
  return 0;
}

int WFSim::WriteRegister(unsigned int, unsigned int) {
  return 0;
}

unsigned int WFSim::ReadRegister(unsigned int) {
  return 0;
}

int WFSim::Init(int, int, int bid, unsigned int) {
  fBID = bid;
  fGen = std::mt19937_64(fRD());
  fFlatDist = std::uniform_real_distribution<>(0., 1.);
  fSPEtemplate = {0.0, 0.0, 0.0, 2.81e-2, 7.4, 6.07e1, 3.26e1, 1.33e1, 7.60, 5.71, 7.75, 4.46,
      3.68, 3.31, 2.97, 2.74, 2.66, 2.48, 2.27, 2.15, 2.03, 1.93, 1.70, 1.68, 1.26, 7.86e-1,
      5.36e-1, 4.36e-1, 3.11e-1, 2.15e-1};
  if (fOptions->GetFaxOptions(fFaxOptions)) {
    fLog->Entry(MongoLog::Message, "Using default fax options");
    fFaxOptions.rate = 1e-8; // 10 Hz in ns
    fFaxOptions.tpc_size = 2; // radius in PMTs
    fFaxOptions.e_absorbtion_length = (fFlatDist(fGen)+1)*fFaxOptions.tpc_size; // TPC lengths
    fFaxOptions.drift_speed = 1e-4; // pmts/ns
  }
  GlobalInit(fFaxOptions);
  sRegistry.push_back(this);
  return 0;
}

void WFSim::GlobalInit(fax_options_t& fax_options) {
  if (sReady == false) {
    sGen = std::mt19937_64(sRD());
    sFlatDist = std::uniform_real_distribution<>(0., 1.);
    sFaxOptions = fax_options;

    sRun = sReady = true;
    sClock = 0;
    sEventCounter = 0;
    sNumPMTs = (1+3*fax_options.tpc_size*(fax_options.tpc_size+1))*2;
    int PMTsPerArray = sNumPMTs/2;
    sPMTxy.reserve(sNumPMTs);
    for (int p = 0; p < sNumPMTs; p++)
      sPMTxy.emplace_back(PMTiToXy(p % PMTsPerArray));

    sGeneratorThread = std::thread(&WFSim::GlobalRun);
  }
}

void WFSim::GlobalDeinit() {
  const std::lock_guard<std::mutex> lg(sMutex);
  if (sGeneratorThread.joinable()) {
    sRun = sReady = false;
    sGeneratorThread::join();
  }
}

uint32_t WFSim::GetAcquisitionStatus() {
  uint32_t ret = 0;
  ret |= 0x4*(fRun == true); // run status
  ret |= 0x8*(fBufferSize > 0); // event ready
  ret |= 0x80; // no PLL unlock
  ret |= 0x100*(sRun == true || sReady == true); // board is ready
  ret |= 0x800*(sRun == true); // S-IN

  return ret;
}

int WFSim::SoftwareStart() {
  if (sReady == true) {
    sRun = true;
    sReady = false;
    sCV.notify_one();
  }
  fRun = true;
  fGeneratorThread = std::thread(&WFSim::Run, this);
  return 0;
}

int WFSim::SINStart() {
  return SoftwareStart();
}

int WFSim::AcquisitionStop() {
  GlobalDeinit();
  fRun = false;
  if (fGeneratorThread.joinable() fGeneratorThread.join();
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

int WFSim::ReadMBLT(uint32_t* &buffer) {
  if (fBufferSize == 0) return 0;
  const std::lock_guard<std::mutex> lg(fBufferMutex);
  int numbytes = fBuffer.size();
  buffer = new uint32_t[numbytes];
  std::memcpy(buffer, fBuffer.data(), numbytes);
  fBuffer.clear();
  fBufferSize = 0;
  return numbytes;
}

std::tuple<double, double, double> WFSim::GenerateEventLocation() {
  double offset = 0.5; // min number of PMTs between S1 and S2 to prevent overlap
  double z = -1.*sFlatDist(fGen)*(2*sFaxOptions.tpc_size-offset)-offset;
  double r = sFlatDist(fGen)*sFaxOptions.tpc_radius; // no, this isn't uniform
  double theta = sFlatDist(fGen)*2*PI();
  return {r*std::cos(theta), r*std::sin(theta), z};
}

std::array<int, 3> WFSim::GenerateEventSize(double, double, double z) {
  int s1 = sFlatDist(sGen)*19+1;
  std::normal_distribution<> s2_over_s1{100, 20};
  double elivetime_loss_fraction = std::exp(-z/sFaxOptions.e_absorbtion_length);
  int s2 = s1*s2_over_s1(sGen)*elivetime_loss_fraction;
  return {0, s1, s2};
}

vector<pair<int, double>> WFSim::MakeHitpattern(int s_i, int photons, double x, double y, double z) {
  double r = std::hypot(x,y), theta = std::atan2(y,x);
  double signal_width = s_i == 1 ? 40 : 1000.+200.*std::sqrt(std::abs(z));
  vector<pair<int, double>> ret(photons); // { {pmt id, hit time}, {pmt id, hit time} ... }
  vector<double> hit_prob(sNumPMTs, 0.);
  std::discrete_distribution<> hitpattern;
  double top_fraction(0);
  int TopPMTs = sNumPMTs/2;
  if (s_i == 1) {
    top_fraction = (0+sNumPMTs)/(0.4-0.1)*(z+sNumPMTs)+0.1; // 10% at bottom, 40% at top
    std::fill_n(hit_prob.begin(), TopPMTs, top_fraction/TopPMTs);
  } else {
    top_fraction = 0.65;
    // let's go with a Gaussian probability, because why not
    double gaus_std = 1.3; // PMTs wide
    auto gen = [&](std::pair<double, double>& p){
      return std::exp(-(std::pow(p.first-x, 2)+std::pow(p.second-y, 2))/(2*gaus_std*gaus_std));
    }

    std::transform(sPMTxy.begin(), sPMTxy.begin()+TopPMTs, hit_prob.begin(), gen);
    // normalize
    double total_top_prob = std::accumulate(hit_prob.begin(), hit_prob.begin()+TopPMTs, 0.);
    std::transform(hit_prob.begin(), hit_prob.begin()+TopPMTs, hit_prob.begin(),
        [](double x){return top_fraction*x/total_top_prob;});
  }
  // bottom array probability simpler to calculate
  std::fill(hit_prob.begin()+TopPMTs, hit_prob.end(), (1.-top_fraction)/TopPMTs);

  hitpattern.param(std::discrete_distribution<>::param_type(hit_prob.begin(), hit_prob.end()));

  std::generate_n(ret.begin(), photons,
      [&]{return std::make_pair(hitpattern(sGen), WFSim::sFlatDist(sGen)*signal_width);});
  return ret;
}

void WFSim::SendToWorkers(const vector<pair<int, double>>& hits) {
  vector<vector<pair<int, double>>> hits_per_board(sRegistry.size());
  for (auto& hit : hits) {
    hits_per_board[hit.first/PMTsPerDigi].emplace_back(hit.first % PMTsPerDigi, hit.second);
  }
  for (unsigned i = 0; i < sRegistry.size(); i++)
    sRegistry[i]->ReceiveFromGenerator(hits_per_board[i], sClock);
  return;
}

void WFSim::ReceiveFromGenerator(const vector<pair<int, double>>& in, long timestamp) {
  {
    const std::lock_guard<std::mutex> lg(fMutex);
    fWFprimitive = in;
    fTimestamp = timestamp;
    fEventCounter++;
  }
  fCV.notify_one();
  return;
}

vector<vector<double>> WFSim::MakeWaveform(const vector<pair<int, double>>& hits, int& mask) {
  mask = 0;
  double last_hit_time = 0;
  double first_hit_time = 1e9;
  for (auto& hit : hits) {
    mask |= (1<<hit.first);
    last_hit_time = std::max(last_hit_time, hit.second);
    first_hit_time = std::min(first_hit_time, hit.second);
  }
  fTimestamp += first_hit_time;
  // which channels contribute?
  vector<int> pmt_arr(PMTsPerDigi, -1);
  unsigned j = 0;
  for (int i = 0; i < PMTsPerDigi; i++)
    if (mask & (1<<i)) pmt_arr[i] = j++;

  int wf_length = fSPEtemplate.size() + last_hit_time/DataFormatDefinition["ns_per_sample"];
  wf_length += wf_length % 2 ? 1 : 2; // ensure an even number of samples with room
  // start with a positive-going pulse, then invert in the next stage when we apply
  // whatever baseline we want
  vector<vector<double>> wf(j, vector<double>(wf_length, 0.));
  std::normal_distribution<> hit_scale{1., 0.15};
  int offset;
  double scale;
  for (auto& hit : hits) {
    offset = hit.second/DataFormatDefinition["ns_per_sample"];
    scale = hit_scale(fGen);
    for (j = 0; j < fSPEtemplate.size(); j++) {
      wf[pmt_arr[hit.first]][offset+j] += fSPEtemplate[j]*scale;
    }
  }
  return wf;
}

void WFSim::ConvertToDigiFormat(const vector<vector<double>>& wf, int mask) {
  const int overhead_per_channel = 2*sizeof(uint32_t), overhead_per_event = 4*sizeof(uint32_t);
  std::string buffer;
  uint32_t word = 0;
  for (auto& ch : wf) word += ch.size(); // samples
  int words_this_event = word/2 + overhead_per_channel*wf.size() + overhead_per_event;
  buffer.reserve(words_this_event);
  word = words_this_event | 0xA00000000;
  buffer.append((char*)&word, sizeof(word));
  buffer.append((char*)&mask, sizeof(mask));
  buffer.append((char*)&fEventCounter, sizeof(fEventCounter));
  uint32_t timestamp = fClock/DataFormatDefinition["ns_per_sample"];
  buffer.append((char*)&timestamp, sizeof(timestamp));
  uint16_t sample, baseline(16000);
  for (auto& ch_wf : wf) {
    word = ch_wf.size()/2 + overhead_per_channel; // size is in samples
    buffer.append((char*)&word, sizeof(word));
    buffer.append((char*)&timestamp, sizeof(timestamp));
    for (unsigned i = 0; i < ch_wf.size(); i += 2) {
      sample = ch_wf[i] > baseline ? 0 : baseline-ch_wf[i];
      word = sample;
      sample = ch_wf[i+1] > baseline ? 0 : baseline-ch_wf[i+1];
      word |= (sample << 16);
      buffer.append((char*)&word, sizeof(word));
    } // loop over samples
  } // loop over channels
  {
    const std::lock_guard<std::mutex> lg(fBufferMutex);
    fBuffer.append(buffer);
    fBufferSize = fBuffer.size();
  }
  return;
}

int WFSim::NoiseInjection() {
  vector<vector<double>> wf(PMTsPerDigi, vector<double>(fSPEtemplate.size(), 0));
  ConvertToDigiFormat(wf, 0xFF);
  return 0;
}

static void WFSim::GlobalRun() {
  std::exponential_distribution<> rate(sFaxOptions.rate);
  double x, y, z, t_max;
  int mask;
  long time_to_next;
  std::array<int, 3> photons; // S1 = 1
  vector<pair<int, double>> hits;
  sClock = (0.5+sFlatDist(fGen))*10000000;
  sEventCounter = 0;
  {
    const std::lock_guard<std::mutex> lg(sMutex);
    sCV.wait(lg, []{return sReady == false;});
  }
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

      time_to_next = (s_i == 1 ? std::abs(z/fFaxOptions.drift_speed) : rate(fGen)) + t_max;
      sClock += time_to_next;
      std::this_thread::sleep_for(std::chrono::nanoseconds(time_to_next));
    }
    sEventCounter++;
  }
}

void WFSim::Run() {
  vector<vector<double>> wf;
  int mask;
  while (sRun == true) {
    {
      std::unique_lock<std::mutex> lk(fMutex);
      fCV.wait(lk, [&]{return fWFprimitive.size();});
      wf = MakeWaveform(fWFprimitive, mask);
    }
    ConvertToDigiFormat(wf, mask);
  }
}

void WFSim::WaitForSignal() {

}
