import pymongo
from pymongo import MongoClient
import os
import time

client = MongoClient("mongodb://daq:%s@xenon1t-daq:27017/admin"%os.environ["MONGO_PASSWORD"])

db = client['daq']
collection = db['status']

clients = ["fdaq00"]
STATUS = ["Idle", "Arming", "Armed", "Running", "Error"]

while 1:
    for c in clients:
        docs= collection.find({"host": c}).sort("_id", -1).limit(1)
        doc = list(docs)[0]
        print("%s: Client %s reports status: %s rate: %.2f buffer: %i"%(doc['_id'].generation_time,
                                                                        doc['host'], STATUS[doc['status']], doc['rate'],
                                                                        doc['buffer_length']))
    time.sleep(1)
