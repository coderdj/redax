# REDAX
D. Coderre, 2018. See license in LICENSE file.

## Prerequisites

mongodb cxx driver
CAENVMElib v2.5+
Driver for your CAEN PCI card
A DAQ hardware setup
A MongoDB deployment

## Install

make

## Usage

./main mongodb://localhost:27017/database

The argument is the connection string to the dax database. 
This database has to be configured as described in the 
section 'database'. You may include a username and password 
as well as any other syntax allowed by a MongoDB URI (it is
parsed by the driver as a URI). 

There is an optional third argument, which is an integer or 
string. In case you run a single node per host the node will 
identify itself by its hostname. But if you run two nodes on 
one host the hostname is no longer unique. The argument is appended
to the hostname (connected with an underscore '_') in order to
allow definition of unique hosts.

For example, if you run on host **daq00**

**One node:**

./main {MONGO_URI}

Here we put no argument. This instance will be addressed by
hostname "daq00".

**Two nodes one host:**

./main {MONGO_URI} 0
./main {MONGO_URI} 1

Here we start two instances with two arguments. One instance
will be addressed as "daq00_0" and the other as "daq00_1".

Easy peasy.

## Database Setup

You need to provide connectivity to a mongodb database using the URI.
This database should have the following collections.

**control:** is where commands go. The storage requirements are basically
zero since commands are deleted after all addressees have acknowledged
them.

**status:** should be configured as a capped collection. Each readout
node will write it's status here every second or so.

**options:** is where settings docs go. When sending the 'arm' command
the name of the options file should be embedded in the command doc.
If the reader can't find an options doc with that name it won't be
able to arm the DAQ.

