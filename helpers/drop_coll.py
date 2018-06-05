import os
import sys
import time
from pymongo import MongoClient
import datetime

client = MongoClient("mongodb://daq:%s@127.0.0.1:27017/admin"%os.environ["MONGO_PASSWORD"])
db = client['data']
collection = db['test']

while(1):
    
    cs = db.command("collstats", "test")['size']/1e6
    collection.drop()
    print(datetime.datetime.now(), "Collection of size %.2f MB dropped"%cs)
    time.sleep(10)
     
