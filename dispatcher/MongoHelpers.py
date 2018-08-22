from pymongo import MongoClient
import datetime
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

    def __init__(self, URI, db_name, log):
        self.uri = URI
        self.client = MongoClient(URI)
        self.db = self.client[db_name]
        self.log = log

    def GetStatus(self, readers, client_timeout):
        # Get status of this detector
        status = -1
        for node in readers:
            cursor = self.db['status'].find({"host": node}).sort("_id", -1).limit(1)

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

    def GetDispatcherCommands(self):
        return self.db['command_queue'].find({            
            "status": {"$exists": False}}).sort("_id", -1)
    
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
        insert_doc['host'] = detectors[command_doc['detector']].readers()
        insert_doc['user'] = 'dispatcher'
        del insert_doc['_id']
        if command_doc['command'] == 'stop':
            self.log.entry("Processing stop command for detector %s"%(command_doc['detector']),
                           MongoLog.message)

            # At the moment I want to *always* send the stop command as it can be also
            # considered a 'reset'            
            self.db['control'].insert(insert_doc)
            update_status = 'processed'
        elif command_doc['command'] == 'start':
            if "mode" not in command_doc.keys():
                update_status = "error"
                self.log.entry("Tried to start the DAQ without specifying a run mode",
                               MongoLog.error)
            else:
                self.log.entry("Processing start command for %s "%(command_doc['detector']),
                               "in run mode %s"%command_doc['mode'])
                
                # Detector must be in "Idle" statue to arm
                if detectors[command_doc['detector']].status == 0:
                    insert_doc['command'] = 'arm'
                    self.db['control'].insert(insert_doc)
                    detectors[command_doc['detector']].start_arm(
                        datetime.datetime.now().timestamp(), command_doc)
                    update_status = 'initializing'
            
        elif command_doc['command'] == 'send_start_signal':            
            if detectors[command_doc['detector']].status == 2:
                self.log.entry("Starting detector %s"%(command_doc['detector']),
                               MongoLog.message)
                insert_doc['command'] = 'start'
                insert_doc['host'].append(detectors[command_doc['detector']].crate_controller())
                self.db['control'].insert(insert_doc)            
            
        else:
            self.log.entry("Received unknown command %s"%command_doc['detector'])
            update_status = "error"
            
        if update_status != None:
            self.db['command_queue'].update_one({"_id": command_doc['_id']},
                                               {"$set": {"status": update_status}})
        return
