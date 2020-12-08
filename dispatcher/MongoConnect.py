from pymongo import MongoClient
import datetime
import os
import json
from DAQController import STATUS
import threading
import time

'''
MongoDB Connectivity Class for XENONnT DAQ Dispatcher
D. Coderre, 12. Mar. 2019

Brief: This code handles the mongo connectivity for both the DAQ 
databases (the ones used for system-wide communication) and the 
runs database. 

Requires: Initialize it with the following config:
{
  "ControlDatabaseURI":   {string}, mongo URI with '%s' symbol in place of pw,
  "ControlDatabaseName":  {string} the name of the control database,
  "RunsDatabaseURI":      {string}, same, but for runs DB,
  "RunsDatabaseName":     {string} the name of the runs database,
  "Hostname":             {string} this is what you call your dispatcher
}

The environment variables MONGO_PASSWORD and RUNS_MONGO_PASSWORD must be set!
'''

def _all(values, target):
    ret = len(values) > 0
    for value in values:
        ret &= (value == target)
    return ret

class MongoConnect():

    def __init__(self, config, log):

        # Define DB connectivity. Log is separate to make it easier to split off if needed
        dbn = config['DEFAULT']['ControlDatabaseName']
        rdbn = config['DEFAULT']['RunsDatabaseName']
        self.dax_db = MongoClient(
            config['DEFAULT']['ControlDatabaseURI']%os.environ['MONGO_PASSWORD'])[dbn]
        self.runs_db = MongoClient(
            config['DEFAULT']['RunsDatabaseURI']%os.environ['RUNS_MONGO_PASSWORD'])[rdbn]

        self.latest_settings = {}

        self.loglevels = {"DEBUG": 0, "MESSAGE": 1, "WARNING": 2, "ERROR": 3, "FATAL": 4}

        # Each collection we actually interact with is stored here
        self.collections = {
            'incoming_commands': self.dax_db['detector_control'],
            'node_status': self.dax_db['status'],
            'aggregate_status': self.dax_db['aggregate_status'],
            'outgoing_commands': self.dax_db['control'],
            'log': self.dax_db['log'],
            'options': self.dax_db['options'],
            'run': self.runs_db[config['DEFAULT']['RunsDatabaseCollection']],
            'command_queue' : self.dax_db['dispatcher_queue'],
        }

        self.error_sent = {}

        # How often we should push certain types of errors (seconds)
        self.error_timeouts = {
            "ARM_TIMEOUT": 1, # 1=push all
            "START_TIMEOUT": 1,
            "STOP_TIMEOUT": 3600/4 # 15 minutes
        }
        # Timeout (in seconds). How long must a node not report to be considered timing out
        self.timeout = int(config['DEFAULT']['ClientTimeout'])

        # We will store the latest status from each reader here
        # Format:
        # {
        #    'tpc':   {
        #                'status': {enum},
        #                'mode': {string} run mode if any,
        #                'rate': {int} aggregate rate if any,
        #                'pulses': {int} pulse rate,
        #                'blt': {float} blt rate,
        #                'readers': {
        #                    'reader_0_reader_0': {
        #                           'status': {enum},
        #                           'checkin': {int},
        #                           'rate': {float},
        #                           'pulses': {int},
        #                           'blt': {int}
        #                     },
        #                 'controller': {}
        #                 }
        #  }
        self.latest_status = {}
        dc = json.loads(config['DEFAULT']['MasterDAQConfig'])
        for detector in dc.keys():
            self.latest_status[detector] = {'readers': {}, 'controller': {}}
            for reader in dc[detector]['readers']:
                self.latest_status[detector]['readers'][reader] = {}
            for controller in dc[detector]['controller']:
                if controller == "":
                    continue
                self.latest_status[detector]['controller'][controller] = {}

        self.command_oid = {k:{c:None} for c in ['start','stop','arm'] for k in self.latest_status.keys()}
        self.log = log
        self.run = True
        self.event = threading.Event()
        self.command_thread = threading.Thread(target=self.ProcessCommands)
        self.command_thread.start()

    def Quit(self):
        self.run = False
        try:
            self.event.set()
            self.command_thread.join()
        except:
            pass

    def __del__(self):
        self.Quit()

    def GetUpdate(self):

        latest = {}
        try:
            for detector in self.latest_status.keys():
                for host in self.latest_status[detector]['readers'].keys():
                    doc = self.collections['node_status'].find_one({'host': host},
                                                                   sort=[('_id', -1)])
                    self.latest_status[detector]['readers'][host] = doc
                for host in self.latest_status[detector]['controller'].keys():
                    doc = self.collections['node_status'].find_one({'host': host},
                                                                    sort=[('_id', -1)])
                    self.latest_status[detector]['controller'][host] = doc
        except Exception as e:
            print(type(e), e)
            return -1

        # Now compute aggregate status
        self.AggregateStatus()

    def ClearErrorTimeouts(self):
        self.error_sent = {}

    def UpdateAggregateStatus(self):
        '''
        Put current aggregate status into DB
        '''
        for detector in self.latest_status.keys():
            doc = {
                "status": self.latest_status[detector]['status'].value,
                "number": -1,
                "detector": detector,
                "rate": self.latest_status[detector]['rate'],
                "readers": len(self.latest_status[detector]['readers'].keys()),
                "time": datetime.datetime.utcnow(),
                "buff": self.latest_status[detector]['buffer'],
                "mode": self.latest_status[detector]['mode'],
            }
            if 'number' in self.latest_status[detector].keys():
                doc['number'] = self.latest_status[detector]['number']
            else:
                doc['number'] = None
            try:
                self.collections['aggregate_status'].insert(doc)
            except:
                self.log.error('RunsDB snafu')
                return

    def AggregateStatus(self):

        # Compute the total status of each detector based on the most recent updates
        # of its individual nodes. Here are some general rules:
        #  - Usually all nodes have the same status (i.e. 'running') and this is
        #    simply the aggregate
        #  - During changes of state (i.e. starting a run) some nodes might be faster
        #    than others. In this case the status can be 'unknown'. The main program should
        #    interpret whether 'unknown' is a reasonable thing, like was a command
        #    sent recently? If so then sure, a 'unknown' status will happpen.
        #  - If any single node reports error then the whole thing is in error
        #  - If any single node times out then the whole thing is in timeout

        whattimeisit = datetime.datetime.utcnow()
        for detector in self.latest_status.keys():
            status_list = []
            status = None
            rate = 0
            mode = None
            buff = 0
            for doc in self.latest_status[detector]['readers'].values():
                try:
                    rate += doc['rate']
                except:
                    pass
                try:
                    buff += doc['buffer'] + doc['strax_buffer']
                except:
                    pass

                if mode is None and 'mode' in doc.keys():
                    mode = doc['mode']
                elif 'mode' in doc.keys() and doc['mode'] != mode:
                    mode = 'undefined'

                try:
                    status = STATUS(doc['status'])
                except KeyError:
                    status = STATUS.UNKNOWN

                # Now check if this guy is timing out
                try:
                    if "time" in doc.keys() and (whattimeisit - doc['time']).seconds > self.timeout:
                        status = STATUS.TIMEOUT
                except:
                    status = STATUS.UNKNOWN

                status_list.append(status)

            # If we have a crate controller check on it too
            for doc in self.latest_status[detector]['controller'].values():
                # Copy above. I guess it would be possible to have no readers
                try:
                    status = STATUS(doc['status'])
                except KeyError:
                    status = STATUS.UNKNOWN
                try:
                    if "time" in doc.keys() and (whattimeisit - doc['time']).seconds > self.timeout:
                        status = STATUS.TIMEOUT
                except:
                    status = STATUS.UNKNOWN
                status_list.append(status)

            # Now we aggregate the statuses
            for stat in ['ARMING','ERROR','TIMEOUT','UNKNOWN']:
                if STATUS[stat] in status_list:
                    status = STATUS[stat]
                    break
            else:
                for stat in ['IDLE','ARMED','RUNNING']:
                    if _all(status_list, STATUS[stat]):
                        status = STATUS[stat]
                        break
                else:
                    status = STATUS['UNKNOWN']

            #if detector == 'tpc':
            #    self.log.debug("Status list for %s: %s = %s" % (
            #        detector, [x.name for x in status_list], status.name))
            if detector == 'neutron_veto':
                status = STATUS.IDLE

            self.latest_status[detector]['status'] = status
            self.latest_status[detector]['rate'] = rate
            self.latest_status[detector]['mode'] = mode
            self.latest_status[detector]['buffer'] = buff


    def GetWantedState(self):
        # Aggregate the wanted state per detector from the DB and return a dict
        try:
            for doc in self.collections['incoming_commands'].aggregate([
                {'$sort': {'_id': -1}},
                {'$group': {
                    '_id': {'$concat': ['$detector', '.', '$field']},
                    'value': {'$first': '$value'},
                    'user': {'$first': '$user'},
                    'time': {'$first': '$time'},
                    'detector': {'$first': '$detector'},
                    'key': {'$first': '$field'}
                    }},
                {'$group': {
                    '_id': '$detector',
                    'keys': {'$push': '$key'},
                    'values': {'$push': '$value'},
                    'users': {'$push': '$user'},
                    'times': {'$push': '$time'}
                    }},
                {'$project': {
                    'detector': '$_id',
                    '_id': 0,
                    'state': {'$arrayToObject': {'$zip': {'inputs': ['$keys', '$values']}}},
                    'user': {'$arrayElemAt': ['$users', {'$indexOfArray': ['$times', {'$max': '$times'}]}]}
                    }}
                ]):
                doc.update(doc['state'])
                del doc['state']
                self.latest_settings[doc['detector']]=doc
            return self.latest_settings
        except:
            return None

    def GetConfiguredNodes(self, detector, link_mv, link_nv):
        '''
        Get the nodes we want from the config file
        '''
        retnodes = []
        retcc = []
        retnodes = list(self.latest_status[detector]['readers'].keys())
        retcc = list(self.latest_status[detector]['controller'].keys())
        if detector == 'tpc' and link_nv == 'true':
            retnodes += list(self.latest_status['neutron_veto']['readers'].keys())
            retcc += list(self.latest_status['neutron_veto']['controllers'].keys())
        if detector == 'tpc' and link_mv == 'true':
            retnodes += list(self.latest_status['muon_veto']['readers'].keys())
            retcc += list(self.latest_status['muon_veto']['controllers'].keys())
        return retnodes, retcc

    def GetRunMode(self, mode):
        '''
        Pull a run doc from the options collection and add all the includes
        '''
        if mode is None:
            return None
        base_doc = self.collections['options'].find_one({'name': mode})
        if base_doc is None:
            self.LogError("dispatcher", "Mode '%s' doesn't exist" % mode, "info", "info")
            return None
        if 'includes' not in base_doc or len(base_doc['includes']) == 0:
            return base_doc
        try:
            if self.collections['options'].count_documents({'name':
                {'$in': base_doc['includes']}}) != len(base_doc['includes']):
                self.LogError("dispatcher", "At least one subconfig for mode '%s' doesn't exist" % mode, "warn", "warn"):
                return None
            return list(self.collections["options"].aggregate([
                {'$match': {'name': mode}},
                {'$lookup': {'from': 'options', 'localField': 'includes',
                    'foreignField': 'name', 'as': 'subconfig'}},
                {'$addFields': {'$concatArrays': ['$subconfig', ['$$ROOT']]}},
                {'$unwind': {'path': '$subconfig'}},
                {'$group': {'_id': None, 'config': {'$mergeObjects': '$subconfig'}}},
                {'$replaceWith': '$config'},
                {'$project': {'_id': 0, 'description': 0, 'includes': 0, 'subconfig': 0}},
                ]))[0]
        except Exception as e:
            self.log.error("Got a %s exception in doc pulling: %s" % (type(E), E))
        return None

    def GetHostsForMode(self, mode):
        '''
        Get the nodes we need from the run mode
        '''
        if mode is None:
            self.log.debug("Run mode is none?")
            return [], []
        doc = self.GetRunMode(mode)
        if doc is None:
            self.log.debug("No run mode?")
            return [], []
        cc = []
        hostlist = []
        for b in doc['boards']:
            if 'V17' in b['type'] and b['host'] not in hostlist:
                hostlist.append(b['host'])
            elif b['type'] == 'V2718':
                cc.append(b['host'])
        return hostlist, cc

    def GetNextRunNumber(self):
        try:
            cursor = self.collections["run"].find().sort("number", -1).limit(1)
        except:
            self.log.error('Database is having a moment?')
            return -1
        if cursor.count() == 0:
            self.log.info("wtf, first run?")
            return 0
        return list(cursor)[0]['number']+1

    def SetStopTime(self, number, detector, force):
        '''
        Sets the 'end' field of the run doc to the time when the STOP command was ack'd
        '''
        self.log.info("Updating run %i with end time"%number)
        try:
            time.sleep(2) # this number depends on the delay between CC and reader stop
            endtime = self.GetAckTime(detector, 'stop')
            if endtime is None:
                endtime = datetime.datetime.utcnow()-datetime.timedelta(seconds=1)
            query = {"number" : int(number), "end" : {"$exists" : False}}
            updates = {"$set" : {"end" : endtime}}
            if force:
                updates["$push"] = {"tags" : {"name" : "messy", "user" : "daq",
                    "date" : datetime.datetime.utcnow()}}
            self.collections['run'].update_one(query, updates)
        except:
            self.log.error("Database having a moment, hope this doesn't crash")
        return

    def GetAckTime(self, detector, command):
        '''
        Finds the time when specified detector's crate controller ack'd the specified command
        '''
        cc = list(self.latest_status[detector]['controller'].keys())[0]
        query = {'acknowledged.%s' % cc: {'$exists' : 1},
                 '_id' : self.command_oid[detector][command]}
        doc = self.collections['outgoing_commands'].find_one(query)
        if doc is not None:
            return datetime.datetime.utcfromtimestamp(doc['acknowledged'][cc]/1000)
        self.log.debug('No ACK time for %s-%s' % (detector, command))
        return None

    def SendCommand(self, command, hosts, user, detector, mode="", delay=0):
        '''
        Send this command to these hosts. If delay is set then wait that amount of time
        '''
        number = None
        n_id = None
        try:
            if command == 'arm':
                number = self.GetNextRunNumber()
                if number == -1:
                    return -1
                n_id = '%06i' % number
                self.latest_status[detector]['number'] = number
            doc_base = {
                "command": command,
                "user": user,
                "detector": detector,
                "mode": mode,
                "options_override": {"number": number},
                "number": number,
                "createdAt": datetime.datetime.utcnow()
            }
            if delay == 0:
                docs = doc_base
                docs['host'] = hosts[0]+hosts[1] if isinstance(hosts, tuple) else hosts
            else:
                docs = [dict(doc_base.items()), dict(doc_base.items())]
                docs[0]['host'], docs[1]['host'] = hosts
                docs[1]['createdAt'] += datetime.timedelta(seconds=delay)
            self.collections['command_queue'].insert(docs)
        except Exception as e:
            self.log.info('Database issue, dropping command %s to %s' % (command, detector))
            print(type(e), e)
            return -1
        else:
            self.log.debug('Queued %s for %s' % (command, detector))
            self.event.set()
        return 0

    def ProcessCommands(self):
        '''
        Process our internal command queue
        '''
        while self.run == True:
            try:
                next_cmd = self.collections['command_queue'].find_one({}, sort=[('createdAt', 1)])
                if next_cmd is None:
                    dt = 10
                else:
                    dt = (next_cmd['createdAt'] - datetime.datetime.utcnow()).total_seconds()
                if dt < 0.01:
                    oid = next_cmd['_id']
                    del next_cmd['_id']
                    ret = self.collections['outgoing_commands'].insert_one(next_cmd)
                    self.collections['command_queue'].delete_one({'_id' : oid})
                    self.command_oid[next_cmd['detector']][next_cmd['command']] = ret.inserted_id
            except Exception as e:
                dt = 10
                self.log.error("DB down? %s" % e)
            self.event.wait(dt)
            self.event.clear()

    def LogError(self, reporter, message, priority, etype):

        # Note that etype allows you to define timeouts.
        nowtime = datetime.datetime.utcnow()
        if ( (etype in self.error_sent and self.error_sent[etype] is not None) and
             (etype in self.error_timeouts and self.error_timeouts[etype] is not None) and 
             (nowtime-self.error_sent[etype]).total_seconds() <= self.error_timeouts[etype]):
            self.log.debug("Could log error, but still in timeout for type %s"%etype)
            return
        self.error_sent[etype] = nowtime
        try:
            self.collections['log'].insert({
                "user": reporter,
                "message": message,
                "priority": self.loglevels[priority]
            })
        except:
            self.log.error('Database error, can\'t issue error message')
        self.log.info("Error message from %s: %s" % (reporter, message))
        return

    def GetRunStart(self, number):
        try:
            doc = self.collections['run'].find_one({"number": number}, {"start": 1})
        except:
            self.log.error('Database is having a moment')
            return None
        if doc is not None and 'start' in doc:
            return doc['start']
        return None

    def InsertRunDoc(self, detector, goal_state):

        number = self.GetNextRunNumber()
        if number == -1:
            self.log.error("DB having a moment")
            return -1
        self.latest_status[detector]['number'] = number
        detectors = [detector]
        if detector == 'tpc' and goal_state['tpc']['link_nv'] == 'true':
            self.latest_status['neutron_veto']['number'] = number
            detectors.append('neutron_veto')
        if detector == 'tpc' and goal_state['tpc']['link_mv'] == 'true':
            self.latest_status['muon_veto']['number'] = number
            detectors.append('muon_veto')

        run_doc = {
            "number": number,
            'detectors': detectors,
            'user': goal_state[detector]['user'],
            'mode': goal_state[detector]['mode'],
        }

        # If there's a source add the source. Also add the complete ini file.
        cfg = self.GetRunMode(goal_state[detector]['mode'])
        if cfg is not None and 'source' in cfg.keys():
            run_doc['source'] = {'type': cfg['source']}
        run_doc['daq_config'] = cfg

        # If the user started the run with a comment add that too
        if "comment" in goal_state[detector] and goal_state[detector]['comment'] != "":
            run_doc['comments'] = [{
                "user": goal_state[detector]['user'],
                "date": datetime.datetime.utcnow(),
                "comment": goal_state[detector]['comment']
            }]

        # Make a data entry so bootstrax can find the thing
        if 'strax_output_path' in cfg:
            run_doc['data'] = [{
                'type': 'live',
                'host': 'daq',
                'location': cfg['strax_output_path']
            }]

        try:
            time.sleep(2)
            start_time = self.GetAckTime(detector, 'start')
            if start_time is None:
                start_time = datetime.datetime.utcnow()-datetime.timedelta(seconds=2)
            run_doc['start'] = start_time

            self.collections['run'].insert_one(run_doc)
        except:
            self.log.error('Database having a moment')
            return -1
        return number
