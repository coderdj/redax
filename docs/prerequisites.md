## Contents
* [Intro](index.md) 
* [Pre-install](prerequisites.md) 
* [DB config](databases.md) 
* [Installation](installation.md) 
* [Options reference](daq_options.md) 
* [Example operation](how_to_run.md)

# Installation of Prerequisites

This section lists all the prerequisites needed. We're assuming an Ubuntu 18.04 LTS system.

## Hardware notes

The most basic system will consist of a CAEN V1724 connected via optical link to a CAEN A3818 or A2818 PCI(e) card installed in the same PC where the software will run. More complex setups, for example using a V2718 crate controller to facilitate synchronized starting of multiple V1724, are of course also possible.

**Note:** the V1724 can be either used with the DPP_DAQ firmware or with the default firmware without 'zero-length-encoding' enabled. ZLE support may be included in a future release if it is needed.

## Libraries from the package repo

  * [LZ4](http://lz4.org) is needed as the primary compression algorithm.
  * [Blosc](http://blosc.org/) is the secondary for compression algorithm.
  * Normal build libraries required. Note that we're using C++17 so require a relatively recent gcc, in case you're on an older OS.
  
Install with: `sudo apt-get install build-essential libblosc-dev liblz4`

## CAEN Libraries

  * CAENVMElib v2.5+
  * Driver for your CAEN PCI card

Both of these are available from [CAEN](http://www.caen.it) directly. We also maintain a private repository in the XENON1T organization called daq_dependencies with the production versions of all drivers and firmwares. 


## MongoDB CXX Driver

This is a condensation of the instructions found [here](https://mongodb.github.io/mongo-cxx-driver/mongocxx-v3/installation)

### Step 1: Build mongo C driver
The CXX driver depends on the C driver now.

1. Get with: `wget https://github.com/mongodb/mongo-c-driver/releases/download/1.9.2/mongo-c-driver-1.9.2.tar.gz` (or whichever version you choose)
2. untar `tar -xvzf mongo-c-driver-1.9.2.tar.gz`
3. See instructions [here](http://mongoc.org/libmongoc/current/installing.html). We're not gonna mess with the package repo versions but will compile from source.
4. prerequisites: `sudo apt-get install pkg-config libssl-dev libsasl2-dev`
5. `./configure –disable-automatic-init-and-cleanup`
6. `make && sudo make install`

That's it! It worked perfectly the first time when we did it on a fresh system.

### Step 2: Build the cxx driver

1. get code: `git clone https://github.com/mongodb/mongo-cxx-driver.git –branch releases/stable –depth 1` Note this gets the newest stable release. For deployment might want to fix a version and update only at fixed times.
2. `cd mongo-cxx-driver/build`
3. `cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local ..` The docs make special note not to forget the trailing '..'
4. Install polyfill (just in case?) `sudo make EP_mnmlstc_core`
5. `make && sudo make install`

That's it. 

## Installing MongoDB Server

The previous step installed the mongodb C++ driver onto your machine. The driver allows you to interact with a mongodb 
deployment sitting anywhere provided you have the proper credentials. However, it doesn't actually create a database. If 
you need to install a database there are a few options.

1. Use a cloud-hosted DB. [Mlab](https://www.mlab.com) and [MongoDB Atlas](https://www.mongodb.com/cloud/atlas) are popular choices and feature a free tier, which is enough for testing. Note that MLab has been acquired by MongoDB.
2. Use your running cloud service. If you happen to be XENONnT we use a mongo cloud deployment that allows us to deploy new databases to our own servers at the click of a button. It is a service from MongoDB and costs a fee per data-bearing machine. Our production systems are managed in this way.
3. Install your own standalone database. This is easy to do and gives you full freedom to use your own hardware. Additionally, a cloud-based solution may not be fully appropriate for a DAQ deployment that is inexorably tied to specific physical hardware (i.e. the detector and electronics readout). Instructions for this follow.

### Local Installtion

The easiest way to do this is to google it.

**Enable authentication**

MongoDB has had a bit of a bad security reputation since it contains no security protection at all by default. This must 
be manually configured. Therefore a bunch of smart people working at sometimes surprisingly professional-sounding places and 
storing also surprisingly sensitive data (much more important than our temporary DAQ data) created databases that were 
completely open to the outside world. Of course a bunch of obviously smarter but morally deficient people then gained 
access to these machines, which is so easy you might do it by accident, and deleted all the data in demand of a ransom, 
which was also never paid. It was a whole thing. Don't be one of those guys and put at least a password on your database.

See the official docs [here](https://docs.mongodb.com/manual/tutorial/enable-authentication/).

Log in to the database:
```
mongo --host 127.0.0.1 --port 27017
```

Create a user in database 'admin' with full control:
```
use admin
db.createUser(
  {
    user: "user",
    pwd: "password",
    roles: [ { role: "userAdminAnyDatabase", db: "admin" },
             { role: "readWriteAnyDatabase", db: "admin" } ]
  }
)
```

This is the bare minimum user configuration for a useful DB. For our full deployment we will create several database users and give each only the permissions it needs to operate.

Open the file /etc/mongod.conf and change the config file to enable auth. The lines are:

```
security:
  authorization: enabled
```
  
Restart the process:

```sudo systemctl restart mongod```

Now try to log in like before… you should get in but any command (i.e. 'show dbs) should fail, right? If it doesn't fail something went wrong. If it does fail then you did it right and are now secure. I still wouldn't expose my database to the internet… but it's probably fine in your subnet.

You can log in now by providing a user, password, and authentication database like so:

```mongo -u user -p password --authenticationDatabase=admin --host=127.0.0.1 --port=27017```

Or using a connection string like so:

```mongo mongodb://user:password@127.0.0.1:27017/admin```

Another note on security. A database isn't really meant to be a public-facing thing like an api. So try to avoid any unnecessary exposure to the internet (certainly) or public/semipublic subnets. An API is recommended for for access to the database (in our case by the slow control system). It is easy to expand this
to do whatever you want. 

## Anaconda Environment for the dispatcher and system monitor

If you use the dispatcher, system monitor, and API functionalities you need python3 and some libraries. If you're installing this on a test or shared system you might consider installing everything in an [anaconda](https://www.anaconda.com/) environment.

Here are the required packages via pip (you may need to install it):
```
pip install psutil pymongo 
```

