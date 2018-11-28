import pymongo
from pymongo import MongoClient
import os



client = MongoClient("mongodb://dax:%s@ds129770.mlab.com:29770/dax"%os.environ["MONGO_PASSWORD"])
#uri = "mongodb://admin:%s@127.0.0.1:27017/admin"%os.environ["MONGO_PASSWORD"]
#client = pymongo.MongoClient(uri)
db = client['dax']
collection = db['options']

run_mode = {
    "name": "test",
    "user": "elykov",
    "description": "Initial test mode",
    "mongo_uri": "mongodb://reader:%s@127.0.0.1:27017/dax",
    "mongo_database": "data",
    "mongo_collection": "test",
    "boards":
    [
        {"crate": 0, "link": 0, "board": 100,
            "vme_address": "0", "type": "V1724"},
        {"crate": 0, "link": 1, "board": 0,
            "vme_address": "0", "type": "V2718"}
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
        "led_trig": 1, 
        "s_in": 1, 
        "m_veto": 1, 
        "n_veto": 1, 
        "pulser_freq": 400}
    ],
    "DDC-10":[{        
        #some paraeters for DDC10 HEV dunno if should be here or in a separate doc
         "rise_time_cut": 30,
         "required": "true",
         "window": 300,
         "parameter_0": 0,
         "prescaling": 100,
         "parameter_2": 0,
         "integration_threshold": 450000,
         "parameter_3": 10000,
         "outer_ring_factor": 2,
         "inner_ring_factor": 1,
         "width_cut": 30,
         "component_status": 6,
         "sign": 0,
         "delay": 400,
         "parameter_1": 0,
         "address": "192.168.131.62",
         "signal_threshold": 200}
    ],
    "V1495": [{
        "on" : 1}
    ]
}

if collection.find_one({"name": run_mode['name']}) is not None:
    print("Please provide a unique name!")

try:
    collection.insert_one(run_mode)
except Exception as e:
    print("Insert failed. Maybe your JSON is bad. Error follows:")
    print(e)
