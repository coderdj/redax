## Contents
* [Intro](index.md) 
* [Pre-install](prerequisites.md) 
* [DB config](databases.md) 
* [Installation](installation.md) 
* [Options reference](daq_options.md) 
* [Example operation](how_to_run.md)

# DAQ Options Reference

DAQ options are stored in MongoDB documents in the daq.options collection. Colloqially a single set of options may be 
referred to as a 'run mode'. Example run modes would be "background_stable" or "rn220_heveto". This is the mode a shifter 
will choose when starting the DAQ.

This section provides all settings and their function.

## Organizational Fields

The following fields are required for organizational purposes:

|Field |Description |
| ---- | ----- |
| name | The name of this mode. Must be unique. The web interface will ensure that this is unique but if you're not using nodiaq you have to ensure this yourself. |
| user | The user who created the mode. |
| description | A human readable description of the mode (like a sentence) to be displayed on the frontend |
| detector | Which detector does this mode apply to. For XENONnT we use 'tpc', 'muon_veto', 'neutron_veto', or for modes that are meant to be included by top-level modes, 'include'|

Note that only 'name' is actually used by redax. The other options are used by the web frontend and only included here for completion.

## The 'includes' field

Sometimes we re-use configuration between many run modes. For example, the electronics are defined once and, provided the 
physical cabling doesn't change, this definition is valid for all run modes for the entire experiment. So it doesn't make 
sense to copy-paste this definition into each and every mode document. This is where 'includes' come in.

Include documents are given detector 'include', which is simply to ensure they don't appear in the mode list for a shifter 
trying to start a run for that detector, since they are by definition incomplete. They can then be included by appending the 
name of the include document to the 'includes' field of the parent doc. 

For example the document 'background_stable' might look like:
```python
{
    "name": "background_stable",
    "includes": ["tpc_channel_definition", "muon_veto_channel_definition", "neutron_veto_channel_definition",
                "default_registers_tpc", "default_registers_muon_veto", "default_registers_neutron_veto", 
                "master_channel_map", "strax_output_ceph"],
    # ALL OTHER OPTIONS
    #
    # ...
    # 
    #
}
```

This will then include within this run mode all options found in each options document named in the 'include' array. In case 
of repeated options, the highest level document should override the others. However *it is highly recommended to not override 
options declared in included documents!* This may lead to undefined behavior, especially in case of complex, nested 
sub-objects where only certain fields get overridden.

## Electronics Definitions

Electronics are defined in the 'boards' field as follows:

```python
"boards": [
    {
      "board": 100,
      "crate": 0,
      "vme_address": "0",
      "type": "V1724",
      "link": 0,
      "host": "daq0_reader_0"
    },
    {
      "board": 101,
      "crate": 0,
      "vme_address": "0",
      "type": "V1724",
      "link": 0,
      "host": "daq1_reader_0"
    },
    {
      "board": 102,
      "crate": 0,
      "vme_address": "0",
      "type": "V1724",
      "link": 2,
      "host": "daq1_reader_1"
    },
    {
      "vme_address": "0",
      "board": 1000,
      "crate": 0,
      "link": 1,
      "host": "daq0_controller_0",
      "type": "V2718"
    }
  ]
```

This is an example that might be used for reading out 3 boards and defining one crate controller in the electronics 
setup given in the [previous chapter](installation.md). Each subdocument contains the following:

|Option |Description |
| ----- | ---------  |
|board |A unique identifier for this board. The best thing is to just use the digitizer serial number. |
|crate |The 'crate' as defined in CAEN lingo. Namely, if multiple boards are connected to one optical link they get crate numbers zero to seven (max) defining their order in the daisy chain. The first board in the daisy chain is zero. The order is defined by the direction of the optical link propagation, which you can deduce by the little lights on the board that light when they receive an input. |
|vme_address |It is planned to support readout via a V2718 crate controller over the VME backplane. In this case board addressing is via VME address only and crate would refer to the location of the crate controller in the daisy chain. This feature is not yet implemented so the option is placeholder (but must be included). |
|link |Defines the optical link index this board is connected to. This is simple in case of one optical link, though like plugging in USB-A there's always a 50-50 chance to guesss it backwards. It becomes a bit more complicated when you include multiple A3818s on one server. There's a good diagram in CAEN's A3818 documentation. |
|host |This is the DAQ name of the process that should control the board. Remember this process must be on the same physical machine as the board is connected to. Also, multiple processes cannot share one optical link (but one process can control one optical link). |
|type |Either V1724, V1724_MV, or V1730 for digitizers or V2718 for crate controllers. If more board types are supported they will be added. FPGA support (V1495) is pending. |

## Register Definitions

Redax provides direct control of CAEN registers and does not wrap or otherwise simplify this functionality. Registers are loaded into digitizers in the order they are provided here.

Here is an example:
```python
"registers": [
    {
      "val": "1",
      "reg": "EF24",
      "board": -1
    },
    {
      "val": "1",
      "reg": "EF1C",
      "board": -1
    },
    ...
]
```

"val" and "reg" are strings designating **hex values**, where 'reg' is the register to write and 'val' is the value to 
write to it. Refer to the CAEN documentation to know which registers you want to set. The field 'board' can include the 
integral board identifier (defined in the board definition section) in case you want to write this register to a single 
board only. If set to -1 it will be written to all digitizers. 

