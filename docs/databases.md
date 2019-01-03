# Configuration of Backend Databases

There are two separate databases defined in the DAQ: the DAQ database and the runs database. In principle they could 
both point to the same deployment, or they could not. This is meant to give flexibility since the DAQ-internal database 
should *definitely* be installed somewhere local to the system (since outages bring the whole thing down) while the run 
database should just *probably* be installed local to the system (since temporary outages may go unnoticed). Even if 
both databases are locally installed, it may make sense to put them in separate deployments because of different replication 
rules. The runs database should probably be replicated out to remote computing sites for easy access while the DAQ database 
is fine with a more localized replication strategy, since in the latter case the replication is more to serve backup than 
availability.

This section will list each collection in each database, explain what it is for, and give the document schema. Note that since this is noSQL you can consider this a 'minimum document schema', as in the code expects these fields to be there but additional 
fields can be present as well. This is especially pertinent to run documents.

## The DAQ Database

### db.status

The status collection should be configured as a capped collection (see the 'helpers' directory for a script to set up the necessary capped collections). Each readout client report's it's status to this DB every few seconds. The broker 
queries the newest document from each node (considering also the time stamp of the document) to determine the aggregate 
DAQ state.

The form of the document is the following:
```python
{
    "host":   "xedaq00_reader_0", // DAQ name of client
    "status": 0,         // status enum
    "rate":  2343319,    // data rate in bytes/s since last update
    "blt": 9182,         // number of block transfers per second since last update
    "digitizers": 8,     // number of digitizers connected
}
```
The status enum has the following values:

|Value	|State |
|0	|Idle |
|1	|Arming (in the process of initializing, baselines, etc) |
|2	|Armed. Ready to start |
|3	|Running |
|4	|Error |
|5	|Timeout |
|6	|Unknown |

These values are valid throughout the entire DAQ.

### db.aggregate_status

This collection should also be configured as **capped**. It is written to by the broker only and provides the 
status history of the system on a detector level, which is a more palatable thing for operators to deal with. It is 
mostly redundant compared to status, except that it depends also on the state document given to the dispatcher. 

The document format is similar to status:
```python
{ 
    "status" : 0,                                    // status enum, see status collection
    "number" : null,                                 // current run number (here Idle so null)
    "detector" : "tpc",                              // detector string
    "rate" : 0,                                      // data rate MB/s float
    "readers" : 2,                                   // number of readers connected
    "time" : ISODate("2018-09-20T13:35:05.642Z"),    // time document inserted
    "buff" : 0,                                      // total buffered data from all readers float
    "mode" : null                                    // run mode string (null because Idle here)                                               
}    
```

### db.detector_control

This is the state document set by the user. There is exactly one document in this collection per detector, so in XENONnT 
there should be 3 documents in this collection (TPC, MV, NV). When a new state is configured, the document for the 
corresponding detector is *updated*. Upon update the broker will attempt to recitfy the current actual state of the DAQ 
with the goal state provided in this document.

Because some of these fields require slightly more explanation a table has been included below in lieu of inline comments.

```python
{ 
        "detector" : "tpc", 
        "active" : "true",
        "stop_after" : "60",
        "mode" : "two_boards_with_sync",
        "user" : "Coderre",
        "comment" : "",
        "link_mv" : "false",
        "link_nv" : "false",
        "diagnosis": "goal",
        "human_readable_status": "Idle"
}  
```

|Field	|Description |
|detector	|Either 'tpc', 'muon_veto', or 'neutron_veto'. Or whatever funny thing you've got in your lab. |
|active	|The user can set whether this detector is 'active' or not. If it's not active then we don't care about it's status. In fact we can't care since some readers will be reused when running in combined modes and may not longer belong to their original detectors.|
|stop_after	|How many minutes (or seconds? check code) until the run automatically restarts. This is a global DAQ state setting, not the setting for a single run. So if you want to run for an hour you set this to 60 minutes, put the detector active, and the broker should handle giving you the 1 hour runs. |
|mode	|The options mode we're running the DAQ in. Should correspond to the 'name' field of one of the documents in the options collection. |
|user	|Who gets credit/blame for starting these runs? This is the user who last changed this command doc and it will be recorded in the run documents of all runs recorded while this command is active. |
|Comment	|You can automatically connect a comment to all runs started with this setting by setting this field. The comment is put in the run doc for all runs started while the command is active. |
|link_mv, link_nv	|These are used by the frontend for detector=tpc only. They simply indicate if the neutron or muon veto are included as part of 'tpc' for this run (for running in combined mode). To the backend this makes no difference. A reader is a reader. To the frontend it can limit the options modes given to the user or help in setting visual cues in the web interface so the operator can figure out what's going on. |
|diagnosis	|The broker's take on what's going on. It's 'goal' if the program thinks everything is OK. It's 'error' if there's an error. It's 'processing' if the broker issued a command and is waiting for this to be implemented. In case the command takes too long the broker can set this field to 'timeout'. |
|human_readable_status	|Just translates the status enum to something people can read. Useful if displaying on a web page or someone calling the API who doesn't want to learn the codes. |

