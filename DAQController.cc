#include "DAQController.hh"
#include "V1724.hh"
#include "V1724_MV.hh"
#include "V1730.hh"
#include "f1724.hh"
#include "DAXHelpers.hh"
#include "Options.hh"
#include "StraxFormatter.hh"
#include "MongoLog.hh"
#include <algorithm>
#include <bitset>
#include <chrono>
#include <cmath>
#include <numeric>

#include <bsoncxx/builder/stream/document.hpp>

// Status:
// 0-idle
// 1-arming
// 2-armed
// 3-running
// 4-error

DAQController::DAQController(std::shared_ptr<MongoLog>& log, std::string hostname){
  fLog=log;
  fOptions = nullptr;
  fStatus = DAXHelpers::Idle;
  fReadLoop = false;
  fNProcessingThreads=8;
  fDataRate=0.;
  fHostname = hostname;
}

DAQController::~DAQController(){
  if(fProcessingThreads.size()!=0)
    CloseThreads();
}

int DAQController::Arm(std::shared_ptr<Options>& options){
  fOptions = options;
  fLog->SetRunId(fOptions->GetInt("number", -1));
  fNProcessingThreads = fOptions->GetNestedInt("processing_threads."+fHostname, 8);
  fLog->Entry(MongoLog::Local, "Beginning electronics initialization with %i threads",
	      fNProcessingThreads);

  // Initialize digitizers
  fStatus = DAXHelpers::Arming;
  std::vector<int> BIDs;
  for(auto d : fOptions->GetBoards("V17XX")){
    fLog->Entry(MongoLog::Local, "Arming new digitizer %i", d.board);

    std::shared_ptr<V1724> digi;
    try{
      if(d.type == "V1724_MV")
        digi = std::make_shared<V1724_MV>(fLog, fOptions, d.link, d.crate, d.board, d.vme_address);
      else if(d.type == "V1730")
        digi = std::make_shared<V1730>(fLog, fOptions, d.link, d.crate, d.board, d.vme_address);
      else if(d.type == "f1724")
        digi = std::make_shared<f1724>(fLog, fOptions, d.link, d.crate, d.board, 0);
      else
        digi = std::make_shared<V1724>(fLog, fOptions, d.link, d.crate, d.board, d.vme_address);
      fDigitizers[d.link].emplace_back(digi);
      BIDs.push_back(digi->bid());
    }catch(const std::exception& e) {
      fLog->Entry(MongoLog::Warning, "Failed to initialize digitizer %i: %s", d.board,
          e.what());
      fDigitizers.clear();
      return -1;
    }
  }
  fLog->Entry(MongoLog::Local, "This host has %i boards", BIDs.size());
  fLog->Entry(MongoLog::Local, "Sleeping for two seconds");
  // For the sake of sanity and sleeping through the night,
  // do not remove this statement.
  sleep(2); // <-- this one. Leave it here.
  // Seriously. This sleep statement is absolutely vital.
  fLog->Entry(MongoLog::Local, "That felt great, thanks.");
  std::map<int, std::map<std::string, std::vector<double>>> dac_values;
  if (fOptions->GetString("baseline_dac_mode") == "cached")
    fOptions->GetDAC(dac_values, BIDs);
  std::vector<std::thread*> init_threads;
  std::map<int,int> rets;
  // Parallel digitizer programming to speed baselining
  for( auto& link : fDigitizers ) {
    rets[link.first] = 1;
    init_threads.push_back(new std::thread(&DAQController::InitLink, this,
	  std::ref(link.second), std::ref(dac_values), std::ref(rets[link.first])));
  }
  std::for_each(init_threads.begin(), init_threads.end(),
      [](std::thread* t) {t->join(); delete t;});

  if (std::any_of(rets.begin(), rets.end(), [](auto& p) {return p.second != 0;})) {
    fLog->Entry(MongoLog::Warning, "Encountered errors during digitizer programming");
    if (std::any_of(rets.begin(), rets.end(), [](auto& p) {return p.second == -2;}))
      fStatus = DAXHelpers::Error;
    else
      fStatus = DAXHelpers::Idle;
    return -1;
  } else
    fLog->Entry(MongoLog::Debug, "Digitizer programming successful");
  if (fOptions->GetString("baseline_dac_mode") == "fit") fOptions->UpdateDAC(dac_values);

  for(auto& link : fDigitizers ) {
    for(auto& digi : link.second){
      if(fOptions->GetInt("run_start", 0) == 1)
	digi->SINStart();
      else
	digi->AcquisitionStop();
    }
  }
  fCounter = 0;
  if (OpenThreads()) {
    fLog->Entry(MongoLog::Warning, "Error opening threads");
    fStatus = DAXHelpers::Idle;
    return -1;
  }
  sleep(1);
  fStatus = DAXHelpers::Armed;

  fLog->Entry(MongoLog::Local, "Arm command finished, returning to main loop");
  return 0;
}

