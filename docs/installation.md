# Redax installation and initial configuration
 
 This section explains how to install and configure redax with a simple example deployment strategy involving two readout 
 PCs reading out digitizers and one PC additionally controlling a V2718 crate controller. 
 
 Figure I-1 illustrates this example setup.
 
<img src="figures/installation_diagram.png" width="600">
<br>
<strong>Figure I-1: We will refer to this example setup for this chapter. This is only for illustration. Currently a total of 5 different PCs with at least 5 readout processes are planned for the final setup.</strong>
<br>

PC-0 is the more straightforward case. It runs one readout process which is responsible for reading out the digitizers 
connected to A3818 optical link zero. It additionally runs one crate controller process that controls a V2718 crate 
controller connected to A3818 optical link one. 

PC-1, on the other hand, showcases the ability of redax to read out multiple optical links. From the perspective 
of raw readout speed there is no disadvantage to this since each optical link is read internally in its own thread. 
On the other hand, a second process is employed on the same PC reading out the third link. This is also allowed since the 
command addressing and broker logic refers to the 'daq name' of the process, not just the hostname of the PC. 

## Reader installation: Run on both PCs

If you already have the prerequities described in a [previous chapter](prerequisites.md) this is as simple as compiling the 
software and running it.

```
cd redax
make
```

You then need to start the process, which takes two important command line arguments. 

```
/main {ID} {MONGO_URI}
```

Here **ID** is an integer that will designate this process and be used in addressing. If you run multiple processes of 
the same type on one host you *must* provide them with unique ID numbers. **URI** is the complete URI to your DAQ 
MongoDB database, including username, password, and authentication database as required.

Assuming we have a database at database.com port 27017 with user daq, password alsodaq, and authentication database 
authdb then the process on host daq0 might be started with:

```
./main 0 mongodb://daq:alsodaq@database.com:27017/authdb
```

This will start a process that will be named daq0_reader_0. The process naming convention is 
`{HOSTNAME}_{PROCESS_TYPE}_{ID}`, where PROCESS_TYPE is either reader or ccontrol, HOSTNAME is the hostname of the PC, 
and ID is the number provided by the operator on the command line.

For host daq1 we want to start two processes like so:

``` 
./main 0 mongodb://daq:alsodaq@database.com:27017/authdb
./main 1 mongodb://daq:alsodaq@database.com:27017/authdb
```

This will start processes with names daq1_reader_0 and daq1_reader_1. Note that these names are important for configuring 
the options documents in the next chapter!

Once the processes are started they will immediately begin polling the database looking for commands and updating the 
database with their status. 

## Crate Controller Installation, Run on daq0

The crate controller module is responsible for the V2718 crate controller. It will also be responsible for configuring the 
V1495 general purpose module (if required) and the DDC-10 high energy veto module. 

To compile it:
```
cd redax
make ccontrol
```

Running it is the same as running the reader:
```
./ccontrol 0 mongodb://daq:alsodaq@database.com:27017/authdb
```

Assuming you run that command on daq0 you'll have a crate controller process running called daq0_ccontrol_0. Identically 
to the reader process this program will immediately begin polling and updating the DAQ database.
