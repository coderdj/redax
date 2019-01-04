# DAQ Options Reference

DAQ options are stored in MongoDB documents in the daq.options collection. Colloqially a single set of options may be 
referred to as a 'run mode'. Example run modes would be "background_stable" or "rn220_heveto". This is the mode a shifter 
will choose when starting the DAQ.

This section provides all settings and their function.

## The 'include' field

Sometimes we re-use configuration between many run modes. For example, the electronics are defined once and, provided the 
physical cabling doesn't change, this definition is valid for all run modes for the entire experiment. So it doesn't make 
sense to copy-paste this definition into each and every mode document. This is where 'includes' come in.

Include documents are given detector 'include', which is simply to ensure they don't appear in the mode list for a shifter 
trying to start a run for that detector, since they are by definition incomplete. They can then be included by appending the 
name of the include document to the 'include' field of the parent doc. 

For example the document 'background_stable' might look like:
```python
{
    "name": "background_stable",
    "include": ["tpc_channel_definition", "muon_veto_channel_definition", "neutron_veto_channel_definition",
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
|board |A unique identifier for this board. The best thing is to just use the digitizer serial number. |
|crate |The 'crate' as defined in CAEN lingo. Namely, if multiple boards are connected to one optical link they get crate numbers zero to seven (max) defining their order in the daisy chain. The first board in the daisy chain is zero. The order is defined by the direction of the optical link propagation, which you can deduce by the little lights on the board that light when they receive an input. |
|vme_address |It is planned to support readout via a V2718 crate controller over the VME backplane. In this case board addressing is via VME address only and crate would refer to the location of the crate controller in the daisy chain. This feature is not yet implemented so the option is placeholder (but must be included). |
|link |Defines the optical link index this board is connected to. This is simple in case of one optical link, though like plugging in USB-A there's always a 50-50 chance to guesss it backwards. It becomes a bit more complicated when you include many A3818 on one server. There's a good diagram in CAEN's A3818 documentation. |
|host |This is the DAQ name of the process that should control the board. Remember this process must be on the same physical machine as the board is connected to. Also, multiple processes cannot share one optical link (but one process can control one optical link). |
|type |Either V1724 for digitizers or V2718 for crate controllers. If more board types are supported they will be added. |

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
