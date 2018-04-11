import pymongo
import os
uri = "mongodb://daq:%s@127.0.0.1:27017/admin"%os.environ["MONGO_PASSWORD"]
client = pymongo.MongoClient(uri)
db = client['dax']
collection = db['options']

run_mode = {
    "name": "test",
    "user": "coderre",
    "description": "Initial test mode",
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
        {"reg": "8020", "val": "32", "board": -1},
        {"reg": "811C", "val": "110", "board": -1},
        {"reg": "8100", "val": "0", "board": -1}
    ]    
}

if collection.find_one({"name": run_mode['name']}) is not None:
    print("Please provide a unique name!")

try:
    collection.insert_one(run_mode)
except Exception as e:
    print("Insert failed. Maybe your JSON is bad. Error follows:")
    print(e)