## V2718

The V2718 crate controller has a few options to configure:
```python
"V2718": {
    "pulser_freq": 0,
    "neutron_veto": 0,
    "muon_veto": 0,
    "led_trigger": 0,
    "s_in": 1
  }
```

| Field | Description |
| ------ | ----------- |
| pulser_freq | The frequency to pulse the trigger/LED pulser in Hz. Supports from 1 Hz up to several kHz. Keep in mind this may not be implemented exactly since the CAEN API doesn't support every possible frequency exactly, but the software will attempt to match the desired frequency as closely as possible. |
| neutron_veto | Should the S-IN signal be propogated to the neutron veto? 1-yes, 0-no |
| muon_veto | Should the S-IN signal be propogated to the muon veto? 1-yes, 0-no |
| led_trigger | Should the LED pulse be propagated to the LED driver? 1-yes, 0-no |
| s_in | Should the run be started with S-IN? 1-yes, 0-no |

The top-level field 'run_start' (next section) is also required to define run start via S-IN.

## Software Configuration Options

Various options that tell redax how to run.

```python
{
    "run_start": 1,
    "baseline_dac_mode": "cached",
    "baseline_reference_run": 1976,
    "baseline_value": 16000,
    "baseline_fixed_value": 4000,
    "processing_threads": {
      "reader0_reader_0": 2,
      "reader1_reader_1": 6,
     }
}
```

|Option | Description |
| -------- | ---------- |
| run_start | Tells the DAQ whether to start the run via register or S-in. 0 for register, 1 for S-in. Note that starting by register means that the digitizer clocks will not be synchronized. This can be fine if you run with an external trigger and use the trigger time as synchronization signal. If running in triggerless mode you need to run with '1' and have your hardware set up accordingly. |
| baseline_dac_mode | cached/fixed/fit. This defines how the DAC-offset values per channel are set. If set to cached the program will load cached baselines from the run specified in *baseline_reference_run*. If it can't find that run it will fall back to the value in *baseline_fixed_value*. If set to 'fixed' it will use *baseline_fixed_value* in any case. If set to 'fit' it will attempt to adjust the DAC offset values until the baseline for each channel matches the value in *baseline_value*. If using negative voltage signals the default value of 16000 is a good one. Baselines for each run are cached in the *dac_values* collection of the daq database. |
|baseline_reference_run | If 'baseline_dac_mode' is set to 'cached' it will use the values from the run number defined here. |
|baseline_value | If 'baseline_dac_mode' is set to 'fit' it will attempt to adjust the baselines until they hit the decimal value defined here, which must lie between 0 and 16386 for a 14-bit ADC. |
|baseline_fixed_value |Use this to set the DAC offset register directly with this value. See CAEN documentation for more details. |
|processing_threads |The number of threads per link working on converting data between CAEN and strax format. Should be larger for processes responsible for more channels and can be smaller for processes only reading a few channels. |

## Strax Output Options

There are various configuration options for the strax output that must be set. 

```python
{
  "strax_chunk_overlap": 0.5,
  "strax_output_path": "/data/xenon/raw/xenonnt",
  "strax_chunk_length": 5,
  "strax_fragment_length": 220
}
```

|Option | Description |
| ---- | ---- | 
| strax_chunk_overlap | Defines the overlap period between strax chunks in seconds. Make is at least some few times larger than your typical event length. In any case it should be larger than your largest expected event. |
| strax_chunk_length | Length of each strax chunk in seconds. There's some balance required here. It should be short enough that strax can process reasonably online, as it waits for each chunk to finish then loads it at once (the size should be digestable). But it shouldn't be so short that it needlessly micro-segments the data. Order of 5-15 seconds seems reasonable at the time of writing. |
|strax_fragment_length | How long are the fragments? In general this should be long enough that it definitely covers the vast majority of your SPE pulses. Our SPE pulses are ~100 samples, so the default value of 220 bytes (2 bytes per sample) provides a small amount of overhead. |
|strax_output_path | Where should we write data? This must be a locally mounted data store. Redax will handle sub-directories so just provide the top-level directory where all the live data should go. |

## Channel Map

Redax needs to provide the channel values to strax. Therefore the channel map (mapping module/channel in the digitizers to PMT position) must be provided at the readout stage. 

This is in a quite simple format: 

```python
"channels": {
    "110": [
      78,
      184,
      219,
      227,
      235,
      205,
      243,
      209
    ],
    "117": [
      174,
      173,
      182,
      98,
      187,
      97,
      113,
      123
    ]
    ...
}
```

Here 'channels' is a dictionary where each key is the string value of a board's ID number as defined in the 'boards' field (typically the serial number). Each value is an array of 8 values (or 16, if appropriate) giving the PMT position of each channel of that digitizer. This PMT position absolutely must match the PMT map assumed by the data processor.

Note that if there are any skipped channels (for instance, if you are using input channels 0, 1, and 3, but not 2), a "blank" or placeholder value should be inserted.

## Trigger thresholds

Redax assigns trigger thresholds using a syntax identical to that of the channel map (above).
