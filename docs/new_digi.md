## Contents
* [Intro](index.md) 
* [Pre-install](prerequisites.md) 
* [DB config](databases.md) 
* [Installation](installation.md) 
* [Options reference](daq_options.md) 
* [Example operation](how_to_run.md)
* [Waveform simulator](fax.md)

# Adding support for new digitizer models/firmwares

All the CAEN digitizers have a similar (but not identical) data format, and for the most part also have different clocks for the digitization and timestamping.
Redax can be trivially extended to support other CAEN digitizers, and probably also non-CAEN digitizers (but this will be less trivial), but subclassing the V1724 base class and changing a few small things.
Here we'll look at exactly how this is done, and what steps need to be taken for an arbitrary digitizer.
All the default assumptions are for the V1724 with DPP DAW firmware.

## Two functions, two values

There are two digitizer class functions that redax calls (well, actually four, but two of those just return a member value, while the other two actually do digitizer-specific stuff).
These two functions are UnpackEventHeader() and UnpackChannelHeader(), and they do exactly what they say on the tin.
The two values are fClockCycle, which is the number of nanoseconds per clock cycle (10 for the 100 MHz trigger clock on the V1724), and fSampleWidth, which is the number of nanoseconds per digitization cycle (also 10 for the 100 MHz digitization on the V1724).
These values are the same for the V1724, but different in other digitizers.
Set these values as necessary in your subclass's constructor.
The two functions are a bit more involved, so let's look at them now.

### UnpackEventHeader
UnpackEventHeader takes as argument a view to the data, and returns a tuple containing the total number of words in this event, the channel mask, if the board fail bit is set or not, and the timestamp in the event header.
Let's look at it here:
```c++
std::tuple<int, int, bool, uint32_t> V1724::UnpackEventHeader(std::u32string_view sv) {
  // returns {words this event, channel mask, board fail, header timestamp
  return {sv[0]&0xFFFFFFF, sv[1]&0xFF, sv[1]&0x4000000, sv[3]&0x7FFFFFFF};
}
```
Where do these magic numbers come from? Let's see what's in the CAEN documentation (pages 10 and 11 of the DPP DAW firmware, document UM5954):

<img source="figures/caen_v1724_headers.png" width="600">
<br>
<strong>Figure 1: V1724 DPP DAW header, Fig 2.2 from the CAEN docs</strong>
<br>

The first element of the tuple is the number of words in the event, which is bits[0:27] of the first word.
The second element is the channel mask, which is bits[0:7] of the second word.
Then is the "board fail bit", bit[26] of the second word.
Last is the header timestamp, which is everything except bit[31] of the last word.
If the bitmasks don't make sense, keep trying until it does.

###UnpackChannelHeader
Next, let's look at UnpackChannelHeader.
This function takes rather more arguments (to support the variations between existing digitizers): a string view to the data itself, and an additional 5 integers, which may or may not actually be used.
```c++
std::tuple<int64_t, int, uint16_t, std::u32string_view> V1724::UnpackChannelHeader(std::u32string_view sv, long rollovers, uint32_t header_time, uint32_t, int, int) {
  // returns {timestamp (ns), words this channel, baseline, waveform}
  long ch_time = sv[1] & 0x7FFFFFFF;
  int words = sv[0] & 0x7FFFFF;
  if (ch_time > 15e8 && header_time < 5e8 && rollovers != 0) rollovers--;
  if (ch_time < 5e8 && header_time > 15e8) rollovers++;
  return {((rollovers<<31) + ch_time) * fClockCycle, words, 0, sv.substr(2, words-2)};
}
```
Let's start with the arguments.
  1. The data itself. This points to the first word of a channel's block of data.
  2. The board's rollover counter.
  3. The timestamp of the first event recorded in the readout block that contains this event (this is the timestamp used to track rollovers).
  4. The timestamp from the header of this event.
  5. The number of words in this event.
  6. The number of channels in this event.
Note that the last three aren't used in this function, but they are used for other digitizers, which is why they're here.

Now for the function body.
Refer back to Figure 1: a channel's data starts with two words of control information, in this case, the number of words for this channel's data and its timestamp.
These two values are split first.
Next there's some logic dealing with clock rollovers.
I'm not going into that here, see the XENONnT DAQ Bible for that.
The return tuple contains another four integers.
  1. Timestamp in nanoseconds since the start of the run. This uses the rollover counter (bit-shifted as necessary), the channel's timestamp, and fClockCycle, which is the member variable that says how many nanoseconds wide the clock cycle is. For the V1724, it's 10, because 100 Mhz.
  2. Words in this channel's block of data. Not much to say here.
  3. The baseline. Some digitizers report the baseline in the header. The V1724 doesn't, so it returns 0 here.
  4. The waveform itself. This is the channel's data except for the first two words, which contain control information.