int DAQController::Start(){
  if(fOptions->GetInt("run_start", 0) == 0){
    for(auto& link : fDigitizers ){
      for(auto& digi : link.second){

	// Ensure digitizer is ready to start
	if(digi->EnsureReady(1000, 1000)!= true){
	  fLog->Entry(MongoLog::Warning, "Digitizer not ready to start after sw command sent");
	  return -1;
	}

	// Send start command
	digi->SoftwareStart();

	// Ensure digitizer is started
	if(digi->EnsureStarted(1000, 1000)!=true){
	  fLog->Entry(MongoLog::Warning,
		      "Timed out waiting for acquisition to start after SW start sent");
	  return -1;
	}
      }
    }
  }
  fStatus = DAXHelpers::Running;
  return 0;
}

int DAQController::Stop(){

  fReadLoop = false; // at some point.
  int counter = 0;
  bool one_still_running = false;
  do{
    one_still_running = false;
    for (auto& p : fRunning) one_still_running |= p.second;
    if (one_still_running) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }while(one_still_running && counter++ < 10);
  if (counter >= 10) fLog->Entry(MongoLog::Local, "Boards taking a while to clear");
  std::cout<<"Deactivating boards"<<std::endl;
  for( auto const& link : fDigitizers ){
    for(auto digi : link.second){
      digi->AcquisitionStop(true);

      // Ensure digitizer is stopped
      if(digi->EnsureStopped(1000, 1000) != true){
	fLog->Entry(MongoLog::Warning,
		    "Timed out waiting for %i to stop after SW stop sent", digi->bid());
          //return -1;
      }
    }
  }
  fLog->Entry(MongoLog::Debug, "Stopped digitizers, closing threads");
  CloseThreads();
  fLog->Entry(MongoLog::Local, "Closing Digitizers");
  for(auto& link : fDigitizers ){
    for(auto& digi : link.second){
      digi->End();
      digi.reset();
    }
    link.second.clear();
  }
  fDigitizers.clear();
  fStatus = DAXHelpers::Idle;

  fLog->SetRunId(-1);
  std::cout<<"Finished end"<<std::endl;
  fStatus = DAXHelpers::Idle;
  return 0;
}

