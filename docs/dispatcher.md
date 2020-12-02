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

If you're running a system with more than one process (reader and crate controller, readers on multiple servers, two readers on one server (not sure why you'd use this, honestly), etc), then trying to control and coordinate manually becomes a major hassle (even using the super-helpful scripts provided).
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

## Step 1: Aggregate system status

This stage turns the various statuses of the hosts attached to a detector into a single aggregate status for that detector.
If you only have one host, then the aggregate system status is obvious.
If you have multiple hosts, this can be rather more complicated.
If all the hosts have the same status, it's simple.
What if one host is 'arming' and another is 'armed'?

In this step, you should collect the most recent status documents from each host (`MongoConnect.GetUpdate`) and figure out what the combination of `status` fields mean (`MongoConnect.AggregateStatus`).
While you're at it, you can add up the `rate` and `buffer_size` fields and do whatever you want with the routine status update information.
Have all the hosts reported in recently, or is one timing out?
Redax will never report a TIMEOUT or UNKNOWN status; these are higher level "meta" statuses, and the dispatcher needs to assign these (at least TIMEOUT) when appropriate.

When it comes to the actual aggregation, there are broadly two kinds of statuses: "or" statuses and "and" statuses.
"Or" statuses are things like "timeout" or "error" - if any one host has this status, the whole detector has this status.
"And" statuses are things like "running" - if all the hosts are running, then the whole detector is running.
Figuring out which statuses are "or" and which are "and" is subject to debate.
For nT I settled on the "or" statuses as 'ARMING', 'ERROR', 'TIMEOUT', and 'UNKNOWN', and "and" as 'IDLE', 'ARMED', and 'RUNNING'.
You could make a case that 'ARMING' should be an "and" status, and in principle yes, but in practice no.
The crate controller arms in a couple of milliseconds, so it's unlikely that a status update will happen during this short period.
Thus, the dispatcher will only ever see this host go from IDLE to ARMED.
The readers take at least 3 seconds to arm, usually up to 15 or 20 seconds depending on how long the baseline routine takes, so you'll always see a couple of status updates during the arming sequence.
So unless you want to code the logic to deal with the CC being ARMED while the readers are still ARMING, leave it like it is.

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
See `MongoConnect.GetWantedState` for redax >= v2 or v1 for a comparison.

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
You don't want to issue commands if hosts aren't in a position to act on them immediately, because then they'll act on them at "random" times and serve to confuse the system.
Let's look at this in more depth.

### Starting

Going from IDLE to ARMED is straightforward - issue an ARM command to all necessary hosts.
Going from ARMED to RUNNING is also straightforward - issue a START command.
You don't need to worry about timing for a "large" setup involving a crate controller, because at the end of the arming sequence the boards are all ready to go.
The START command doesn't actually do anything to the readers other than change what status they report; all the threads are created in the arming sequence and are just waiting for data to start showing up.

### Stopping

Going from RUNNING to IDLE takes a bit more subtlety.
When the readers get the STOP command, they immediately do exactly that.
They stop reading from the digitizers, finish processing whatever is still in the queue, and start destructing objects.
You want to make sure that this happens after data has stopped flowing.
We issue a STOP command to the CC 5 seconds before it goes to the readers, because this gives the readers enough time to finish everything before they stop.
For a lower readout rate, 5 seconds is being very generous, however you also have to consider this value relative to the command polling frequency.
The default value here is 100ms, but in the past it's varied from 1ms (typo) to 1s.
If the readout rate is "low", a 1 second difference is fine, but if your poll frequency is only 1 second, you can run into the case where the CC gets the command a handful of milliseconds before a reader, and this is dangerous because you might lose data.

### UNKNOWN

UNKNOWN is expected when the detector is transitioning and the hosts don't all have the same status, so as long as the system hasn't been UNKNOWN for too long it's probably fine.

### TIMEOUT

