from pymongo import MongoClient
import datetime
import os
import json
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

class MongoConnect():

    def __init__(self, config, log):

        # Define DB connectivity. Log is separate to make it easier to split off if needed
        dbn = config['DEFAULT']['ControlDatabaseName']
        rdbn = config['DEFAULT']['RunsDatabaseName']
        print("Initializing with DB name %s"%dbn)
        self.dax_db = MongoClient(
            config['DEFAULT']['ControlDatabaseURI']%os.environ['MONGO_PASSWORD'])[dbn]
        self.log_db = MongoClient(
            config['DEFAULT']['ControlDatabaseURI']%os.environ['MONGO_PASSWORD'])[dbn]
        self.runs_db = MongoClient(
            config['DEFAULT']['RunsDatabaseURI']%os.environ['RUNS_MONGO_PASSWORD'])[rdbn]

        self.latest_settings = {}

        # Translation to human-readable statuses
        self.statuses = ['Idle', 'Arming', 'Armed', 'Running', 'Error', 'Timeout', 'Unknown']
        self.st = dict(zip(self.statuses, range(len(self.statuses))))
        self.loglevels = {"DEBUG": 0, "MESSAGE": 1, "WARNING": 2, "ERROR": 3, "FATAL": 4}

        # Each collection we actually interact with is stored here
        self.collections = {
            'incoming_commands': self.dax_db['detector_control'],
            'node_status': self.dax_db['status'],
            'aggregate_status': self.dax_db['aggregate_status'],
            'outgoing_commands': self.dax_db['control'],
            'log': self.dax_db['log'],
            'options': self.dax_db['options'],
            'run': self.runs_db[config['DEFAULT']['RunsDatabaseCollection']]
        }

        self.outgoing_commands = []
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

        self.log = log

    def GetUpdate(self):

        # Get updates from readers and controller
        for detector in self.latest_status.keys():
            for reader in self.latest_status[detector]['readers'].keys():
                try:
                    doc = list(self.collections['node_status'].find(
                        {"host": reader}).sort("_id", -1).limit(1))[0]
                    self.latest_status[detector]['readers'][reader] = doc
                except Exception as e:
                    # no doc found. don't crash but should fail at aggregate status step
                    continue

            for controller in self.latest_status[detector]['controller'].keys():
                try:
                    doc = list(self.collections['node_status'].find(
                        {"host": controller}).sort("_id", -1).limit(1))[0]
                    self.latest_status[detector]['controller'][controller] = doc
                except:
                    continue

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
                "status": self.latest_status[detector]['status'],
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
            self.collections['aggregate_status'].insert(doc)

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

        whattimeisit = datetime.datetime.utcnow().timestamp()
        for detector in self.latest_status.keys():
            status_list = []
            status = None
            rate = 0
            mode = None
            buff = 0
            #print(self.latest_status)
            for reader in self.latest_status[detector]['readers'].keys():
                doc = self.latest_status[detector]['readers'][reader]
                try:
                    rate += doc['rate']
                except:
                    rate += 0.
                try:
                    buff += doc['buffer_length']
                except:
                    buff += 0

                if mode == None and 'run_mode' in doc.keys():
                    mode = doc['run_mode']
                elif 'run_mode' in doc.keys() and doc['run_mode'] != mode:
                    mode = 'undefined'

                try:
                    status = doc['status']
                except KeyError:
                    status = 6

                # Now check if this guy is timing out
                if "_id" in doc.keys():
                    gentime = doc['_id'].generation_time.timestamp()
                    if (whattimeisit - gentime) > self.timeout:
                        status = 5

                status_list.append(status)

            # If we have a crate controller check on it too
            for controller in self.latest_status[detector]['controller'].keys():
                doc = self.latest_status[detector]['controller'][controller]
                # Copy above. I guess it would be possible to have no readers
                try:
                    status = doc['status']
                except KeyError:
                    status = 6
                if "_id" in doc.keys():
                    gentime = doc['_id'].generation_time.timestamp()
                    if (whattimeisit-gentime) > self.timeout:
                        status = 5
                status_list.append(status)

            # Now we aggregate the statuses
            for stat in ['Error','Timeout','Unknown']:
                if self.st[stat] in status_list:
                    status = self.st[stat]
                    break
            else:
                for stat in ['Idle','Arming','Armed','Running']:
                    if all([self.st[stat] == x for x in status_list]):
                        status = self.st[stat]
                    break
                else:
                    status = self.st['Unknown']

            self.log.debug("Status list for %s: %s = %s" % (detector, status_list, status))

            self.latest_status[detector]['status'] = status
            self.latest_status[detector]['rate'] = rate
            self.latest_status[detector]['mode'] = mode
            self.latest_status[detector]['buffer'] = buff


    def GetWantedState(self):
        # Pull the wanted state per detector from the DB and return a dict
        retdoc = {}
        for detector in self.latest_status.keys():
            command = self.collections['incoming_commands'].find_one(
                {"detector": detector})
            if command is None:
                print("Error! Wanted to find command for detector %s but it isn't there"%detector)
            retdoc[detector] = command
        self.latest_settings = retdoc
        return retdoc

    def GetConfiguredNodes(self, detector, link_mv, link_nv):
        '''
        Get the nodes we want from the config file
        '''
        retnodes = []
        retcc = []
        retnodes = [r for r in self.latest_status[detector]['readers'].keys()]
        retcc = [r for r in self.latest_status[detector]['controller'].keys()]
        if detector == 'tpc' and self.latest_settings[detector]['link_nv'] == 'true':
            for r in self.latest_status['neutron_veto']['readers'].keys():
                retnodes.append(r)
            for r in self.latest_status['neutron_veto']['controller'].keys():
                retcc.append(r)
        if detector == 'tpc' and self.latest_settings[detector]['link_mv'] == 'true':
            for r in self.latest_status['muon_veto']['readers'].keys():
                retnodes.append(r)
            for r in self.latest_status['muon_veto']['controller'].keys():
                retcc.append(r)
        self.log.debug("Nodes: %s" % retnodes)
        self.log.debug("CCs: %s" % retcc)
        return retnodes, retcc

    def GetRunMode(self, mode):
        '''
        Pull a run doc from the options collection and add all the includes
        '''
        if mode is None:
            return None
        doc = self.collections["options"].find_one({"name": mode})
        fields_to_exclude = ['name', 'detector', 'description', 'user', '_id']
        try:
            newdoc = {**dict(doc)}
            if "includes" in doc.keys():
                for i in doc['includes']:
                    incdoc = self.collections["options"].find_one({"name": i})
                    for field in fields_to_exclude:
                        if field in incdoc:
                            del incdoc[field]
                    newdoc.update(incdoc)
            return newdoc
        except Exception as E:
            # LOG ERROR
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
        #self.log.debug("Hosts %s, cc %s" % (hostlist, cc))
        return hostlist, cc

    def GetNextRunNumber(self):
        cursor = self.collections["run"].find().sort("number", -1).limit(1)
        if cursor.count() == 0:
            self.log.info("wtf, first run?")
            return 0
        return list(cursor)[0]['number']+1

    def SetStopTime(self, number):
        '''
        Set's the 'end' field of the run doc to the current time if not done yet
        '''
        self.log.info("Updating run %i with end time"%number)
        self.collections['run'].update_one({"number": int(number), "end": {"$exists": False}},
                                          {"$set": {"end": datetime.datetime.utcnow()}})

    def SendCommand(self, command, host_list, user, detector, mode="", delay=0):
        '''
        Send this command to these hosts. If delay is set then wait that amount of time
        '''
        #self.log.debug("SEND COMMAND %s to %s"%(command, detector))
        number = None
        n_id = None
        if command == 'arm':
            number = self.GetNextRunNumber()
            n_id = (str(number)).zfill(6)
            self.latest_status[detector]['number'] = number
        self.outgoing_commands.append({
            "command": command,
            "user": user,
            "detector": detector,
            "mode": mode,
            "options_override": {"run_identifier": n_id},
            "number": number,
            "host": host_list,
            "createdAt": datetime.datetime.utcnow() + datetime.timedelta(seconds=delay)
        })
        return

    def ProcessCommands(self):
        '''
        Process our internal command queue
        '''
        nowtime = datetime.datetime.utcnow()
        afterlist = []
        for command in self.outgoing_commands:
            if command['createdAt'] <= nowtime:
                self.collections['outgoing_commands'].insert(command)
            else:
                afterlist.append(command)
        self.outgoing_commands = afterlist
        return

    def LogError(self, reporter, message, priority, etype):

        # Note that etype allows you to define timeouts.
        nowtime = datetime.datetime.utcnow()
        if ( (etype in self.error_sent and self.error_sent[etype] is not None) and
             (etype in self.error_timeouts and self.error_timeouts[etype] is not None) and 
             (nowtime-self.error_sent[etype]).total_seconds() <= self.error_timeouts[etype]):
            self.log.debug("Could log error, but still in timeout for type %s"%etype)
            return
        self.error_sent[etype] = nowtime
        self.collections['log'].insert({
            "user": reporter,
            "message": message,
            "priority": self.loglevels[priority]
        })
        self.log.info("Error message from %s: %s" % (reporter, message))
        return

    def GetRunStart(self, number):
        doc = self.collections['run'].find_one({"number": number}, {"start": 1})
        if doc is not None:
            return doc['start']
        return None

    def InsertRunDoc(self, detector, goal_state):

        number = self.GetNextRunNumber()
        self.latest_status[detector]['number'] = number
        if detector == 'tpc' and goal_state[detector]['link_nv']:
            self.latest_status['neutron_veto']['number'] = number
        if detector == 'tpc' and goal_state[detector]['link_mv']:
            self.latest_status['muon_veto']['number'] = number

        run_doc = {
            "number": number,
            'detector': detector,
            'user': goal_state[detector]['user'],
            'mode': goal_state[detector]['mode'],
        }

        # If there's a source add the source. Also add the complete ini file.
        ini = self.GetRunMode(goal_state[detector]['mode'])
        if ini is not None and 'source' in ini.keys():
            run_doc['source'] = {'type': ini['source']}
        run_doc['ini'] = ini

        # If the user started the run with a comment add that too
        if "comment" in goal_state[detector] and goal_state[detector]['comment'] != "":
            run_doc['comments'] = [{
                "user": goal_state[detector]['user'],
                "date": datetime.datetime.utcnow(),
                "comment": goal_state[detector]['comment']
            }]

        # Make a data entry so bootstrax can find the thing
        if 'strax_output_path' in ini:
            run_doc['data'] = [{
                'type': 'live',
                'host': 'daq',
                'location': ini['strax_output_path']
            }]

        # Lastly, update the start time. It is possible that one day someone will come looking
        # to see exactly how our event times get defined. This line shows you why we have a GPS
        # clock and why you shouldn't trust the event times from strax to be relatable to the
        # outside world to any degree of precision without invoking said GPS time
        run_doc['start'] = datetime.datetime.utcnow()

        self.collections['run'].insert_one(run_doc)
        return number
