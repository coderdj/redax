#include "DAQController.hh"
#include <functional>
#include "V1724.hh"
#include "V1724_MV.hh"
//#include "V1730.hh"
#include "DAXHelpers.hh"
#include "Options.hh"
#include "StraxInserter.hh"
#include "MongoLog.hh"
#include <unistd.h>
#include <algorithm>
#include <bitset>
#include <chrono>
#include <cmath>
#include <numeric>

// Status:
// 0-idle
// 1-arming
// 2-armed
// 3-running
// 4-error

DAQController::DAQController(MongoLog *log, std::string hostname){
  fLog=log;
  fOptions = NULL;
  fStatus = DAXHelpers::Idle;
  fReadLoop = false;
  fNProcessingThreads=8;
  fBufferLength = 0;
  fRawDataBuffer = NULL;
  fDatasize=0.;
  fHostname = hostname;
}

DAQController::~DAQController(){
  if(fProcessingThreads.size()!=0)
    CloseProcessingThreads();
}

std::string DAQController::run_mode(){
  if(fOptions == NULL)
    return "None";
  try{
    return fOptions->GetString("name");
  }
  catch(const std::exception &e){
    return "None";
  }
}

int DAQController::InitializeElectronics(Options *options, std::vector<int>&keys){

  End();
  
  fOptions = options;
  fNProcessingThreads = fOptions->GetNestedInt("processing_threads."+fHostname, 8);  
  fLog->Entry(MongoLog::Local, "Beginning electronics initialization with %i threads",
	      fNProcessingThreads);

  // Initialize digitizers
  fStatus = DAXHelpers::Arming;
  std::vector<int> BIDs;
  for(auto d : fOptions->GetBoards("V17XX", fHostname)){
    fLog->Entry(MongoLog::Local, "Arming new digitizer %i", d.board);

    V1724 *digi;
    if(d.type == "V1724_MV")
      digi = new V1724_MV(fLog, fOptions);
    // else if(d.type == "V1730")
    // digi = new V1730(fLog, fOptions);
    else
      digi = new V1724(fLog, fOptions);

    
    if(digi->Init(d.link, d.crate, d.board, d.vme_address)==0){
	fDigitizers[d.link].push_back(digi);
	fDataPerDigi[digi->bid()] = 0;
        BIDs.push_back(digi->bid());

	if(std::find(keys.begin(), keys.end(), d.link) == keys.end()){
	  fLog->Entry(MongoLog::Local, "Defining a new optical link at %i", d.link);
	  keys.push_back(d.link);
	}    
	fLog->Entry(MongoLog::Debug, "Initialized digitizer %i", d.board);
	
	int write_success = 0;
	write_success += digi->WriteRegister(0xEF24, 0x1);
	write_success += digi->WriteRegister(0xEF00, 0x30);
	if(write_success!=0){
	  fLog->Entry(MongoLog::Error,
		      "Digitizer %i unable to load pre-registers",
		      digi->bid());
	  fStatus = DAXHelpers::Idle;
	  return -1;
	}
    }
    else{
      fLog->Entry(MongoLog::Warning, "Failed to initialize digitizer %i", d.board);
      fStatus = DAXHelpers::Idle;
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
  if (fOptions->GetString("baseline_dac_mode") == "cached") fOptions->GetDAC(dac_values, BIDs);
  std::vector<std::thread*> init_threads;

  init_threads.reserve(fDigitizers.size());
  std::vector<int> rets;
  rets.reserve(fDigitizers.size());
  // Parallel digitizer programming to speed baselining
  for( auto& link : fDigitizers ) {
    rets.push_back(1);
    init_threads.push_back(new std::thread(&DAQController::InitLink, this,
	  std::ref(link.second), std::ref(dac_values), std::ref(rets.back())));
  }
  for (unsigned i = 0; i < init_threads.size(); i++) {
    init_threads[i]->join();
    delete init_threads[i];
  }
  if (std::any_of(rets.begin(), rets.end(), [](int i) {return i != 0;})) {
    fLog->Entry(MongoLog::Warning, "Encountered errors during digitizer programming");
    if (std::any_of(rets.begin(), rets.end(), [](int i) {return i == -2;}))
      fStatus = DAXHelpers::Error;
    else
      fStatus = DAXHelpers::Idle;
    return -1;
  } else
    fLog->Entry(MongoLog::Debug, "Digitizer programming successful");
  if (fOptions->GetString("baseline_dac_mode") == "fit") fOptions->UpdateDAC(dac_values);

  for(auto const& link : fDigitizers ) {
    for(auto digi : link.second){
      if(fOptions->GetInt("run_start", 0) == 1)
	digi->SINStart();
      else
	digi->AcquisitionStop();
    }
  }
  sleep(1);
  fStatus = DAXHelpers::Armed;

  fLog->Entry(MongoLog::Local, "Arm command finished, returning to main loop");


  return 0;
}

int DAQController::Start(){
  if(fOptions->GetInt("run_start", 0) == 0){
    for( auto const& link : fDigitizers ){
      for(auto digi : link.second){

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

  std::cout<<"Deactivating boards"<<std::endl;
  for( auto const& link : fDigitizers ){
    for(auto digi : link.second){
      digi->AcquisitionStop();

      // Ensure digitizer is stopped
      if(digi->EnsureStopped(1000, 1000) != true){
	//if(digi->MonitorRegister(0x8104, 0x4, 1000, 1000, 0x0) != true){
	fLog->Entry(MongoLog::Warning,
		    "Timed out waiting for acquisition to stop after SW stop sent");
          return -1;
      }
    }
  }
  fLog->Entry(MongoLog::Debug, "Stopped digitizers");

  fReadLoop = false; // at some point.
  fStatus = DAXHelpers::Idle;
  return 0;
}
void DAQController::End(){
  Stop();
  fLog->Entry(MongoLog::Local, "Closing Processing Threads");
  CloseProcessingThreads();
  fLog->Entry(MongoLog::Local, "Closing Digitizers");
  for( auto const& link : fDigitizers ){    
    for(auto digi : link.second){
      digi->End();
      delete digi;
    }
  } 
  fDigitizers.clear();
  fStatus = DAXHelpers::Idle;

  if(fRawDataBuffer != NULL){
    fLog->Entry(MongoLog::Warning, "Deleting uncleard buffer of size %i",
		fRawDataBuffer->size());
    for(unsigned int i=0; i<fRawDataBuffer->size(); i++){
      delete[] (*fRawDataBuffer)[i].buff;
      (*fRawDataBuffer)[i].buff = NULL;
    }
    delete fRawDataBuffer;
    fRawDataBuffer = NULL;
  }

  std::cout<<"Finished end"<<std::endl;
}

void DAQController::ReadData(int link){
  fReadLoop = true;
  
  // Raw data buffer should be NULL. If not then maybe it was not cleared since last time
  fBufferMutex.lock();
  if(fRawDataBuffer != NULL){
    fLog->Entry(MongoLog::Debug, "Raw data buffer being brute force cleared.");
    for(unsigned int x=0;x<fRawDataBuffer->size(); x++){
      delete[] (*fRawDataBuffer)[x].buff;
      (*fRawDataBuffer)[x].buff = NULL;
    }
    delete fRawDataBuffer;
    fBufferLength=0;
    fRawDataBuffer = NULL;
  }
  fBufferMutex.unlock();
  
  u_int32_t lastRead = 0; // bytes read in last cycle. make sure we clear digitizers at run stop
  long int readcycler = 0;
  while(fReadLoop){
    
    std::vector<data_packet> local_buffer;
    for(unsigned int x=0; x<fDigitizers[link].size(); x++){

      // Every 1k reads check board status
      if(readcycler%10000==0){
	readcycler=0;
	u_int32_t data = fDigitizers[link][x]->GetAcquisitionStatus();
        fLog->Entry(MongoLog::Local, "Board %i has status 0x%04x",
            fDigitizers[link][x]->bid(), data);
      }
      data_packet d;
      d.buff=NULL;
      d.size=0;
      d.bid = fDigitizers[link][x]->bid();
      d.size = fDigitizers[link][x]->ReadMBLT(d.buff);

      lastRead += d.size;
      
      if(d.size<0){
	//LOG ERROR
	if(d.buff!=NULL){
	  delete[] d.buff;
          d.buff = NULL;
        }
	break;
      }
      if(d.size>0){
	d.header_time = fDigitizers[link][x]->GetHeaderTime(d.buff, d.size);
	d.clock_counter = fDigitizers[link][x]->GetClockCounter(d.header_time);
	fDatasize += d.size;
	fDataPerDigi[d.bid] += d.size;
	local_buffer.push_back(d);
      }
    }
    if(local_buffer.size()!=0)
      AppendData(local_buffer);
    local_buffer.clear();
    readcycler++;
    usleep(1);
  }

}


std::map<int, u_int64_t> DAQController::GetDataPerDigi(){
  // Return a map of data transferred per digitizer since last update
  // and clear the private map
  std::map <int, u_int64_t>retmap;
  for(auto const &kPair : fDataPerDigi){
    retmap[kPair.first] = (u_int64_t)(fDataPerDigi[kPair.first]);
    fDataPerDigi[kPair.first] = 0;
  }
  return retmap;
}

long DAQController::GetStraxBufferSize() {
  return std::accumulate(fProcessingThreads.begin(), fProcessingThreads.end(), 0,
      [=](long tot, processingThread pt) {return tot + pt.inserter->GetBufferSize();});
}

std::map<std::string, int> DAQController::GetDataFormat(){
  for( auto const& link : fDigitizers )    
    for(auto digi : link.second)
      return digi->DataFormatDefinition;
  return std::map<std::string, int>();
}

void DAQController::AppendData(std::vector<data_packet> &d){
  // Blocks!
  fBufferMutex.lock();
  if(fRawDataBuffer==NULL)
    fRawDataBuffer = new std::vector<data_packet>();
  fRawDataBuffer->insert( fRawDataBuffer->end(), d.begin(), d.end() );
  u_int64_t bl = 0;
  for(unsigned int x=0; x<fRawDataBuffer->size(); x++){
    bl += (*fRawDataBuffer)[x].size;
  }
  fBufferLength = bl; 
  fBufferMutex.unlock();  
}

int DAQController::GetData(std::vector <data_packet> *&retVec){
  // Check once, is it worth locking mutex?
  retVec=NULL;
  if(fBufferLength==0)
    return 0;
  if(!fBufferMutex.try_lock())
    return 0;

  int ret = 0;
  // Check again, is there still data?
  if(fRawDataBuffer != NULL && fRawDataBuffer->size()>0){

    // Pass ownership to calling function
    retVec = fRawDataBuffer;
    fRawDataBuffer = NULL;

    ret = retVec->size();
    fBufferLength = 0;
  }
  fBufferMutex.unlock();
  return ret;
}

bool DAQController::CheckErrors(){

  // This checks for errors from the threads by checking the
  // error flag in each object. It's appropriate to poll this
  // on the order of ~second(s) and initialize a STOP in case
  // the function returns "true"

  for(unsigned int i=0; i<fProcessingThreads.size(); i++){
    if(fProcessingThreads[i].inserter->CheckError()){
      fLog->Entry(MongoLog::Error, "Error found in processing thread.");
      fStatus=DAXHelpers::Error;
      return true;
    }
  }
  return false;
}

int DAQController::OpenProcessingThreads(){
  int ret = 0;
  for(int i=0; i<fNProcessingThreads; i++){
    processingThread p;
    p.inserter = new StraxInserter();
    if (p.inserter->Initialize(fOptions, fLog, this, fHostname)) {
      p.pthread = new std::thread(); // something to delete later
      ret++;
    } else
      p.pthread = new std::thread(&StraxInserter::ReadAndInsertData, p.inserter);
    fProcessingThreads.push_back(p);
  }
  return ret;
}

void DAQController::CloseProcessingThreads(){
  std::map<int,int> board_fails;

  for(unsigned int i=0; i<fProcessingThreads.size(); i++){
    fProcessingThreads[i].inserter->Close(board_fails);
    fProcessingThreads[i].pthread->join();

    delete fProcessingThreads[i].pthread;
    delete fProcessingThreads[i].inserter;
  }
  fProcessingThreads.clear();
  if (std::accumulate(board_fails.begin(), board_fails.end(), 0,
	[=](int tot, std::pair<int,int> iter) {return tot + iter.second;})) {
    std::stringstream msg;
    msg << "Found board failures: ";
    for (auto& iter : board_fails) msg << iter.first << ":" << iter.second << " | ";
    fLog->Entry(MongoLog::Warning, msg.str());
  }
}

void DAQController::InitLink(std::vector<V1724*>& digis,
    std::map<int, std::map<std::string, std::vector<double>>>& cal_values, int& ret) {
  std::string BL_MODE = fOptions->GetString("baseline_dac_mode", "fixed");
  std::map<int, std::vector<u_int16_t>> dac_values;
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
    fMapMutex.lock();
    auto board_dac_cal = cal_values.count(bid) ? cal_values[bid] : cal_values[-1];
    fMapMutex.unlock();
    if(BL_MODE == "cached") {
      dac_values[bid] = std::vector<u_int16_t>(digi->GetNumChannels());
      fLog->Entry(MongoLog::Local, "Board %i using cached baselines", bid);
      for (unsigned ch = 0; ch < digi->GetNumChannels(); ch++)
	dac_values[bid][ch] = nominal_baseline*board_dac_cal["slope"][ch] + board_dac_cal["yint"][ch];
      digi->ClampDACValues(dac_values[bid], board_dac_cal);
    }
    else if(BL_MODE != "fixed" && BL_MODE != "fit"){
      fLog->Entry(MongoLog::Warning, "Received unknown baseline mode '%s', fallback to fixed", BL_MODE.c_str());
      BL_MODE = "fixed";
    }
    if(BL_MODE == "fixed"){
      int BLVal = fOptions->GetInt("baseline_fixed_value", 4000);
      fLog->Entry(MongoLog::Local, "Loading fixed baselines with value 0x%04x", BLVal);
      dac_values[bid] = std::vector<u_int16_t>(digi->GetNumChannels(), BLVal);
    }

    //int success = 0;
    fLog->Entry(MongoLog::Local, "Board %i finished baselines", bid);
    if(success==-2){
      fLog->Entry(MongoLog::Warning, "Board %i Baselines failed with digi error");
      ret = -2;
      return;
    }
    else if(success!=0){
      fLog->Entry(MongoLog::Warning, "Board %i failed baselines with timeout", digi->bid());
      ret = -1;
      return;
    }

    fLog->Entry(MongoLog::Local, "Board %i survived baseline mode. Going into register setting",
		bid);

    for(auto regi : fOptions->GetRegisters(bid)){
      unsigned int reg = DAXHelpers::StringToHex(regi.reg);
      unsigned int val = DAXHelpers::StringToHex(regi.val);
      success+=digi->WriteRegister(reg, val);
    }
    fLog->Entry(MongoLog::Local, "Board %i loaded user registers, loading DAC.", bid);

    // Load the baselines you just configured
    success += digi->LoadDAC(dac_values[bid]);

    fLog->Entry(MongoLog::Local,
	"DAC finished for %i. Assuming not directly followed by an error, that's a wrap.",
          digi->bid());
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

int DAQController::FitBaselines(std::vector<V1724*> &digis,
    std::map<int, std::vector<u_int16_t>> &dac_values, int target_baseline,
    std::map<int, std::map<std::string, std::vector<double>>> &cal_values) {
  using std::vector;
  using namespace std::chrono_literals;
  int max_iter(2);
  unsigned max_steps(20), digis_this_link(digis.size()), ch_per_digi(digis[0]->GetNumChannels());
  int adjustment_threshold(10), convergence_threshold(3), min_adjustment(0xA);
  int rebin_factor(1); // log base 2
  int nbins(1 << (14-rebin_factor)), bins_around_max(3);
  int triggers_per_step = 3, steps_repeated(0), max_repeated_steps(10);
  std::chrono::milliseconds ms_between_triggers(10);
  vector<int> hist(nbins);
  vector<long> DAC_cal_points = {60000, 30000, 6000}; // arithmetic overflow
  vector<vector<int>> channel_finished(digis_this_link, vector<int>(ch_per_digi));
  vector<u_int32_t*> buffers(digis_this_link);
  vector<int> bytes_read(digis_this_link), ret_vect(digis_this_link);
  vector<vector<vector<double>>> bl_per_channel(digis_this_link,
         vector<vector<double>>(ch_per_digi, vector<double>(max_steps)));
  vector<vector<int>> diff(digis_this_link, vector<int>(ch_per_digi));
  bool done(false), redo_iter(false), fail(false), calibrate(true);
  int bid(0);
  double counts_total(0), counts_around_max(0), B,C,D,E,F, slope, yint, baseline;
  double fraction_around_max(0.8);
  u_int32_t words_in_event, channel_mask, words_per_channel;
  u_int16_t val0, val1;
  int channels_in_event, idx;
  auto beg_it = hist.begin(), max_it = hist.begin(), end_it = hist.end();
  auto max_start = max_it, max_end = max_it;

  for (auto digi : digis) {
    dac_values[digi->bid()] = vector<u_int16_t>(ch_per_digi);
  }

  for (int iter = 0; iter < max_iter; iter++) {
    if (done || fail) break;
    for (auto& vv : bl_per_channel) // vv = vector<vector<double>>
      for (auto& v : vv) // v = vector<double>
        v.assign(v.size(), 0);
    for (auto& v : channel_finished) v.assign(v.size(), 0);
    steps_repeated = 0;
    fLog->Entry(MongoLog::Local, "Beginning baseline iteration %i/%i", iter, max_iter);

    for (unsigned step = 0; step < max_steps; step++) {
      fLog->Entry(MongoLog::Local, "Beginning baseline step %i/%i", step, max_steps);
      if (std::all_of(channel_finished.begin(), channel_finished.end(),
            [&](vector<int>& v) {
              return std::all_of(v.begin(), v.end(), [=](int i)
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
          dac_values[d->bid()].assign(ch_per_digi, (int)DAC_cal_points[step]);
      }
      for (auto d : digis) {
        if (d->LoadDAC(dac_values[d->bid()])) {
          fLog->Entry(MongoLog::Warning, "Board %i failed to load DAC", d->bid());
          return -2;
        }
      }
      // "After writing, the user is recommended to wait for a few seconds before
      // a new RUN to let the DAC output get stabalized" - CAEN documentation
      std::this_thread::sleep_for(1s);
      // sleep(2) seems unnecessary after preliminary testing

      // start board
      for (auto digi : digis) {
        if (digi->EnsureReady(1000,1000))
          digi->SoftwareStart();
        else
          fail = true;
      }
      std::this_thread::sleep_for(5ms);
      for (auto digi : digis) {
        if (!digi->EnsureStarted(1000,1000)) {
          digi->AcquisitionStop();
          fail = true;
        }
      }

      // send triggers
      for (int trig = 0; trig < triggers_per_step; trig++) {
        for (auto digi : digis) digi->SWTrigger();
        std::this_thread::sleep_for(ms_between_triggers);
      }
      // stop
      for (auto digi : digis) {
        digi->AcquisitionStop();
        if (!digi->EnsureStopped(1000,1000)) {
          fail = true;
        }
      }
      if (fail) {
        for (auto digi : digis) digi->AcquisitionStop();
        fLog->Entry(MongoLog::Warning, "Error in baseline digi control");
        return -2;
      }
      std::this_thread::sleep_for(1ms);

      // readout
      for (unsigned d = 0; d < digis.size(); d++)
        bytes_read[d] = digis[d]->ReadMBLT(buffers[d]);

      // decode
      if (std::any_of(bytes_read.begin(), bytes_read.end(), [=](int i) {return i < 0;})) {
        for (unsigned d = 0; d < digis.size(); d++) {
          if (bytes_read[d] < 0)
            fLog->Entry(MongoLog::Error, "Board %i has readout error in baselines",
                digis[d]->bid());
          return -2;
        }
      }
      if (std::any_of(bytes_read.begin(), bytes_read.end(), [=](int b) {
            return (0 <= b) && (b <= 16);})) { // header-only readouts???
        fLog->Entry(MongoLog::Local, "Undersized readout");
        step--;
        steps_repeated++;
        for (auto& b : buffers) delete[] b;
        continue;
      }

      // analyze
      for (unsigned d = 0; d < digis.size(); d++) {
        idx = 0;
        while ((idx * sizeof(u_int32_t) < bytes_read[d]) && (idx >= 0)) {
          if ((buffers[d][idx]>>28) == 0xA) {
            words_in_event = buffers[d][idx]&0xFFFFFFF;
            if (words_in_event == 4) {
              idx += 4;
              continue;
            }
            channel_mask = buffers[d][idx+1]&0xFF;
            if (digis[d]->DataFormatDefinition["channel_mask_msb_idx"] != -1) {
              // V1730 stuff here
            }
            if (channel_mask == 0) { // should be impossible?
              idx += 4;
              continue;
            }
            channels_in_event = std::bitset<16>(channel_mask).count();
            words_per_channel = (words_in_event - 4)/channels_in_event;
            words_per_channel -= digis[d]->DataFormatDefinition["channel_header_words"];

            idx += 4;
            for (unsigned ch = 0; ch < ch_per_digi; ch++) {
              if (!(channel_mask & (1 << ch))) continue;
              idx += digis[d]->DataFormatDefinition["channel_header_words"];
              hist.assign(hist.size(), 0);
              for (unsigned w = 0; w < words_per_channel; w++) {
                val0 = buffers[d][idx+w]&0xFFFF;
                val1 = (buffers[d][idx+w]>>16)&0xFFFF;
                if (val0*val1 == 0) continue;
                hist[val0 >> rebin_factor]++;
                hist[val1 >> rebin_factor]++;
              }
              idx += words_per_channel;
              for (auto it = beg_it; it < end_it; it++) if (*it > *max_it) max_it = it;
              max_start = std::max(max_it - bins_around_max, beg_it);
              max_end = std::min(max_it + bins_around_max+1, end_it);
              counts_total = std::accumulate(beg_it, end_it, 0.);
              counts_around_max = std::accumulate(max_start, max_end, 0.);
              if (counts_around_max/counts_total < fraction_around_max) {
                fLog->Entry(MongoLog::Local,
                    "Bd %i ch %i: %d out of %d counts around max %i",
                    digis[d]->bid(), ch, counts_around_max, counts_total,
                    (max_it - beg_it)<<rebin_factor);
                redo_iter = true;
              }
              if (counts_total/words_per_channel < 1.5) //25% zeros
                redo_iter = true;
              baseline = 0;
              // calculated weighted average
              for (auto it = max_start; it < max_end; it++)
                baseline += ((it - beg_it)<<rebin_factor)*(*it);
              baseline /= counts_around_max;
              bl_per_channel[d][ch][step] = baseline;
            } // for each channel

          } else { // if header
            idx++;
          }
        } // end of while in buffer
      } // process per digi
      // cleanup buffers
      for (auto& b : buffers) delete[] b;
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
        for (unsigned d = 0; d < digis_this_link; d++) {
          bid = digis[d]->bid();
          fMapMutex.lock();
          cal_values[bid] = std::map<std::string, vector<double>>(
              {{"slope", vector<double>(ch_per_digi)},
               {"yint", vector<double>(ch_per_digi)}});
          fMapMutex.unlock();
          for (unsigned ch = 0; ch < ch_per_digi; ch++) {
            B = C = D = E = F = 0;
            for (unsigned i = 0; i < DAC_cal_points.size(); i++) {
              B += DAC_cal_points[i]*DAC_cal_points[i];
              C += 1;
              D += DAC_cal_points[i]*bl_per_channel[d][ch][i];
              E += bl_per_channel[d][ch][i];
              F += DAC_cal_points[i];
            }
            cal_values[bid]["slope"][ch] = slope = (C*D - E*F)/(B*C - F*F);
            cal_values[bid]["yint"][ch] = yint = (B*E - D*F)/(B*C - F*F);
            fLog->Entry(MongoLog::Debug, "Bd %i ch %i calibration %.3f/%.1f",
                bid, ch, slope, yint);
	    dac_values[bid][ch] = (target_baseline-yint)/slope;
          }
        }
        calibrate = false;
      } else {
        // ******************
        // Do fitting process
        // ******************
        for (unsigned d = 0; d < digis_this_link; d++) {
          bid = digis[d]->bid();
          for (unsigned ch = 0; ch < ch_per_digi; ch++) {
            if (channel_finished[d][ch] >= convergence_threshold) continue;

            float off_by = target_baseline - bl_per_channel[d][ch][step];
            if (abs(off_by) < adjustment_threshold) {
              channel_finished[d][ch]++;
              continue;
            }
	    channel_finished[d][ch] = std::max(0, channel_finished[d][ch]-1);
            int adjustment = off_by * cal_values[bid]["slope"][ch];
            if (abs(adjustment) < min_adjustment)
              adjustment = std::copysign(min_adjustment, adjustment);
            fLog->Entry(MongoLog::Local,
                "Bd %i ch %i dac %04x bl %.1f adjust %i step %i", bid, ch,
                dac_values[bid][ch], bl_per_channel[d][ch][step], adjustment, step);
            dac_values[bid][ch] += adjustment;
          } // for channels
        } // for digis
      } // fit/calibrate
      for (auto d : digis)
        d->ClampDACValues(dac_values[d->bid()], cal_values[d->bid()]);

    } // end steps
    if (std::all_of(channel_finished.begin(), channel_finished.end(),
          [&](vector<int>& v){return std::all_of(v.begin(), v.end(), [=](int i){
            return i >= convergence_threshold;});})) {
      fLog->Entry(MongoLog::Local, "All baselines for boards on this link converged");
      break;
    }
  } // end iterations
  for (unsigned d = 0; d < digis_this_link; d++) {
    for (unsigned ch = 0; ch < ch_per_digi; ch++) {
      fLog->Entry(MongoLog::Local, "Bd %i ch %i diff %i", digis[d]->bid(), ch,
	(target_baseline-cal_values[digis[d]->bid()]["yint"][ch])/cal_values[digis[d]->bid()]["slope"][ch] - dac_values[digis[d]->bid()][ch]);
    }
  }
  if (fail) return -2;
  if (std::any_of(channel_finished.begin(), channel_finished.end(),
    	[&](vector<int>& v){return std::any_of(v.begin(), v.end(), [=](int i){
      	  return i < convergence_threshold;});})) return -1;
  return 0;
}