That's it - implement these two functions (as necessary) and redax will know how to understand the data from your digitizer.
Let's look at two examples, the V1730 with DPP DAW firmware, and the V1724 without.

## Case study: V1730

<img src="figures/caen_v1730_headers.png" width="600">
<br>
<strong>Figure 2: V1730 DPP DAW header, Fig 2.3 from the CAEN docs</strong>
<br>

### UnpackEventHeader
```c++
std::tuple<int, int, bool, uint32_t> V1730::UnpackEventHeader(std::u32string_view sv) {
  return {sv[0] & 0xFFFFFFF,
         (sv[1] & 0xFF) | ((sv[2]>>16)&0xFF00),
          sv[1]&0x4000000,
          sv[3]&0x7FFFFFFF};
}
```
The big difference here is the channel mask is split between two words, so a bit of bit manipulation magic is needed to combine it.

### UnpackChannelHeader
```c++
std::tuple<int64_t, int, uint16_t, std::u32string_view> V1730::UnpackChannelHeader(std::u32string_view sv, long, uint32_t, uint32_t, int, int) {
  int words = sv[0]&0x7FFFFF;
  return {(long(sv[1]) | (long(sv[2]&0xFFFF)<<32))*fClockCycle,
          words,
          (sv[2]>>16)&0x3FFF,
          sv.substr(3, words-3)};
}
```
First up, note that the V1730 has no need of anything other than the data itself, the extra integers aren't used.
Note that the 48-bit timestamp is spread over two words, so some bit manipulation is needed to combine it.
Also, we see that there's a baseline, and a total of three control words.

## Case study: V1274 with default firmware

<img source="figures/caen_v1724_default.png" width="600">
<br>
<strong>Figure 3: V1724 headers with default firmware</strong>
<br>

This is the firmware the muon veto uses, which is why "MV" shows up everywhere.

### UnpackEventHeader

It's the same as the DPP DAW firmware, so nothing is necessary.

### UnpackChannelHeader

The keen observer will note that there isn't a channel header like in the previous examples.
The keen observer would be correct.
```c++
std::tuple<int64_t, int, uint16_t, std::u32string_view> V1724_MV::UnpackChannelHeader(std::u32string_view sv, long rollovers, uint32_t header_time, uint32_t channel_time, int event_words, int n_channels) {
  int words = (event_words-4)/n_channels;
  if (event_time > 15e8 && header_time < 5e8 && rollovers != 0) rollovers--;
  else if (event_time < 5e8 && header_time > 15e8) rollovers++;
  return {((rollovers<<31) + event_time) * fClockCycle,
          words,
          0,
          sv.substr(0, words)};
}
```
This should look very similar, except for where the number of words in this channel comes from.
That's the only real change, and which timestamp gets used in the rollover logic.

## Other considerations

There are important other things involved in getting redax to work with your digitizer model.

### Registers

You may need to specify different register addresses for various digitizer (or override the named functions that call them).
The list of registers is in the V1724 constructor.
Refer to this list, the UM5913 document from CAEN, and the equivalent register list for your model, and make sure to override any that don't match.

### Global implementation

Once you've written the header and source files for your digitizer, you need to inform the rest of redax about it.
This requires changes in four locations:
  1. The Makefile, SOURCES_SLAVE definition.
  2. DAQController.cc, in the includes.
  3. DAQController::Arm, around L60.
  4. Options::GetBoards, around L190.
It should be clear from the context what changes are necessary.

### Non-CAEN digitizers

Redax assumes that you're interfacing with digitizers via CAENVMElib.
If you aren't using a CAEN digitizer, this probably won't be the case.
To add support, you would most likely need to override more named functions in the V1724 base class, rather than just change register values.
For instance, functions like SWTrigger and GetAcquisitionStatus are essentially wrappers around reads and writes to specific registers.
These functions can get implemented in your derived class to interface with your digitizer in whatever way makes the most sense.

Further, redax assumes that the data comes off the digitizer as a series of 32-bit words, with an event header, channel headers (maybe), and two samples per word.
If this isn't the case, your mileage will probably vary quite significantly.
One option would be to reformat the data as you read it from the digitizer, which would not work well under high data rates, because you would be doing this processing in the readout thread.
Another option would be to subclass the StraxFormatter, but this would require more structural changes as well, because this option wasn't strictly foreseen in the design.
Or, you just make whatever changes are necessary and don't bother ever pulling from master again, but this can very easily become untenable.
