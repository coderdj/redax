#include "WFSim.hh"
#include <chrono>
#include <cmath>

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

int WFSim::WriteRegister(unsigned int reg, unsigned int) {

}

unsigned int WFSim::ReadReigster(unsigned int reg) {

}

int WFSim::Init(int link, int crate, int bid, unsigned int address) {
  link = crate = 0;
  fBID = bid;
  bRun = true;
  fGen = std::mt19937_64(fRD());
  fFlatDist = std::uniform_real_distribution<>(0., 1.);
  fSPEtemplate = {0.0, 0.0, 0.0, 2.81,-2, 7.4, 6.07e1, 3.26e1, 1.33e1, 7.60, 5.71, 7.75, 4.46,
      3.68, 3.31, 2.97, 2.74, 2.66, 2.48, 2.27, 2.15, 2.03, 1.93, 1.70, 1.68, 1.26, 7.86e-1,
      5.36e-1, 4.36e-1, 3.11e-1, 2.15e-1};
  if (fOptions->GetFaxOptions(fFaxOptions)) {
    // fail
  }

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
  std::uniform_real_distribution<> flat_dist(0., 1.);
  double z = -1.*fFlatDist(fGen)*fFaxOptions.tpc_length+2; // min 2us so peaks don't overlap
  double r = fFlatDist(fGen)*fFaxOptions.tpc_radius;
  double theta = fFlatDist(fGen)*2*PI;
  double x = r*std::cos(theta), y = r*std::sin(theta);
  return {x,y,z};
}

std::tuple<int, int> WFSim::GenerateEventSize(double x, double y, double z) {
  s1 = fFlatDist(fGen)*10;
  std::normal_distribution<> s2s1({100, 20});
  double elivetime_loss_fraction = std::exp(-z/fFaxOptions.e_absorbtion_dist);
  s2 = s1*s2s1(fGen)*elivetime_loss_fraction;
  return {s1, s2};
}

std::vector<std::pair<int, double>> MakeHitpattern(s_type s_i, int photons, double x, double y, double z,
    std::vector<int>& hit_pmt, std::vector<double>& hit_delta_t) {
  double r = std::hypot(x,y), theta = std::atan2(y,x);
  double signal_width = s_i == 1 ? 40 : 1000.+200*std::sqrt(z);
  hit_pmt.clear();
  hit_pmt.reserve(photons);
  std::vector<double> hit_probability(fNumPMTs, 0.);
  hit_delta_t.clear();
  hit_delta_t.reserve(photons);
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

  std::generate_n(hit_delta_t.begin(), photons, [&]{return fFlatDist(fGen);});
  std::generate_n(hit_pmt.begin(), photons, [&]{return hitpattern(fGen);});
}

std::vector<std::vector<double>> WFSim::MakeWaveform(std::vector<int>& hit_pmt,
    std::vector<double>& hit_time, int& mask) {
  mask = 0;
  for (auto& iter : hit_pmt) mask |= (1<<*iter);
  std::vector<std::vector<double>> wf (std::bitset<fNumPMTs>(mask).count(), std::vector<double>(wf_length, 16000.);

  for (unsigned i = 0; i < hit_pmt.size(); i++) {
    for (unsigned j = 0; j < fSPEtemplate.size(); j++) {
      wf[hit_pmt[i]][offset+j] -= fSPEtemplate[j];
    }
  }

  return wf;
}

void WFSim::ConvertToDigiFormat(std::vector<std::vector<double>>&& wf, int mask) {
  int num_channels = std::bitset<fNumPMTs>(mask).count();
  const int overhead_per_channel = 2*sizeof(uint32_t), overhead_per_event = 4*sizeof(uint32_t);
  std::string buffer;
  uint32_t word = 0;
  for (auto& ch : wf) word += ch.size();
  buffer.reserve(word/2 + overhead_per_channel*num_channels + overhead_per_event);
  word = (word>>2) | 0xA00000000;
  buffer.append((char*)&word, sizeof(word));
  buffer.append((char*)&mask, sizeof(mask));
  buffer.append((char*)&fEventCounter, sizeof(fEventCounter));
  uint32_t timestamp = fClock/DataFormatDefinition["ns_per_sample"];
  buffer.append((char*)&timestamp, sizeof(timestamp));
  uint16_t sample;
  for (auto& ch_wf : wf) {
    word = ch_wf.size()/2;
    buffer.append((char*)&word, sizeof(word));
    buffer.append((char*)&timestamp, sizeof(timestamp));
    for (unsigned i = 0; i < ch_wf.size(); i += 2) {
      sample = std::max(ch_wf[i], 0);
      word = sample;
      sample = std::max(ch_wf[i+1], 0);
      word |= (sample << 16);
      buffer.append((char*)&word, sizeof(word));
    }
  }
  {
    const std::lock_guard<std::mutex> lg(fBufferMutex);
    fBuffer.append(buffer);
    fBufferSize = fBuffer.size();
  }
}

void WFSim::Run() {
  using namespace std::chrono;
  std::exponential_distribution<> rate(fFaxOptions.rate);
  double x, y, z;
  int s1, s2;
  std::vector<int> v1;
  std::vector<double> v2;
  fClock = (0.5+fFlatDist(fGen))*10000000;
  fEventCounter = 0;
  while (bRun == true) {
    GenerateEventLocation(x, y, z);
    [s1, s2] = GenerateEventSize(x, y, z);
    [v1, v2, dt] = MakeHitpattern(S1, s1, x, y, z);
    fClock += dt;
    MakeWaveform(v1, v2);
    drift_time = z/fFaxOptions.drift_speed;
    std::this_thread::sleep_for(nanoseconds(drift_time));
    fClock += drift_time;
    fClock += MakeHitpattern(S2, s2, x, y, z);
    time_to_next_event = rate(fGen);
    std::this_thread::sleep_for(nanoseconds(time_to_next_event));
    fClock += time_to_next_event;
    fEventCounter++;
  }
}