void DAQController::ReadData(int link){
  fReadLoop = true;

  fDataRate = 0;

  uint32_t board_status = 0;
  int readcycler = 0;
  int err_val = 0;
  std::list<std::unique_ptr<data_packet>> local_buffer;
  std::unique_ptr<data_packet> dp;
  int words = 0;
  int local_size(0);
  fRunning[link] = true;
  std::chrono::microseconds sleep_time(fOptions->GetInt("us_between_reads", 10));
  while(fReadLoop){
    for(auto& digi : fDigitizers[link]) {

      // Every 1k reads check board status
      if(readcycler%10000==0){
        readcycler=0;
        board_status = digi->GetAcquisitionStatus();
        fLog->Entry(MongoLog::Local, "Board %i has status 0x%04x",
            digi->bid(), board_status);
      }
      if (digi->CheckFail()) {
        err_val = digi->CheckErrors();
        fLog->Entry(MongoLog::Local, "Error %i from board %i", err_val, digi->bid());
        if (err_val == -1 || err_val == 0) {

        } else {
          fStatus = DAXHelpers::Error; // stop command will be issued soon
          if (err_val & 0x1) fLog->Entry(MongoLog::Local, "Board %i has PLL unlock",
                                         digi->bid());
          if (err_val & 0x2) fLog->Entry(MongoLog::Local, "Board %i has VME bus error",
                                         digi->bid());
        }
      }
      if((words = digi->Read(dp))<0){
        dp.reset();
        fStatus = DAXHelpers::Error;
        break;
      } else if(words>0){
        dp->digi = digi;
        local_buffer.emplace_back(std::move(dp));
        local_size += words*sizeof(char32_t);
      }
    } // for digi in digitizers
    if (local_buffer.size() > 0) {
      fDataRate += local_size;
      int selector = (fCounter++)%fNProcessingThreads;
      fFormatters[selector]->ReceiveDatapackets(local_buffer, local_size);
      local_size = 0;
    }
    readcycler++;
    std::this_thread::sleep_for(sleep_time);
  } // while run
  fRunning[link] = false;
  fLog->Entry(MongoLog::Local, "RO thread %i returning", link);
}

int DAQController::OpenThreads(){
  const std::lock_guard<std::mutex> lg(fMutex);
  fProcessingThreads.reserve(fNProcessingThreads);
  for(int i=0; i<fNProcessingThreads; i++){
    try {
      fFormatters.emplace_back(std::make_unique<StraxFormatter>(fOptions, fLog));
      fProcessingThreads.emplace_back(&StraxFormatter::Process, fFormatters.back().get());
    } catch(const std::exception& e) {
      fLog->Entry(MongoLog::Warning, "Error opening processing threads: %s",
          e.what());
      return -1;
    }
  }
  fReadoutThreads.reserve(fDigitizers.size());
  for (auto& p : fDigitizers)
    fReadoutThreads.emplace_back(&DAQController::ReadData, this, p.first);
  return 0;
}

void DAQController::CloseThreads(){
  const std::lock_guard<std::mutex> lg(fMutex);
  fLog->Entry(MongoLog::Local, "Ending RO threads");
  for (auto& t : fReadoutThreads) if (t.joinable()) t.join();
  fLog->Entry(MongoLog::Local, "Joining processing threads");
  std::map<int,int> board_fails;
  for (auto& sf : fFormatters) {
    while (sf->GetBufferSize().first > 0)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    sf->Close(board_fails);
  }
  for (auto& t : fProcessingThreads) if (t.joinable()) t.join();
  fProcessingThreads.clear();
  fLog->Entry(MongoLog::Local, "Destroying formatters");
  for (auto& sf : fFormatters) sf.reset();
  fFormatters.clear();

  if (std::accumulate(board_fails.begin(), board_fails.end(), 0,
	[=](int tot, auto& iter) {return std::move(tot) + iter.second;})) {
    std::stringstream msg;
    msg << "Found board failures: ";
    for (auto& iter : board_fails) msg << iter.first << ":" << iter.second << " | ";
    fLog->Entry(MongoLog::Warning, msg.str());
  }
}

void DAQController::StatusUpdate(mongocxx::collection* collection) {
  auto insert_doc = bsoncxx::builder::stream::document{};
  std::map<int, int> retmap;
  std::pair<long, long> buf{0,0};
  int rate = fDataRate;
  fDataRate = 0;
  {
    const std::lock_guard<std::mutex> lg(fMutex);
    for (auto& p : fFormatters) {
      p->GetDataPerChan(retmap);
      auto x = p->GetBufferSize();
      buf.first += x.first;
      buf.second += x.second;
    }
  }
  insert_doc << "host" << fHostname <<
    "time" << bsoncxx::types::b_date(std::chrono::system_clock::now())<<
    "rate" << rate/1e6 <<
    "status" << fStatus <<
    "buffer_size" << (buf.first + buf.second)/1e6 <<
    "run_mode" << (fOptions ? fOptions->GetString("name", "none") : "none") <<
    "channels" << bsoncxx::builder::stream::open_document <<
      [&](bsoncxx::builder::stream::key_context<> doc){
      for( auto const& pair : retmap)
        doc << std::to_string(pair.first) << short(pair.second>>10); // KB not MB
      } << bsoncxx::builder::stream::close_document;
  collection->insert_one(insert_doc << bsoncxx::builder::stream::finalize);
  return;
}

