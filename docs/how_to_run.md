## Contents
* [Intro](index.md) 
* [Pre-install](prerequisites.md) 
* [DB config](databases.md) 
* [Installation](installation.md) 
* [Options reference](daq_options.md) 
* [Example operation](how_to_run.md)
* [Waveform simulator](fax.md)

# Examples of Running the Program

*Fair disclaimer: I didn't test all these steps together yet so something might go wrong. Report if so. Or if you fix it update the docs (and remove the disclaimer)*

Let's assume you have the electronics setup given in the [installation section](installation.md) and you installed everything and started all the processes. Now you want to test taking some data. This section will walk you through it. Note that we're only going to use a single PC (daq0 in the [diagram](installation.md)) and we're going to use just 1 digitizer and 1 crate controller. But by now you have the knowledge to expand that if needed.

Note: this is going to be a minor pain in the neck. This software is designed to run smoothly with a large deployment containing many components and running via a [web interface](https://github.com/coderdj/nodiaq). So running it locally requires a bunch of direct database calls. Luckily a few scripts are prepared to help with that.

## 1. Create and insert an options file

Refer to the example script helpers/set_run_mode.py. These settings are unlikely to cause crashes, but also unlikely to be exactly what you want. This is provided as an example, it is not a "complete" options doc.

```python
from pymongo import MongoClient
client = MongoClient("mongodb://user:pw@host:port/authdb")
db = client['daq_db_name']
collection = db['options']

run_mode = {
    "name": "test",
    "user": "you",
    "description": "Initial test mode",
    "run_start": 1,
    "strax_chunk_overlap": 0.5,
    "strax_output_path": "/output_path",
    "strax_chunk_length": 5,
    "strax_fragment_length": 220,
    "baseline_dac_mode": "fit",
    "baseline_value": 16000,
    "boards":
    [
        {"crate": 0, "link": 0, "board": 100,
            "vme_address": "0", "type": "V1724", "host": "fdaq00_reader_0"},
        {"crate": 0, "link": 1, "board": 0,
            "vme_address": "0", "type": "V2718", "host": "fdaq00_ccontol_0"}
    ],
    "registers":
    [
        {"reg": "EF24", "val": "1", "board": -1},
        {"reg": "EF1C", "val": "1", "board": -1},
        {"reg": "EF00", "val": "10", "board": -1},
        {"reg": "8120", "val": "FF", "board": -1},
        {"reg": "8000", "val": "310", "board": -1},
        {"reg": "8080", "val": "310000", "board": -1},
        {"reg": "800C", "val": "A", "board": -1},
        {"reg": "8020", "val": "258", "board": -1},
        {"reg": "811C", "val": "110", "board": -1},
        {"reg": "8100", "val": "0", "board": -1},
        {"reg": "81A0", "val": "200", "board": -1},
        {"reg": "8098", "val": "1000", "board": -1},
        {"reg": "8038", "val": "12C", "board": -1},
        {"reg": "8060", "val": "3e8", "board": -1},
        {"reg": "8078", "val": "12C", "board": -1}
    ],
    "V2718":[{
        "led_trig": 0, 
        "s_in": 1, 
        "m_veto": 0, 
        "n_veto": 0, 
        "pulser_freq": 0}
    ],
    "channels": { "100": [0, 1, 2, 3, 4, 5, 6, 7]},
}

if collection.find_one({"name": run_mode['name']}) is not None:
    print("Please provide a unique name!")

try:
    collection.insert_one(run_mode)
except Exception as e:
    print("Insert failed. Maybe your JSON is bad. Error follows:")
    print(e)
```

Adjust the parameters as needed though. You definitely need to define your MongoDB connectivity. Probably also change the fields "name" and "strax_output_path" in the mode definition at least.

## 2. Monitor the state of the system

Use something similar to helpers/monitor_status.py, except that we're going to monitor the aggregate status, while that script monitors just one node.

```python
import pymongo
from pymongo import MongoClient
import os
import time

client = MongoClient("mongodb://user:pw@host:port/authdb")
db = client['daq_db_name']
collection = db['aggregate_status']

dets = ["my_detector"]
STATUS = ["Idle", "Arming", "Armed", "Running", "Error"]

while 1:
    for d in dets:
        docs= collection.find({"detector": d}).sort("_id", -1).limit(1)
        try: 
            doc = list(docs)[0]
            print("%s: Client %s reports status: %s rate: %.2f"%(doc['_id'].generation_time,
                                                                        doc['host'], STATUS[doc['status']], doc['rate']))
        catch:
            print("Detector not defined yet")
    time.sleep(1)
```

If you start running this it should just tell you every second the detector isn't defined yet. No biggie.

## 3. Define a detector and start a run

Leave the window from (2) running and open a new terminal. Now we want to insert a detector state document. Note that we will only insert this ONCE and subsequent state changes must be defined as updates. Including multiple state documents referring to a single detector will break everything.

```python
from pymongo import MongoClient

client = MongoClient("mongodb://user:pw@host:port/authdb")
db = client['daq_db_name']
collection = db['detector_control']

idoc = { 
        "detector" : "my_detector", 
        "active" : "true",
        "stop_after" : "60",
        "mode" : "test_mode",
        "user" : "me",
        "comment" : "",
        "link_mv" : "false",
        "link_nv" : "false",
        "remote": "false"
} 
collection.insert(idoc)
```

In your window from step (2) you should eventually see the detector start taking data.

## 4. Stop the run

Stop the run by updating the document. Either in a python script or just in shell.

```javascript
use daq_db_name
db.detector_control.update({detector: "my_detector"}, {$set: {active: "false"}})
```
The daq should cease acquisition in window (2) and go idle. You can also update the mode to a different mode (if you defined one) or change 'stop_after'.

## 5. Just for fun, check your runs DB

You must have defined a runs DB in your dispatcher settings. Have a look at the run collection and make sure you see entries for the data that was just taken.