### db.control
The control database is used to propagate commands from the broker to the reader and crate controller nodes. It is used purely internally by the broker. Users wanting to set the DAQ state should set the detector control doc instead (preferably using the web interface). The exception to this is if you're running a small setup with a custom broker and want to issue commands to your readout nodes manually. 
```python
{
    "options_override" : {
        "strax_output_path" : "/strax_output/Run000048"
     },
     "mode" : "two_links",
     "user" : "web",
     "host" : ["fdaq00_reader_0"],
     "acknowledged" : [
         "fdaq00_reader_0"
     ],
     "command" : "arm" 
}    
```
|Field	|Description |
|options_override	|Override specific options in the options ini document. Mostly used to set custom output paths so that we're writing to the right place for each run. |
|mode	|Options file to use for this run. Corresponds to the 'name' field of the options doc. |
|user	|Who started the run? Corresponds to the last person to change the detector_status doc during normal operation. Exceptional stop commands can be automatically issued by various subsystems as well in case of errors.
host	List of all hosts to which this command is directed. Readers and crate controllers will only process commands addressed to them. |
|acknowledged	|Before attempting to process a command all reader and crate controller processes will first acknowledge the command as received. This does not indicate that processing the command was successful! It just indicates the thing tried. The broker has to watch for the appropriate state change of the slave nodes in order to determine if the command achieved its goal. |
|command	|This is the actual command. 'arm' gets the DAQ ready to start. 'start' starts readout by sending the S-in signal. 'send_stop_signal' puts the s-in to zero. 'stop' resets readout processes. |

### db.options

This is where the DAQ options are stored. There's a lot here to it got it's own chapter. Please see [here](daq_options.md).

### db.log

Sometimes the DAQ wants to inform users of some problem or exceptional circumstance. This is what the log is for. In the beginning we'll also include lots of debug output.

The log documents have the following format:
Basic log documents have the following simple format:
```python
{
	"message" : "Received arm command from user web for mode test",
	"priority" : 1,
	"user" : "fdaq00_reader_0"
}
```
Where the 'user' is an identifier for which process sent the message, or in case of messages sent by a user it can 
identify the user. The field 'message' is the message itself, and 'priority' is a log level enum. The following table 
gives the standard priorities:

|Priority	|Value	|Use |
|0	|DEBUG	|Debug output for developers. Can either be silenced in production or added with a TTL expiry index (see next section). |
|1	|MESSAGE	|Normal log output that is important to propagate during normal operation, but does not indicate any exceptional state. |
|2	|WARNING	|Inform the user of an exceptional situation. However this flag is reserved for minor issues that the system will handle on its own and should require no user input. |
|3	|ERROR	|An exceptional situation with major operational impact that requires intervention from the user. |
|4	|FATAL	|An exceptional situation reserved for processes that will likely or certainly cause the calling process to crash. |
|5	|USER	|Users can put messages into the log with the web frontend. |

**Setting a time-to-live (TTL) for debug logs**
Maybe you want to put some debug info into the logs so it's available during normal operation, but maybe we don't necessarily need to store this debug information until the end of time. You can put (and we did put) a MongoDB TTL index on the log collection that expires documents after one week if they contain the datetime field 'expire_from'. So if you want your docs to expire make sure to set 'expire_from' to datetime.now() or new Date() or whatever such that they are caught by this index.

Note! Setting this field will cause all documents to expire, not just DEBUG level. So really only include this field if you're sure the message you're sending will not be interesting for debugging things in the future.

## The Runs Database

The runs database contains collections which may be interesting collaboration-wide by other sub-systems. 

### db.runs

This is the runs database. It is the holy grail, the rosetta stone, and the sacred holy text of the experiment. It contains a directory of every bit of data taken, what settings the data was taken with, and where in the world that data might currently be found. In a production setting great care should be taken when interacting with this database as several systems outside the DAQ also use, change, and depend on it.

Here are the fields set by the DAQ. There are additional fields set at various levels of processing and analysis that are not covered here:

```python
{
    "number": 10000,                                    # run number
    "user": "notch",                                    # who started the run
    "start": ISODate("2018-09-20T13:35:05.642Z"),       # time that the run was started
    "end": ISODate("2018-09-20T13:55:05.642Z"),         # time that the run was ended. d.n.e. if run not ended
    "detectors":  ["tpc", "muon_veto", "neutron_veto"], # subdetectors in run
    "reader": {
       "ini": {DOCUMENT}                                # the entire options doc used for readout
    },
    "source": {
       "type": "none"                                   # the source type used. (i.e. LED, Rn220). 
    },
    "strax": {DOCUMENT},                                # override settings for strax (see strax docs)
    "data": [                                           # all locations where data for this run might be found
        {
            "type":    "live",                          # raw/processed/reduced/etc. live means pre-trigger.
            "host":    "daq",                           # usually host of the machine but 'daq' just means pre-trigger
            "path":    "/mnt/cephfs/pre_trigger/run_10000"
            "status":  "transferring"                   # transferring/transferred/error
         }
    ]
}
    
```

### db.users, db.shift_rules, db.shifts, etc

There's a bunch of cool stuff implemented in the web frontend that calls on additional collections. However since this isn't pertinent to the readout it isn't documented here.