void DAQController::InitLink(std::vector<std::shared_ptr<V1724>>& digis,
    std::map<int, std::map<std::string, std::vector<double>>>& cal_values, int& ret) {
  std::string BL_MODE = fOptions->GetString("baseline_dac_mode", "fixed");
  std::map<int, std::vector<uint16_t>> dac_values;
  int nominal_baseline = fOptions->GetInt("baseline_value", 16000);
  if (BL_MODE == "fit") {
    if ((ret = FitBaselines(digis, dac_values, nominal_baseline, cal_values))) {
      fLog->Entry(MongoLog::Warning, "Errors during baseline fitting");
      return;
    }
  }

  for(auto digi : digis){
    fLog->Entry(MongoLog::Local, "Board %i beginning specific init", digi->bid());

    // Multiple options here
    int bid = digi->bid(), success(0);
    if (BL_MODE == "fit") {
    } else if(BL_MODE == "cached") {
      fMutex.lock();
      auto board_dac_cal = cal_values.count(bid) ? cal_values[bid] : cal_values[-1];
      fMutex.unlock();
      dac_values[bid] = std::vector<uint16_t>(digi->GetNumChannels());
      fLog->Entry(MongoLog::Local, "Board %i using cached baselines", bid);
      for (unsigned ch = 0; ch < digi->GetNumChannels(); ch++)
	dac_values[bid][ch] = nominal_baseline*board_dac_cal["slope"][ch] + board_dac_cal["yint"][ch];
      digi->ClampDACValues(dac_values[bid], board_dac_cal);
    } else if(BL_MODE == "fixed"){
      int BLVal = fOptions->GetInt("baseline_fixed_value", 4000);
      fLog->Entry(MongoLog::Local, "Loading fixed baselines with value 0x%04x", BLVal);
      dac_values[bid] = std::vector<uint16_t>(digi->GetNumChannels(), BLVal);
    } else {
      fLog->Entry(MongoLog::Warning, "Received unknown baseline mode '%s', valid options are \"fit\", \"cached\", and \"fixed\"", BL_MODE.c_str());
      ret = -1;
      return;
    }

    if(success==-2){
      fLog->Entry(MongoLog::Warning, "Board %i Baselines failed with digi error");
      ret = -2;
      return;
    } else if(success!=0){
      fLog->Entry(MongoLog::Warning, "Board %i failed baselines with timeout", digi->bid());
      ret = -1;
      return;
    }

    for(auto& regi : fOptions->GetRegisters(bid)){
      unsigned int reg = DAXHelpers::StringToHex(regi.reg);
      unsigned int val = DAXHelpers::StringToHex(regi.val);
      success+=digi->WriteRegister(reg, val);
    }
    success += digi->LoadDAC(dac_values[bid]);
    // Load all the other fancy stuff
    success += digi->SetThresholds(fOptions->GetThresholds(bid));

    fLog->Entry(MongoLog::Local, "Board %i programmed", digi->bid());
    if(success!=0){
      //LOG
      fLog->Entry(MongoLog::Warning, "Failed to configure digitizers.");
      ret = -1;
      return;
    }
  } // loop over digis per link

  ret = 0;
  return;
}

