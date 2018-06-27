import os
import sys
import time
from pymongo import MongoClient
import datetime

client = MongoClient("mongodb://reader:%s@127.0.0.1:27017/dax"%os.environ["MONGO_PASSWORD"])
db = client['data']



while(1):

    for collection in db.collection_names():
        cs = db.command("collstats", collection)['size']/1e6
        db[collection].drop()
        print(datetime.datetime.now(), "Collection of size %.2f MB dropped"%cs)
        time.sleep(10)
     
