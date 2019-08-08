import os
import sys
from pymongo import MongoClient

command = sys.argv[1]

#client = MongoClient("mongodb://reader:%s@127.0.0.1:27017/dax"%os.environ["MONGO_PASSWORD"])
client = MongoClient("mongodb://daq:WIMPfinder@localhost:27017/admin")

db = client['xenonnt']
collection = db['control']

try:
    hostname = sys.argv[2]
except:
    hostname = os.uname()[1]
    print("No hostname provided so assuming it's running locally at %s"%hostname)

hostname = ['fdaq00_reader_7']

try:
    run_num = int(sys.argv[4])
except:
    run_num = 1
    print("Didn't provide a run number so trying 1")

try:
    runmode = sys.argv[3]
except:
    runmode = 'test'
    print("Didn't provide a run mode so trying 'test'")



doc = {}
if command == 'start':

    doc = {
        "detector": "NaI",
        "command": "start",
        "run": run_num,
        "mode": runmode,
        "host": hostname,
        "user": os.getlogin()        
    }
    
elif command == 'stop':

    doc = {
        "detector": "NaI",
        "command": "stop",
        "run": run_num,
        "mode": runmode,
        "host": hostname,
        "user": os.getlogin()
    }

elif command == 'arm':

    doc = {
        "detector": "NaI",
        "mode": runmode,
        "command": "arm",
        "run": run_num,
        "host": hostname,
        "user": os.getlogin()
    }
                   
else:
    print("Usage: python runcommand.py {start/stop/arm} {host} {runmode} {run}")
    exit(0)

try:
    collection.insert(doc)
    print("Command sent!")
except Exception as e:
    print("Failed!")
    print(str(e))
