import configparser
import argparse
from pymongo import MongoClient
import os
import json
import datetime
import time

STATUS=["Idle", "Arming", "Armed", "Running", "Error", "Timeout", "Undecided"]

# Parse command line
parser = argparse.ArgumentParser(description='Manage the DAQ')
parser.add_argument('--config', type=str,
                   help='Path to your configuration file')
args = parser.parse_args()

config = configparser.ConfigParser()
try:
    config.read(args.config)
except Exception as e:
    print(e)
    print("Invalid configuration file: %s"%args.config)
    exit(-1)
#print(config.sections())

# Initialize database connection
try:
    control_client = MongoClient(
        config['DEFAULT']['ControlDatabaseURI']%os.environ["MONGO_PASSWORD"])

    runs_client = MongoClient(
        config['DEFAULT']['RunsDatabaseURI']%os.environ["MONGO_PASSWORD"])
    control_db = control_client['dax']
    runs_db = runs_client['run']
except Exception as E:
    print("Failed to connect to control or runs database. Did you set your "
          "password in the MONGO_PASSWORD environment variable? The "
          "following URIs were attempted.")
    print("Control: %s"%config['DEFAULT']['ControlDatabaseURI'])
    print("Run: %s"%config['DEFAULT']['RunsDatabaseURI'])
    print(E)
    exit(-1)

def GetStatus(status_collection, detector_config, client_timeout):
    # Get status of this detector
    status = -1
    for node in detector_config['readers']:
        cursor = status_collection.find({"host": node}).sort("_id", -1).limit(1)

        # Node present?
        try:
            doc = list(cursor)[0]
            assert doc is not None
        except:
            status = 4
            break

        # Node timing out?
        if (datetime.datetime.now().timestamp() -
            doc['_id'].generation_time.timestamp() > client_timeout):
            status = 5
            continue

        # Otherwise node same as other nodes?
        if status == -1:
            status = doc['status']
        elif doc['status'] == 4:
            status = 4
            break
        elif status != doc['status']:
            status = 6

    return status

try:
    detector_config = json.loads(config['DETECTORS']['MasterDAQConfig'])
    node_timeout = config.getint('DEFAULT', 'ClientTimeout')
    poll_frequency = config.getint('DEFAULT', 'PollFrequency')
except Exception as E:
    print(E)
    print("Your config file has to define the above value!")
    raise

while(1):

    detector_status = {}
    
    for detector in detector_config.keys():
        detector_status[detector]=GetStatus(control_db['status'], 
                                            detector_config[detector],
                                            node_timeout)

        print("Detector %s has status %s"%(detector, STATUS[detector_status[detector]]))
    time.sleep(poll_frequency)
