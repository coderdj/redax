import pymongo
from pymongo import MongoClient
import os
import time

#client = pymongo.MongoClient("mongodb://daq:%s@127.0.0.1:27017/admin"%os.environ["MONGO_PASSWORD"])
client = MongoClient("mongodb://dax:%s@ds129770.mlab.com:29770/dax"%os.environ["MONGO_PASSWORD"])

db = client['dax']
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
