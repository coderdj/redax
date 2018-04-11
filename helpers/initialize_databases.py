import pymongo
import os

client = pymongo.MongoClient("mongodb://daq:%s@127.0.0.1:27017/admin"%os.environ["MONGO_PASSWORD"])
db = client['dax']

# Create capped collection 'status' of about 50MB
db.create_collection("status", capped=True, size=52428800)

# Create capped collection 'aggregate_status' with same size
db.create_collection("aggregate_status", capped=True, size=52428800)
