## Contents
* [Intro](index.md) 
* [Pre-install](prerequisites.md) 
* [DB config](databases.md) 
* [Installation](installation.md) 
* [Options reference](daq_options.md)
* [Dispatchers](dispatcher.md)
* [Example operation](how_to_run.md)
* [Extending redax](new_digi.md)
* [Waveform simulator](fax.md)

# Dispatchers

If you're running a system with more than one process (reader and crate controller, readers on multiple servers, two readers on one server (not sure why you'd use this, honestly), etc), then trying to control and coordinate manually becomes a major hassle.
Even for one process after a while it becomes annoying.
This is the purpose of the dispatcher.
The dispatcher takes as input a) what the system is currently doing, and b) what the system should be doing, and issues commands to make this happen.
For a "simple" system, this usually isn't too complex.
For something with the interconnected-ness of XENONnT, it becomes absurd.

The dispatcher that comes with redax is the XENONnT dispatcher.
There is a 99% chance that it's far more complex than what you need, so you can either rip random parts out of it, or just write your own.
Writing your own is probably simpler than trying to figure out if you actually need this complex piece of logic, and we'll go over the various parts here.
It may be instructive to refer to the nT dispatcher for this discussion.

To prevent some potential ambiguity, I often use 'process' and 'host' interchangably - while you can run multiple readout processes on the same physical server, there's no real reason to do so.
There's a slight performance penalty because of overhead threads, and the database will take slightly longer to query because it has to sift through more documents.
When a distinction is necessary I'll probably refer to a "physical host" to mean the actual server any processes run on.

## Aggregate system status

This stage turns the various statuses of the hosts attached to a detector into a single aggregate status for that detector.
If you only have one host, then the aggregate system status is obvious.
If you have multiple hosts, this can be rather more complicated.
If all the hosts have the same status, it's simple.
What if one host is 'arming' and another is 'armed'?

In this step, you should collect the most recent status documents from each host (`MongoConnect.GetUpdate`) and figure out what the combination of `status` fields mean (`MongoConnect.AggregateStatus`).
While you're at it, you can add up the `rate` fields and do whatever you want with the routine status update information.
Have all the hosts reported in recently, or is one timing out?
Redax will never report a TIMEOUT or UNKNOWN status; these are higher level, and the dispatcher needs to assign these (at least TIMEOUT) when appropriate.

When it comes to the actual aggregation, there are broadly two kinds of statuses: "or" statuses and "and" statuses.
"Or" statuses are things like "timeout" - if any one host is in timeout, the whole detector is in timeout.
"And" statuses are things like "running" - if all the hosts are running, then the whole detector is running.
Figuring out which statuses are "or" and which are "and" is subject to debate.
The delineation used for nT works well enough, feel free to copy it, or modify if you have a better idea.

One additional consideration here: if you have multiple hosts, are all of them _supposed_ to be doing something?
Maybe you're using a run config that doesn't need all hosts you have.
Before the aggregate step, you may want to filter which hosts are actually being considered to only those that are supposed to be participating in whatever the detector is supposed to be doing.
If you have a crate controller, then this is probably always going to be participating, so you can use the `mode` that it reports and only look at the processes that are required for that config.
If you don't have a crate controller, it'll be a bit more complex.

## Get desired system state

This should be pretty straightforward.
You can have a collection with one document per detector that contains all the necessary information (if it should be on or off, what mode, etc).
Alternately, you can store these things individually and aggregate them when you need them.
The advantage of the second is that changes are easier to track (also more compactly), but the first is easier to maintain.

## Make it so

This is where the magic happens.
You have the two necessary inputs, now you need to issue commands to turn one into the other.
You can largely divide this into two cases - if the detector is supposed to be on or off.
The "off" case is simple - if the detector isn't off, send a STOP command (unless you just sent one recently).
The "on" case is rather less simple.
Going from "on" to "off" is a one-step process.
It may take a few seconds (probably should if you have a crate controller), but you issue STOP and in a few seconds it'll have happened.
Going from "off" to "on" takes longer.
First you issue the ARM command, then you have to wait for all the hosts to go through the arming sequence and end up in the ARMED state, and only then can you issue the START command.
You don't want to issue commands if hosts aren't in a position to act on them immediately.

UNKNOWN and TIMEOUT are the statuses that require the most careful approach.
UNKNOWN is expected when the detector is transitioning and the hosts don't all have the same status,
so as long as the system hasn't been UNKNOWN for too long it's probably fine.
TIMEOUT seems straightforward - as long as you can figure out what exactly is timing out.
Generally this means one process (or physical host) has crashed, but because mutexes this is only the most common cause.
Also, what was the detector supposed to be doing when it timed out?
Are we still in the timeout period after issuing a command?
These considerations require some subtlety to handle properly

## Other considerations

The above are more or less the minimum requirements for a dispatcher.
There are, naturally, more things a dispatcher can do.

### Rundocs

The dispatcher can also be responsible for building the run document and inserting it into your runs database.
The rundoc should probably contain a complete copy of the options that the readers/cc are going to load, so you can refer to it later.

### Run start and stop times

Often it's desirable to know "exactly" when a run started or stopped.
If you have a crate controller, this will be based on when it sends or stops the S-IN signal.
Redax tracks when the CC process receives the command; the START signal takes under 1ms, the STOP signal about 2ms.
For the STOP command, you'll want to make sure it's issued to the CC a few seconds before it goes to the readers, because it takes a bit of time for the readers to clear the boards and finish processing the data.
The readers stop reading from the boards as soon as the receive the STOP, so you don't want to potentially lose a couple of ms of data from some boards due to poor timing.
