import os
import sys
from pymongo import MongoClient
import argparse

def main(db):
    parser = argparse.ArgumentParser()
    parser.add_argument('--command', help='arm/start/stop', choices='arm start stop'.split(), required=True)
    parser.add_argument('--number', type=int, help='Run number', default=1)
    parser.add_argument('--mode', help='Run mode', default=None)

    args = parser.parse_args()

    doc = {
            "detector" : "none",
            "command" : args.command,
            "number" : args.number,
            "user" : os.getlogin(),
            "host" : ["fdaq00_reader_0"],
            }
    if args.command == 'arm' and args.mode is None:
        print('Can\'t arm without specifying a mode')
        return
    doc['mode'] = args.mode

    db['control'].insert_one(doc)

if __name__ == '__main__':
    with MongoClient("mongodb://daq:%s@localhost:27017/admin" % os.environ['MONGO_DAQ_PASSWORD']) as client:
        try:
            main(client['fax_test'])
        except Exception as e:
            print('Caught a %s: %s' % (type(e), e))

