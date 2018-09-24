from pymongo import MongoClient
import datetime
import time
import copy

debug = 0
message = 1
warning = 2
error = 3
fatal = 4

class MongoLog:

    debug = 0
    message = 1
    warning = 2
    error = 3
    fatal = 4

    def __init__(self, URI, db_name, host):
        self.uri = URI
        self.client = MongoClient(URI)
        self.db = self.client[db_name]
        self.host = host

    def entry(self, message, priority):
        self.db['log'].insert({
            "user": self.host,
            "message": message,
            "priority": priority
        })


class ControlDB:

    def __init__(self, URI, db_name, log, runs_db):
        self.uri = URI
        self.client = MongoClient(URI)
        self.db = self.client[db_name]
        self.log = log
        self.runs_db = runs_db

    def SetAggregateStatus(self, detector_name, detector):
        indoc = {
            "detector": detector_name,
            "status": detector.status,
            "time": datetime.datetime.utcnow(),
            "rate": detector.aggregate['rate'],
            "buff": detector.aggregate['buff'],
            "number": detector.current_number,
            "mode": detector.mode,
            "readers": detector.n_readers
        }
        self.db['aggregate_status'].insert(indoc)
        
    def GetStatus(self, readers, client_timeout):
        # Get status of this detector
        status = -1
        rate = 0.
        buff = 0.
        for node in readers:
            cursor = self.db['status'].find({"host": node}).sort("_id", -1).limit(1)            
            
            # Node present?
            try:
                doc = list(cursor)[0]
                assert doc is not None
            except:
                status = 4
                break

            rate += doc['rate']
            buff += doc['buffer_length']

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
        ret = {
            "rate": rate,
            "buff": buff,
            "status": status
        }
        return ret

    def GetDispatcherCommands(self):
        return self.db['command_queue'].find({            
            "$or": [{"status": {"$exists": False}},
                    {"status": "queued"}]}).sort("_id", 1)

    def UpdateCommandDoc(self, cid, status):
        self.db['command_queue'].update({"_id": cid}, {"$set": {"status": status}})
        
    def ProcessCommand(self, command_doc, detectors):

        print("Received command %s for detector %s"%(command_doc['command'],
                                                     command_doc['detector']))
        if (command_doc['detector'] not in detectors.keys()):
            self.log.entry("Received invalid command for detector %s, which is not"
                           " in the dispatcher configuration", MongoLog.error)

            # Update command status with plain text
            self.db['command_queue'].update_one({"_id": command_doc['_id']},
                                    {"$set": {"status": "error"}})
            return -1

        update_status = None
        insert_doc = copy.deepcopy(dict(command_doc))

        # Addressing. We want this command to be seen by all readers associated with
        # this detector as well as the crate controller
        insert_doc['host'] = detectors[command_doc['detector']].readers()
        if detectors[command_doc['detector']].crate_controller() is not None:
            insert_doc['host'].append(detectors[command_doc['detector']].crate_controller())

        # Have to delete the ID field or you won't get a new ID (with timestamp) assigned
        insert_doc['user'] = 'dispatcher'
        del insert_doc['_id']

        
        # Stop command
        if command_doc['command'] == 'stop':
            self.log.entry("Processing stop command for detector %s"%(command_doc['detector']),
                           MongoLog.message)

            # At the moment I want to *always* send the stop command as it can be also
            # considered a 'reset'. Number -1 will be treated as a 'reset everything' command
            # while number == int will be considered for that run only
            insert_doc['number'] = -1
            if detectors[command_doc['detector']].current_number != None:
                insert_doc['number'] = detectors[command_doc['detector']].current_number

            # If the following lines are still in the code at production and you
            # can explain to me why they are wrong and make a PR to fix it, you get
            # one sucker punch
            stop_signal_doc = copy.deepcopy(dict(insert_doc))
            stop_signal_doc['command'] = 'send_stop_signal'
            self.db['control'].insert(stop_signal_doc)
            time.sleep(1)
            # End sucker punch code

            print("Sending doc ", insert_doc)
            self.db['control'].insert(insert_doc)
            update_status = 'processed'

            if detectors[command_doc['detector']].status == 3:
                self.runs_db.SetStopTime(detectors[command_doc['detector']].current_number)
                print("Set end time for %i"%detectors[command_doc['detector']].current_number)
                detectors[command_doc['detector']].current_number = None
                detectors[command_doc['detector']].mode = None

        # Start command, which actually means "ARM"
        elif command_doc['command'] == 'start':
            if "mode" not in command_doc.keys():
                update_status = "error"
                self.log.entry("Tried to start the DAQ without specifying a run mode",
                               MongoLog.error)
            else:
                #self.log.entry(("Processing start command for %s "%(command_doc['detector']) +
                #                "in run mode %s"%command_doc['mode']), MongoLog.message)
                
                # Detector must be in "Idle" statue to arm
                if (detectors[command_doc['detector']].status == 0 and
                    not detectors[command_doc['detector']].arming):
                    insert_doc['command'] = 'arm'
                    self.db['control'].insert(insert_doc)
                    detectors[command_doc['detector']].start_arm(
                        datetime.datetime.now().timestamp(), command_doc)
                    update_status = 'initializing'
                else:
                    update_status = "queued"

        # Send start signal tells crate controller to actually start the run
        elif command_doc['command'] == 'send_start_signal':            
            if detectors[command_doc['detector']].status == 2:
                self.log.entry("Starting detector %s"%(command_doc['detector']),
                               MongoLog.message)
                insert_doc['command'] = 'start'               
                self.db['control'].insert(insert_doc)
                update_status = "processed"
                if 'number' in insert_doc.keys():
                    detectors[command_doc['detector']].current_number = command_doc['number']
                    self.db['command_queue'].update_one({"_id": command_doc['_id']},
                                               {"$set": {"number": command_doc['number']}})
            
        else:
            self.log.entry("Received unknown command %s"%command_doc['detector'])
            update_status = "error"
            
        if update_status != None:
            self.db['command_queue'].update_one({"_id": command_doc['_id']},
                                               {"$set": {"status": update_status}})
        return