int DAQController::FitBaselines(std::vector<std::shared_ptr<V1724>> &digis,
    std::map<int, std::vector<u_int16_t>> &dac_values, int target_baseline,
    std::map<int, std::map<std::string, std::vector<double>>> &cal_values) {
  using std::vector;
  using namespace std::chrono_literals;
  int max_iter = fOptions->GetInt("baseline_max_iterations", 2);
  unsigned ch_this_digi(0), max_steps = fOptions->GetInt("baseline_max_steps", 20);
  int adjustment_threshold = fOptions->GetInt("baseline_adjustment_threshold", 10);
  int convergence_threshold = fOptions->GetInt("baseline_convergence_threshold", 3);
  int min_adjustment = fOptions->GetInt("baseline_min_adjustment", 0xA);
  int rebin_factor = fOptions->GetInt("baseline_rebin_log2", 1); // log base 2
  int bins_around_max = fOptions->GetInt("baseline_bins_around_max", 3);
  int steps_repeated(0), max_repeated_steps(10), bid(0);
  int triggers_per_step = fOptions->GetInt("baseline_triggers_per_step", 3);
  std::chrono::milliseconds ms_between_triggers(fOptions->GetInt("baseline_ms_between_triggers", 10));
  vector<long> DAC_cal_points = {60000, 30000, 6000}; // arithmetic overflow
  std::map<int, vector<int>> channel_finished;
  std::map<int, std::unique_ptr<data_packet>> buffers;
  std::map<int, int> words_read;
  std::map<int, vector<vector<double>>> bl_per_channel;
  std::map<int, vector<int>> diff;

  for (auto digi : digis) { // alloc ALL the things!
    bid = digi->bid();
    ch_this_digi = digi->GetNumChannels();
    dac_values[bid] = vector<u_int16_t>(ch_this_digi, 0);
    channel_finished[bid] = vector<int>(ch_this_digi, 0);
    bl_per_channel[bid] = vector<vector<double>>(ch_this_digi, vector<double>(max_steps,0));
    diff[bid] = vector<int>(ch_this_digi, 0);
  }

  bool done(false), redo_iter(false), fail(false), calibrate(true);
  int counts_total(0), counts_around_max(0);
  double B,C,D,E,F, slope, yint, baseline;
  double fraction_around_max = fOptions->GetDouble("baseline_fraction_around_max", 0.8);
  u_int32_t words_in_event, channel_mask, words;
  u_int16_t val0, val1;
  int channels_in_event;

  for (int iter = 0; iter < max_iter; iter++) {
    if (done || fail) break;
    for (auto& vv : bl_per_channel) // vv = pair(int, vector<vector<double>>)
      for (auto& v : vv.second) // v = vector<double>
        v.assign(v.size(), 0);
    for (auto& v : channel_finished) v.second.assign(v.second.size(), 0);
    steps_repeated = 0;
    fLog->Entry(MongoLog::Local, "Beginning baseline iteration %i/%i", iter, max_iter);

    for (unsigned step = 0; step < max_steps; step++) {
      fLog->Entry(MongoLog::Local, "Beginning baseline step %i/%i", step, max_steps);
      if (std::all_of(channel_finished.begin(), channel_finished.end(),
            [&](auto& p) {
              return std::all_of(p.second.begin(), p.second.end(), [=](int i)
                  {return i >= convergence_threshold;});})) {
        fLog->Entry(MongoLog::Local, "All boards on this link finished baselining");
        done = true;
        break;
      }
      if (steps_repeated >= max_repeated_steps) {
        fLog->Entry(MongoLog::Debug, "Repeating a lot of steps here");
        break;
      }
      // prep
      if (step < DAC_cal_points.size()) {
        if (!calibrate) continue;
        for (auto d : digis)
          dac_values[d->bid()].assign(d->GetNumChannels(), (int)DAC_cal_points[step]);
      }
      for (auto d : digis) {
        if (d->LoadDAC(dac_values[d->bid()])) {
          fLog->Entry(MongoLog::Warning, "Board %i failed to load DAC", d->bid());
          return -2;
        }
      }
      // "After writing, the user is recommended to wait for a few seconds before
      // a new RUN to let the DAC output get stabilized" - CAEN documentation
      std::this_thread::sleep_for(1s);
      // sleep(2) seems unnecessary after preliminary testing

      // start board
      for (auto d : digis) {
        if (d->EnsureReady(1000,1000))
          d->SoftwareStart();
        else
          fail = true;
      }
      std::this_thread::sleep_for(5ms);
      for (auto d : digis) {
        if (!d->EnsureStarted(1000,1000)) {
          d->AcquisitionStop();
          fail = true;
        }
      }

      // send triggers
      for (int trig = 0; trig < triggers_per_step; trig++) {
        for (auto d : digis) d->SWTrigger();
        std::this_thread::sleep_for(ms_between_triggers);
      }
      // stop
      for (auto d : digis) {
        d->AcquisitionStop();
        if (!d->EnsureStopped(1000,1000)) {
          fail = true;
        }
      }
      if (fail) {
        for (auto d : digis) d->AcquisitionStop();
        fLog->Entry(MongoLog::Warning, "Error in baseline digi control");
        return -2;
      }
      std::this_thread::sleep_for(1ms);

      // readout
      for (auto d : digis) {
        words_read[d->bid()] = d->Read(buffers[d->bid()]);
      }

      // decode
      if (std::any_of(words_read.begin(), words_read.end(),
            [=](auto p) {return p.second < 0;})) {
        for (auto d : digis) {
          if (words_read[d->bid()] < 0)
            fLog->Entry(MongoLog::Error, "Board %i has readout error in baselines",
                d->bid());
        }
        return -2;
      }
      if (std::any_of(words_read.begin(), words_read.end(), [=](auto p) {
            return (0 <= p.second) && (p.second <= 16);})) { // header-only readouts???
        for (auto& p : words_read) if ((0 <= p.second) && (p.second <= 16))
          fLog->Entry(MongoLog::Local, "Board %i undersized readout (%i)",
              p.first, p.second);
        step--;
        steps_repeated++;
        continue;
      }

      // analyze
      for (auto d : digis) {
        bid = d->bid();
        auto it = buffers[bid]->buff.begin();
        while (it < buffers[bid]->buff.end()) {
          if ((*it)>>28 == 0xA) {
            words = (*it)&0x7FFFFFFF;
            std::u32string_view sv(buffers[bid]->buff.data() + std::distance(buffers[bid]->buff.begin(), it), words);
            std::tie(words_in_event, channel_mask, std::ignore, std::ignore) = d->UnpackEventHeader(sv);
            if (words == 4) {
              it += 4;
              continue;
            }
            if (channel_mask == 0) { // should be impossible?
              it += 4;
              continue;
            }
            channels_in_event = std::bitset<16>(channel_mask).count();
            it += words;
            sv.remove_prefix(4);
            for (unsigned ch = 0; ch < d->GetNumChannels(); ch++) {
              if (!(channel_mask & (1 << ch))) continue;
              std::u32string_view wf;
              std::tie(std::ignore, words, std::ignore, wf) = d->UnpackChannelHeader(sv,
                  0, 0, 0, words, channels_in_event);
              vector<int> hist(0x4000, 0);
              for (auto w : wf) {
                val0 = w&0x3FFF;
                val1 = (w>>16)&0x3FFF;
                if (val0*val1 == 0) continue;
                hist[val0 >> rebin_factor]++;
                hist[val1 >> rebin_factor]++;
              }
              sv.remove_prefix(words);
              auto max_it = std::max_element(hist.begin(), hist.end());
              auto max_start = std::max(max_it - bins_around_max, hist.begin());
              auto max_end = std::min(max_it + bins_around_max+1, hist.end());
              counts_total = std::accumulate(hist.begin(), hist.end(), 0);
              counts_around_max = std::accumulate(max_start, max_end, 0);
              if (counts_around_max < fraction_around_max*counts_total) {
                fLog->Entry(MongoLog::Local,
                    "Bd %i ch %i: %i out of %i counts around max %i",
                    bid, ch, counts_around_max, counts_total,
                    std::distance(hist.begin(), max_it)<<rebin_factor);
                redo_iter = true;
              }
              if (counts_total < 1.5*words) {//25% zeros
                redo_iter = true;
                fLog->Entry(MongoLog::Local, "Bd %i ch %i too many skipped samples", bid, ch);
              }
              vector<int> bin_ids(std::distance(max_start, max_end), 0);
              std::iota(bin_ids.begin(), bin_ids.end(), std::distance(hist.begin(), max_start));
              baseline = 0;
              // calculated weighted average
              baseline = std::inner_product(max_start, max_end, bin_ids.begin(), 0) << rebin_factor;
              baseline /= counts_around_max;
              bl_per_channel[bid][ch][step] = baseline;
            } // for each channel

          } else { // if header
            it++;
          }
        } // end of while in buffer
      } // process per digi
      if (redo_iter) {
        redo_iter = false;
        step--;
        steps_repeated++;
        continue;
      }
      if (step+1 < DAC_cal_points.size()) continue;
      if (step+1 == DAC_cal_points.size() && calibrate) {
        // ****************************
        // Determine calibration values
        // ****************************
        for (auto d : digis) {
          bid = d->bid();
          fMutex.lock();
          cal_values[bid] = std::map<std::string, vector<double>>(
              {{"slope", vector<double>(d->GetNumChannels())},
               {"yint", vector<double>(d->GetNumChannels())}});
          fMutex.unlock();
          for (unsigned ch = 0; ch < d->GetNumChannels(); ch++) {
            B = C = D = E = F = 0;
            for (unsigned i = 0; i < DAC_cal_points.size(); i++) {
              B += DAC_cal_points[i]*DAC_cal_points[i];
              C += 1;
              D += DAC_cal_points[i]*bl_per_channel[bid][ch][i];
              E += bl_per_channel[bid][ch][i];
              F += DAC_cal_points[i];
            }
            cal_values[bid]["slope"][ch] = slope = (C*D - E*F)/(B*C - F*F);
            cal_values[bid]["yint"][ch] = yint = (B*E - D*F)/(B*C - F*F);
            dac_values[bid][ch] = (target_baseline-yint)/slope;
          }
        }
        calibrate = false;
      } else {
        // ******************
        // Do fitting process
        // ******************
        for (auto d : digis) {
          bid = d->bid();
          for (unsigned ch = 0; ch < d->GetNumChannels(); ch++) {
            if (channel_finished[bid][ch] >= convergence_threshold) continue;

            float off_by = target_baseline - bl_per_channel[bid][ch][step];
            if (abs(off_by) < adjustment_threshold) {
              channel_finished[bid][ch]++;
              continue;
            }
            channel_finished[bid][ch] = std::max(0, channel_finished[bid][ch]-1);
            int adjustment = off_by * cal_values[bid]["slope"][ch];
            if (abs(adjustment) < min_adjustment)
              adjustment = std::copysign(min_adjustment, adjustment);
            dac_values[bid][ch] += adjustment;
          } // for channels
        } // for digis
      } // fit/calibrate
      for (auto d : digis)
        d->ClampDACValues(dac_values[d->bid()], cal_values[d->bid()]);

    } // end steps
    if (std::all_of(channel_finished.begin(), channel_finished.end(),
          [&](auto& p){return std::all_of(p.second.begin(), p.second.end(), [=](int i){
            return i >= convergence_threshold;});})) {
      fLog->Entry(MongoLog::Local, "All baselines for boards on this link converged");
      break;
    }
  } // end iterations
  if (fail) return -2;
  if (std::any_of(channel_finished.begin(), channel_finished.end(),
      [&](auto& p){return std::any_of(p.second.begin(), p.second.end(), [=](int i){
        return i < convergence_threshold;});})) {
    fLog->Entry(MongoLog::Warning, "Couldn't baseline properly!");
    return -1;
  }
  for (auto d : digis) {
    for (unsigned ch = 0; ch < d->GetNumChannels(); ch++) {
      bid = d->bid();
      fLog->Entry(MongoLog::Local, "Bd %i ch %i exp %x act %x", bid, ch,
        (target_baseline-cal_values[bid]["yint"][ch])/cal_values[bid]["slope"][ch],
        dac_values[bid][ch]);
    }
  }
  return 0;
}
