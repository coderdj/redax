# REDAX
D. Coderre, 2018. Please see license in LICENSE file.

See documentation here: [link](https://coderdj.github.io/redax)
## Prerequisites

* mongodb cxx driver
* CAENVMElib v2.5+
* libblosc-dev (on ubuntu)
* C++17-compatible compiler. Tested on gcc 7.3.0
* Driver for your CAEN PCI card
* A DAQ hardware setup (docs coming on xenon wiki)
* A MongoDB deployment you can write to

## Install

To build the reader:

make

To build the crate controller controller:

make ccontrol

## Starting the Reader Process

./main {ID} mongodb://localhost:27017/database

The first argument is a unique ID that will identify your reader. This is important since your physical hardware setup needs to be associated with the software programs that will read the things out. So you probably want to map out ahead of time which reader processes will read out which optical links.

Note: this is designed to be able to run multiple times on one PC. You should make sure that optical links are not shared between readers, since the CAEN optical interface is not thread safe at the stage of a single link. Also, give each process its own ID. Within a single process the code will assign one dedicated readout thread per optical link in order to maximize read speeds. 

The ID number will create a unique process identifier for this instance of redax which appears as so: HOSTNAME_reader_ID. So if you run on host daq00 with ID 1 your process ID will be daq00_reader_1. This is the ID you use to address commands to this host.


## Starting the Broker (optional)

If you run with more than one readout process you should configure a broker. The broker handles communication with the user interface and translates human-level commands to the readout nodes. The broker is a python script stored in the 'broker' subdirectory. 

At the moment the broker is just a script, not an executable. So it can be run with:
`python broker.py --config=options.ini`

The config option points to an ini file for the broker. See the subdirectory for a readme detailing the options herein.

## Database Setup

You need to provide connectivity to a mongodb database using the URI.
This database should have the following collections.

**control:** is where commands go. The storage requirements are basically
zero since commands are deleted after all addressees have acknowledged
them.

**status:** should be configured as a capped collection. Each readout
node will write it's status here every second or so.

**log:** The DAQ will log here.

**options:** is where settings docs go. When sending the 'arm' command
the name of the options file should be embedded in the command doc.
If the reader can't find an options doc with that name it won't be
able to arm the DAQ.

If you run with the broker you additionally need a collection called **detector_control**, which will supercede control as the top-level user interface. The **control** collection will still be used by the broker to control the readout nodes but the user should not use it in this case. If there is just one readout node you may want to skip the broker and just directly control that node for simplicity.

## First steps: from nothing to starting a run

Install all prerequisites, a mongodb database, and the redax software as described above. If you have XENON wiki access there are some build notes on the DAQ page [here](https://xe1t-wiki.lngs.infn.it/doku.php?id=xenon:xenonnt:dsg:daq#reader wiki), however if you don't have access don't worry too much since everything is straight off google searches.

If you want to configure your status collection as capped (as well as the 'aggregate_status', which stored broker state history and 'system_monitor' for use with the optional monitor script) you can run helpers/initialize_databases.py.

Hook your digitizer up via optical link to your PC. Hopefully you're using our self-triggering firmware, if not you'll have to deal with configuring an external trigger for your digitizer yourself. This example will use just one digitizer.

Start the reader process with ./main 0 {MONGO_URI}, with mongo uri pointing to your mongo DB.

Use the script helpers/set_run_mode.py to put some settings into the 'options' collection of your database. You'll need to modify the script to put in your database connectivity parameters. The given register settings should work fine if you're using our self-trigger firmware. Otherwise adjust accordingly. Also be sure to change the optical link address at `boards[0].link` in case you're not plugged into link 0. Note the value in the 'name' field of the document you are inserting. One more thing, you probably want to look at data so set strax_output_path to some place you can write and read from. Your data will go there.

You probably want to see what your DAQ is doing. Open a terminal window and use the script helpers/monitor_status.py. Change the mongo connectivity to your database. This will poll the DAQ status and print. Right now it should be 'IDLE' because your DAQ isn't doing anything. Great. Leave it running.

Now you should be able to ARM the daq with helpers/runcommand.py. Use it like so:
`python runcommand.py arm {host}_reader_0 test`
This assumes you did not change the name of your options file from 'test' and host is the hostname of the machine. In case you did then replace 'test' with whatever you change it to. If the command is successful you should see the DAQ status change to ARMED in your status window. If this did not work you can check the 'log' collection for what is hopefully an enlightening error message. (note that you're basically manually do all the calls a website would do, which is why this seems like a lot of annoying little calls to the DB)

To start the DAQ use the runcommand.py script again, but only if the DAQ is in ARMED state, otherwise it will ignore the command. So:
`python runcommand.py start {host}_reader_0`
And bingo, you should be running now. The status window will print the current rate and should say RUNNING.

To stop the DAQ again:
`python runcommand.py stop {host}_reader_0`

Maybe you want to look at the data to make sure it's not rubbish. One option is to just run strax on it (beyond scope of this guide). But in case you just want a quick check use helpers/strax_waveform_inspector.py. You'll have to change the path within the script to point to your readout location. Note that this needs to point to a sub-file (as in the example script) so make sure you point it to an actual data file and not one of the directories. It should display a waveform (x-server required of course). If you don't have a display running use the strax_data_inspector to print some stuff out instead.