class RunsDB:

    def __init__(self, runs_uri, db_name, collection_name, control_uri, log):
        self.uri = runs_uri
        self.client = MongoClient(runs_uri)
        self.db = self.client[db_name]
        self.collection = self.db[collection_name]
        self.control_client = MongoClient(control_uri)
        self.log = log

    def GetNextRunNumber(self, detector):
        cursor = self.collection.find({'detector': detector}).sort("_id", -1).limit(1)
        try:
            last_doc = list(cursor)[0]
        except:
            self.log.entry("Didn't find any runs in your database so assuming you're "
                           "starting with run 0. Congratulations and good luck.",
                           MongoLog.message)
            last_doc = {"number": -1}
        return (last_doc['number']+1)

    def InsertRunDoc(self, number, run_start_doc):        
        run_doc = {
            "number": number,
            "detector": [run_start_doc['detector']],
            "user": run_start_doc['user'],
            "start": datetime.datetime.utcnow(),
            "mode": run_start_doc['mode'],
            "source": "none",
        }

        ini = self.control_client['dax']['options'].find_one({"name": run_start_doc['mode']})
        run_doc['reader'] = ini

        if "source" in ini.keys():
            run_doc['source'] = {"type": ini['source']}
            
        if "comment" in run_start_doc.keys() and run_start_doc['comment'] != '':
            run_doc['comments'] = [{
                "user": run_start_doc['user'],
                "date": datetime.datetime.now(),
                "comment": run_start_doc['comment']
            }]

        # Create initial data entry
        if 'strax_output_path' in ini:
            run_doc['data'] = [{
                "type": "live",
                "host": "daq",
                "location": ini['strax_output_path']
            }]

        self.collection.insert_one(run_doc)

    def SetStopTime(self, number):
        if number is None:
            return
        self.collection.update({"number": number}, {"$set":{"end": datetime.datetime.now()}})
