#include "WFSim.hh"
#include "MongoLog.hh"
#include "Options.hh"
#include <chrono>
#include <cmath>

using std::vector;
using std::tuple;
using std::pair;
const double PI = std::acos(-1.);

WFSim::WFSim(MongoLog* log, Options* options) : V1724(log, options), fNumPMTs(8) {
  fLog->Entry(MongoLog::Warning, "Initializing fax digitizer");
}

WFSim::~WFSim() {
  End();
}

int WFSim::End() {
  fRun = false;
  if (fGeneratorThread.joinable()) fGeneratorThread.join();
}

int WFSim::Reset() {
  const std::lock_guard<std::mutex> lg;
  fBuffer.clear();
  fBufferSize = 0;
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
  fSPEtemplate = {0.0, 0.0, 0.0, 2.81,-2, 7.4, 6.07e1, 3.26e1, 1.33e1, 7.60, 5.71, 7.75, 4.46,
      3.68, 3.31, 2.97, 2.74, 2.66, 2.48, 2.27, 2.15, 2.03, 1.93, 1.70, 1.68, 1.26, 7.86e-1,
      5.36e-1, 4.36e-1, 3.11e-1, 2.15e-1};
  if (fOptions->GetFaxOptions(fFaxOptions)) {
    fLog->Entry(MongoLog::Message, "Using default fax options");
    fFaxOptions.rate = 1e-7; // 100 Hz in ns
    fFaxOptions.tpc_radius = 35; // mm
    fFaxOptions.rpc_length = 70; // mm
    fFaxOptions.e_absorbtion_dist = 70; // mm
    fFaxOptions.drift_speed = 1.5e-3 // mm/ns
  }

}

int WFSim::SoftwareStart() {
  fRun = true;
  fGeneratorThread = std::thread(&WFSim::Run, this);
  return 0;
}

int WFSim::Reset() {
  const std::lock_guard<std::mutex> lg(fBufferMutex);
  fBuffer.clear();
  fClock = 0;
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

tuple<double, double, double> WFSim::GenerateEventLocation() {
  double z = -1.*fFlatDist(fGen)*(fFaxOptions.tpc_length-1)-1; // min so peaks don't overlap
  double r = fFlatDist(fGen)*fFaxOptions.tpc_radius; // no, this isn't uniform
  double theta = fFlatDist(fGen)*2*PI;
  double x = r*std::cos(theta), y = r*std::sin(theta);
  return {x,y,z};
}

tuple<int, int> WFSim::GenerateEventSize(double x, double y, double z) {
  int s1 = fFlatDist(fGen)*19+1;
  std::normal_distribution<> s2_over_s1({100, 20});
  double elivetime_loss_fraction = std::exp(-z/fFaxOptions.e_absorbtion_length);
  int s2 = s1*s2_over_s1(fGen)*elivetime_loss_fraction;
  return {s1, s2};
}

vector<pair<int, double>> MakeHitpattern(s_type s_i, int photons, double x, double y, double z) {
  double r = std::hypot(x,y), theta = std::atan2(y,x);
  double signal_width = s_i == 1 ? 40 : 1000.+200.*std::sqrt(std::abs(z));
  vector<pair<int, double>> ret; // { {pmt id, hit time}, {pmt id, hit time} ... }
  ret.reserve(photons);
  vector<double> hit_probability(fNumPMTs, 0.);
  std::discrete_distribution<> hitpattern;
  if (s_i == S1) {
    hit_probability[0] = -0.3/70*z+0.6;
    double prob_left = 1.-hit_probability[0];
    for (auto it = hit_probability.begin()+1, it < hit_probability.end(); it++) *it = prob_left/6;
  } else {
    int max_pmt = r < 25 ? 1 : 2+6*theta/(2*PI);
    hit_probability[0] = 0.3;
    hit_probability[max_pmt] = 0.5;
    if (max_pmt == 1) {
      for (auto it = hit_probability.begin()+2; it < hit_probability.end(); it++) *it = 0.2/6;
    } else {
      hit_probability[1] = 0.2/3;
      hit_probability[max_pmt == 2 ? fNumPMTs-1 : max_pmt-1] = 0.2/3;
      hit_probability[max_pmt == fNumPMTs-1 ? 2 : max_pmt+1] = 0.2/3;
    }
  }
  hitpattern.param(std::discrete_distribution::param_type(hit_probability.begin(), hit_probability.end()));

  std::generate_n(ret.begin(), photons,
      [&]{return std::make_pair(hitpattern(fGen), fFlatDist(fGen)*signal_width);});
  return ret;
}

vector<vector<double>> WFSim::MakeWaveform(vector<pair<int, double>>& hits, int& mask) {
  mask = 0;
  double last_hit_time = 0;
  for (auto& hit : hits) {
    mask |= (1<<iter.first);
    last_hit_time = std::max(latest_hit_time, iter.second);
  }
  std::array<int, fNumPMTs> pmt_arr;
  unsigned j = 0;
  for (int i = 0; i < fNumPMTs; i++) pmt_arr[i] = (mask & (1<<i)) ? j++ : -1;
  int wf_length = fSPEtemplate.size() + last_hit_time/DataFormatDefinition["ns_per_sample"];
  wf_length += wf_length % 2 ? 1 : 2; // ensure an even number of samples with room
  // start with a positive-going pulse, then invert in the next stage when we apply
  // whatever baseline we want
  vector<vector<double>> wf(std::bitset<fNumPMTs>(mask).count(), vector<double>(wf_length, 0.));
  int offset;
  for (auto& hit : hits) {
    offset = hit.second/DataFormatDefinition["ns_per_sample"];
    for (j = 0; j < fSPEtemplate.size(); j++) {
      wf[pmt_arr[hit.first]][offset+j] += fSPEtemplate[j];
    }
  }
  return wf;
}

int WFSim::ConvertToDigiFormat(vector<vector<double>>& wf, int mask) {
  int num_channels = std::bitset<fNumPMTs>(mask).count();
  const int overhead_per_channel = 2*sizeof(uint32_t), overhead_per_event = 4*sizeof(uint32_t);
  std::string buffer;
  uint32_t word = 0;
  for (auto& ch : wf) word += ch.size(); // samples
  buffer.reserve(word/2 + overhead_per_channel*num_channels + overhead_per_event);
  word = (word>>2) | 0xA00000000;
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
  return wf[0].size()*DataFormatDefinition["ns_per_sample"]; // width of signal
}

void WFSim::Run() {
  std::exponential_distribution<> rate(fFaxOptions.rate);
  double x, y, z;
  int mask;
  long time_to_next;
  tuple<int, int> photons;
  vector<pair<int, double>> hits;
  vector<vector<double>> wf;
  fClock = (0.5+fFlatDist(fGen))*10000000;
  fEventCounter = 0;
  while (fRun == true) {
    std::tie(x,y,z) = GenerateEventLocation();
    photons = GenerateEventSize(x, y, z);
    for (auto s_i : {S1, S2}) {
      hits = MakeHitpattern(s_i, std::get<s_i>(photons), x, y, z);
      wf = MakeWaveform(hits);
      fClock += ConvertToDigiFormat(wf, mask);
      time_to_next = s == S1 ? z/fFaxOptions.drift_speed : rate(fGen);
      fClock += time_to_next;
      std::this_thread::sleep_for(std::chrono::nanoseconds(time_to_next));
    }
    fEventCounter++;
  }
}