TIMEOUT seems straightforward - as long as you can figure out what exactly is timing out.
Generally this means one process (or physical host) has crashed, but because mutexes this is only the most common cause.
The stopping sequence locks a mutex, so you have a massive data backlog when a STOP comes through the status updating thread has to wait until it can lock that mutex (it's necessary), so it'll look like that host is timing out even though it's perfectly fine and will return to normal "soon".
Also, what was the detector supposed to be doing when it timed out?
Are we still in the timeout period after issuing a command?
These considerations require some subtlety to handle properly.

## Other considerations

The above are more or less the minimum requirements for a dispatcher.
There are, naturally, more things a dispatcher can do.

### Rundocs

The dispatcher can also be responsible for building the run document and inserting it into your runs database.
The rundoc should probably contain a complete copy of the options that the readers/cc are going to load, so you can refer to it later.

### Run start and stop times

Often it's desirable to know "exactly" when a run started or stopped.
If you have a crate controller, this will be based on when it sends or stops the S-IN signal.
Redax tracks when the CC process receives and acknowledges commands; the START command takes less than 1ms to process; STOP takes about 2ms because it also has to destroy a bunch of objects.
This gives timestamps accurate to within 1ms of NTP time, which is perfectly fine for a TPC with a 1ms drift length.
Yes, we have a GPS module, but please explain to me why we need sub-ms precision to enable an S2-only analysis if a supernova comes through.
The timestamp of an S2-only event has an uncertainty of the drift length, because you don't know how long the electrons were drifting.
Therefore, having a global timestamp as accurate as your drift length doesn't impact you negatively.
`</rant>`

The run stop time also is a bit nuanced in situations where things aren't strictly going to plan, but this largely comes down to exactly how you set the run end-time.
If you do it whenever you issue a STOP, you'll likely run into edge cases where you issue run end-times incorrectly if detectors are misbehaving.
A lot of this is down to the implementation details.

### Soft stop

This is a quality-of-life feature that gives another option regarding stopping an active detector.
Rather than stop "now", it waits until the active run is finished and then issues the STOP.

### Kicking it while it's down, or flogging a dead horse

This metaphor combination covers two broad things.
The first is about issuing commands too quickly.
The system takes some time to respond, the arming and stopping sequences for instance take several seconds each.
The infamous `sleep(2)` exists because doing things too quickly is a great way to cause crashes.
It's not a bad idea to let the system sit for a few seconds between ending one run and starting the next.
What's the rush?
An extra 3 seconds idle out of an hour doesn't really change anything.
Let the system respond properly to something before trying to get it to do something else.

The second metaphor relates much more to handling timeouts.
Usually, if a host is timing out, it also isn't acknowledging new commands.
You can queue up a bunch of STOPs because that's kinda the only thing the dispatcher can do, but there's no difference between one un-ack'd STOP and twenty.
A smart dispatcher would check this first rather than queueing up however many STOPs that the poor host will have to wade through when it comes back up.

### Error reporting

At some point the dispatcher will need to either call for help or let someone know that something isn't going according to plan.
This is where you need some method of reporting errors.
You don't want to fill up your database with "trivial" messages, so these can get written to the logfiles.
Also, if the detector is acting up (usually timing out in some way - either a host is timing out or it took too long to arm or something), you probably want to issue a message, but not on every update cycle, so you want some kind of backing-off mechanism.
When the issue is cleared, you want this to be reset so you can catch the next error easily.

# A deeper look into the nT dispatcher

Most (all?) of the complexity of the nT dispatcher comes because of the requirement of "linked" mode, where the TPC and at least one veto are operated as a single detector.
This is currently supported by having extensive `if link_mv` or `if link_nv` everywhere inside the TPC-specific logic blocks.
A lot of functions take `link_mv` and `link_nv` arguments that influence the values returned.
The TPC crate controller has a special place here, because it's the one that issues the master S-IN signal.
The controllers for the veto detectors still need to be initialized as usual (modulo not sending the S-IN themselves), but you might not be able to treat them identically with the TPC CC.
Transitioning from unlinked to linked mode is best done when the everything involved is idle.

One way to handle linked mode that doesn't involve massive blocks of `if link_nv and...` could be to treat the various combinations of TPC and vetoes as distinct detectors, so you would have four "detector groups", corresponding to all possible combinations of linking-ness.
Is this the best way?
I don't know, but it might be simpler than large blocks of `if` statements and the inevitable case that falls through the holes.
