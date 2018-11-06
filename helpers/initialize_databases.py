import pymongo
import os

#client = pymongo.MongoClient("mongodb://admin:%s@127.0.0.1:27017/admin"%os.environ["MONGO_PASSWORD"])
client = pymongo.MongoClient("mongodb://%s:%s@%s:%s/%s"%(os.environ['DAX_USERNAME'],
                                                         os.environ["DAX_PASSWORD"],
                                                         os.environ["DAX_HOST"], os.environ["DAX_PORT"],
                                                         os.environ['DAX_DB']))
db = client['dax']

# Create capped collection 'status' of about 50MB
db.create_collection("status", capped=True, size=52428800)

# Create capped collection 'aggregate_status' with same size
db.create_collection("aggregate_status", capped=True, size=52428800)

# Create capped collection 'system monitor' with same size
db.create_collection("system_monitor", capped=True, size=52428800)

# You cannot cap the control collection but you can set TTL to expire old docs
# (this is the Mongo shell command, pymongo must be similar)
#db.log_events.createIndex( { "createdAt": 1 }, { expireAfterSeconds: 3600 } )

