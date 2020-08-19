import os
import sys
from pymongo import MongoClient

command = sys.argv[1]

client = MongoClient("mongodb://daq:%s@localhost:27017/admin" % os.environ['MONGO_ADMIN_PASSWORD'])

db = client['fax_test']
collection = db['control']

try:
    hostname = sys.argv[2]
except:
    hostname = os.uname()[1]
    print("No hostname provided so assuming it's running locally at %s"%hostname)

hostname = ['fdaq00_reader_0']

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
        "detector": "TPC",
        "command": "start",
        "run": run_num,
        "mode": runmode,
        "host": hostname,
        "user": os.getlogin()
    }

elif command == 'stop':

    doc = {
        "detector": "TPC",
        "command": "stop",
        "run": run_num,
        "mode": runmode,
        "host": hostname,
        "user": os.getlogin()
    }

elif command == 'arm':

    doc = {
        "detector": "TPC",
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
