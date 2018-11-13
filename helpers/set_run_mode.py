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
        {
            "crate": 0, "link": 0, "board": 100,
            "vme_address": "0", "type": "V1724"}
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
    "active": [{
        "V2718" : {"led_trig" :1, 
            "s_in" : 1, 
            "m_veto" : 1, 
            "n_veto" : 1, 
            "pulser_freq" : 1},
    "DCC10" : {"on" : 1},
    "V1495" : {"on" : 1},
    }]
}

if collection.find_one({"name": run_mode['name']}) is not None:
    print("Please provide a unique name!")

try:
    collection.insert_one(run_mode)
except Exception as e:
    print("Insert failed. Maybe your JSON is bad. Error follows:")
    print(e)
