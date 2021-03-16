iquote_plus(os.environ['MONGO_PASSWORD_DAQ'])mport datetime
from daqnt import DAQ_STATUS
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

    def __init__(self, config, log, control_mc, runs_mc, hypervisor, testing=False):

        # Define DB connectivity. Log is separate to make it easier to split off if needed
        dbn = config['ControlDatabaseName']
        rdbn = config['RunsDatabaseName']
        self.dax_db = control_mc[dbn]
        self.runs_db = runs_mc[rdbn]
        self.hypervisor = hypervisor

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
            'run': self.runs_db[config['RunsDatabaseCollection']],
            'command_queue': self.dax_db['dispatcher_queue'],
        }

        self.error_sent = {}

        # How often we should push certain types of errors (seconds)
        self.error_timeouts = {
            "ARM_TIMEOUT": 1, # 1=push all
            "START_TIMEOUT": 1,
            "STOP_TIMEOUT": 3600/4 # 15 minutes
        }
        # Timeout (in seconds). How long must a node not report to be considered timing out
        self.timeout = int(config['ClientTimeout'])

        # How long a node can be timing out before it gets fixed (TPC only)
        self.timeout_take_action = int(config['TimeoutActionThreshold'])

        # Which control keys do we look for?
        self.control_keys = config['ControlKeys'].split()

        self.digi_type = 'V17' if not testing else 'f17'
        self.cc_type = 'V2718' if not testing else 'f2718'

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
        self.host_config = {}
        self.dc = config['MasterDAQConfig']
        for detector in self.dc:
            self.latest_status[detector] = {'readers': {}, 'controller': {}}
            for reader in self.dc[detector]['readers']:
                self.latest_status[detector]['readers'][reader] = {}
                self.host_config[reader] = detector
            for controller in self.dc[detector]['controller']:
                self.latest_status[detector]['controller'][controller] = {}
                self.host_config[controller] = detector

        self.command_oid = {d:{c:None} for c in ['start','stop','arm'] for d in self.dc}
        self.log = log
        self.run = True
        self.event = threading.Event()
        self.command_thread = threading.Thread(target=self.process_commands)
        self.command_thread.start()

    def quit(self):
        self.run = False
        try:
            self.event.set()
            self.command_thread.join()
        except:
            pass

    def __del__(self):
        self.quit()

    def get_update(self):

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
            self.log.error(f'Got error while getting update: {type(e)}: {e}')
            return True

        # Now compute aggregate status
        return self.aggregate_status() is not None

    def clear_error_timeouts(self):
        self.error_sent = {}

    def update_aggregate_status(self):
        '''
        Put current aggregate status into DB
        '''
        for detector in self.latest_status.keys():
            doc = {
                "status": self.latest_status[detector]['status'].value,
                "detector": detector,
                "rate": self.latest_status[detector]['rate'],
                "readers": len(self.latest_status[detector]['readers'].keys()),
                "time": datetime.datetime.utcnow(),
                "buff": self.latest_status[detector]['buffer'],
                "mode": self.latest_status[detector]['mode'],
                "pll_unlocks": self.latest_status[detector]["pll_unlocks"],
            }
            if 'number' in self.latest_status[detector].keys():
                doc['number'] = self.latest_status[detector]['number']
            else:
                doc['number'] = None
            try:
                self.collections['aggregate_status'].insert(doc)
            except Exception as e:
                self.log.error('RunsDB snafu')
                self.log.debug(f'That snafu was {type(e)} {str(e)}')
                return

    def aggregate_status(self):

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

        now = time.time()
        ret = None
        for detector in self.latest_status.keys():
            statuses = {}
            status = None
            mode = 'none'
            rate = 0
            buff = 0
            pll = 0
            run_num = -1
            for doc in self.latest_status[detector]['readers'].values():
                try:
                    rate += doc['rate']
                    buff += doc['buffer_size']
                    pll += doc.get('pll', 0)
                except Exception as e:
                    # This is not really important it's nice if we have
                    # it but not essential.
                    self.log.debug(f'Rate calculation ran into {type(e)}')
                    pass

                try:
                    status = DAQ_STATUS(doc['status'])
                    dt = (now - int(str(doc['_id'])[:8], 16))
                    if dt > self.timeout:
                        self.log.debug(f'{doc["host"]} reported {int(dt)} sec ago')
                        status = DAQ_STATUS.TIMEOUT
                        if self.host_config[doc['host']] == 'tpc':
                            if (dt > self.timeout_take_action or
                                    ((ts := self.host_ackd_command(doc['host'])) is not None and
                                     ts-now > self.timeout)):
                                self.log.info(f'{doc["host"]} is getting restarted')
                                self.hypervisor.handle_timeout(doc['host'])
                                ret = 1
                except Exception as e:
                    status = DAQ_STATUS.UNKNOWN

                statuses[doc['host']] = status

            # If we have a crate controller check on it too
            for doc in self.latest_status[detector]['controller'].values():
                # Copy above. I guess it would be possible to have no readers
                try:
                    mode = doc['mode']
                    status = DAQ_STATUS(doc['status'])

                    dt = (now - int(str(doc['_id'])[:8], 16))
                    doc['last_checkin'] = dt
                    if dt > self.timeout:
                        self.log.debug(f'{doc["host"]} reported {int(dt)} sec ago')
                        status = DAQ_STATUS.TIMEOUT
                        if self.host_config[doc['host']] == 'tpc':
                            if (dt > self.timeout_take_action or
                                    ((ts := self.host_ackd_command(doc['host'])) is not None and
                                     ts-now > self.timeout)):
                                self.log.info(f'{doc["host"]} is getting restarted')
                                self.hypervisor.handle_timeout(doc['host'])
                                ret = 1
                except Exception as e:
                    self.log.debug(f'Setting status to unknown because of {type(e)}: {e}')
                    status = DAQ_STATUS.UNKNOWN

                statuses[doc['host']] = status
                mode = doc.get('mode', 'none')
                run_num = doc.get('number', -1)

            if mode != 'none': # readout is "active":
                a,b = self.get_hosts_for_mode(mode)
                active = a + b
                status_list = [v for k,v in statuses.items() if k in active]
            else:
                status_list = list(statuses.values())

            # Now we aggregate the statuses
            for stat in ['ARMING','ERROR','TIMEOUT','UNKNOWN']:
                if DAQ_STATUS[stat] in status_list:
                    status = DAQ_STATUS[stat]
                    break
            else:
                for stat in ['IDLE','ARMED','RUNNING']:
                    if _all(status_list, DAQ_STATUS[stat]):
                        status = DAQ_STATUS[stat]
                        break
                else:
                    status = DAQ_STATUS.UNKNOWN

            self.latest_status[detector]['status'] = status
            self.latest_status[detector]['rate'] = rate
            self.latest_status[detector]['mode'] = mode
            self.latest_status[detector]['buffer'] = buff
            self.latest_status[detector]['number'] = run_num
            self.latest_status[detector]['pll_unlocks'] = pll

        return ret


    def get_wanted_state(self):
        # Aggregate the wanted state per detector from the DB and return a dict
        try:
            latest_settings = {}
            for detector in 'tpc muon_veto neutron_veto'.split():
                latest = None
                latest_settings[detector] = {}
                for key in self.control_keys:
                    doc = self.collections['incoming_commands'].find_one(
                            {'key': f'{detector}.{key}'}, sort=[('_id', -1)])
                    if doc is None:
                        self.log.error('No key %s for %s???' % (key, detector))
                        return None
                    latest_settings[detector][doc['field']] = doc['value']
                    if latest is None or doc['time'] > latest:
                        latest = doc['time']
                        latest_settings[detector]['user'] = doc['user']
            self.latest_settings = latest_settings
            return self.latest_settings
        except Exception as e:
            self.log.debug(f'get_wanted_state failed due to {type(e)} {e}')
            return None

    def get_configured_nodes(self, detector, link_mv, link_nv):
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

    def get_run_mode(self, mode):
        '''
        Pull a run doc from the options collection and add all the includes
        '''
        if mode is None:
            return None
        base_doc = self.collections['options'].find_one({'name': mode})
        if base_doc is None:
            self.log_error("dispatcher", "Mode '%s' doesn't exist" % mode, "info", "info")
            return None
        if 'includes' not in base_doc or len(base_doc['includes']) == 0:
            return base_doc
        try:
            if self.collections['options'].count_documents({'name':
                {'$in': base_doc['includes']}}) != len(base_doc['includes']):
                self.log_error("dispatcher", "At least one subconfig for mode '%s' doesn't exist" % mode, "warn", "warn")
                return None
            return list(self.collections["options"].aggregate([
                {'$match': {'name': mode}},
                {'$lookup': {'from': 'options', 'localField': 'includes',
                    'foreignField': 'name', 'as': 'subconfig'}},
                {'$addFields': {'subconfig': {'$concatArrays': ['$subconfig', ['$$ROOT']]}}},
                {'$unwind': '$subconfig'},
                {'$group': {'_id': None, 'config': {'$mergeObjects': '$subconfig'}}},
                {'$replaceWith': '$config'},
                {'$project': {'_id': 0, 'description': 0, 'includes': 0, 'subconfig': 0}},
                ]))[0]
        except Exception as e:
            self.log.error("Got a %s exception in doc pulling: %s" % (type(e), e))
        return None

    def get_hosts_for_mode(self, mode):
        '''
        Get the nodes we need from the run mode
        '''
        if mode is None:
            self.log.debug("Run mode is none?")
            return [], []
        doc = self.get_run_mode(mode)
        if doc is None:
            self.log.debug("No run mode?")
            return [], []
        cc = []
        hostlist = []
        for b in doc['boards']:
            if self.digi_type in b['type'] and b['host'] not in hostlist:
                hostlist.append(b['host'])
            elif b['type'] == self.cc_type and b['host'] not in cc:
                cc.append(b['host'])
        return hostlist, cc

    def get_next_run_number(self):
        try:
            cursor = self.collections["run"].find({},{'number': 1}).sort("number", -1).limit(1)
        except Exception as e:
            self.log.error(f'Database is having a moment? {type(e)}, {e}')
            return -1
        if cursor.count() == 0:
            self.log.info("wtf, first run?")
            return 0
        return list(cursor)[0]['number']+1

    def set_stop_time(self, number, detectors, force):
        '''
        Sets the 'end' field of the run doc to the time when the STOP command was ack'd
        '''
        self.log.info(f"Updating run {number} with end time ({detectors})")
        try:
            time.sleep(0.5) # this number depends on the CC command polling time
            endtime = self.get_ack_time(detectors, 'stop')
            if endtime is None:
                self.logger.debug(f'No end time found for run {number}')
                endtime = datetime.datetime.utcnow()-datetime.timedelta(seconds=1)
            query = {"number": int(number), "end": None, 'detectors': detectors}
            updates = {"$set": {"end": endtime}}
            if force:
                updates["$push"] = {"tags": {"name": "_messy", "user": "daq",
                    "date": datetime.datetime.utcnow()}}
            if self.collections['run'].update_one(query, updates).modified_count == 1:
                self.log.debug('Update successful')
                rate = {}
                for doc in self.collections['aggregate_status'].aggregate([
                    {'$match': {'number': number}},
                    {'$group': {'_id': '$detector',
                                'avg': {'$avg': '$rate'},
                                'max': {'$max': '$rate'}}}
                    ]):
                    rate[doc['_id']] = {'avg': doc['avg'], 'max': doc['max']}
                self.collections['run'].update_one({'number': int(number)},
                                                   {'$set': {'rate': rate}})
            else:
                self.log.debug('No run updated?')
        except Exception as e:
            self.log.error(f"Database having a moment, hope this doesn't crash. {type(e)}, {e}")
        return

    def get_ack_time(self, detector, command):
        '''
        Finds the time when specified detector's crate controller ack'd the specified command
        '''
        cc = list(self.latest_status[detector]['controller'].keys())[0]
        query = {f'acknowledged.{cc}': {'$ne': 0},
                 '_id': self.command_oid[detector][command]}
        doc = self.collections['outgoing_commands'].find_one(query)
        if doc is not None and not isinstance(doc['acknowledged'][cc], int):
            return doc['acknowledged'][cc]
        self.log.debug(f'No ACK time for {detector}-{command}')
        return None

    def send_command(self, command, hosts, user, detector, mode="", delay=0, force=False):
        '''
        Send this command to these hosts. If delay is set then wait that amount of time
        '''
        number = None
        if command == 'stop' and not self.detector_ackd_command(detector, 'stop'):
            self.log.error(f"{detector} hasn't ack'd its last stop, let's not flog a dead horse")
            if not force:
                return 1
        try:
            if command == 'arm':
                number = self.get_next_run_number()
                if number == -1:
                    return -1
                self.latest_status[detector]['number'] = number
            doc_base = {
                "command": command,
                "user": user,
                "detector": detector,
                "mode": mode,
                "createdAt": datetime.datetime.utcnow()
            }
            if command == 'arm':
                doc_base['options_override'] = {'number': number}
            if delay == 0:
                docs = doc_base
                docs['host'] = hosts[0]+hosts[1] if isinstance(hosts, tuple) else hosts
                docs['acknowledged'] = {h:0 for h in docs['host']}
            else:
                docs = [dict(doc_base.items()), dict(doc_base.items())]
                docs[0]['host'], docs[1]['host'] = hosts
                docs[0]['acknowledged'] = {h:0 for h in docs[0]['host']}
                docs[1]['acknowledged'] = {h:0 for h in docs[1]['host']}
                docs[1]['createdAt'] += datetime.timedelta(seconds=delay)
            self.collections['command_queue'].insert(docs)
        except Exception as e:
            self.log.info(f'Database issue, dropping command {command} to {detector}')
            self.log.debug(f'SendCommand ran into {type(e)}, {e})')
            return -1
        else:
            self.log.debug(f'Queued {command} for {detector}')
            self.event.set()
        return 0

    def process_commands(self):
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
                    oid = next_cmd.pop('_id')
                    ret = self.collections['outgoing_commands'].insert_one(next_cmd)
                    self.collections['command_queue'].delete_one({'_id': oid})
                    self.command_oid[next_cmd['detector']][next_cmd['command']] = ret.inserted_id
            except Exception as e:
                dt = 10
                self.log.error(f"DB down? {type(e)}, {e}")
            self.event.wait(dt)
            self.event.clear()

    def host_ackd_command(self, host):
        """
        Finds the timestamp of the most recent unacknowledged command send to the specified host
        :param host: str, the process name to check
        :returns: float, the timestamp of the last unack'd command, or None if none exist
        """
        q = {f'acknowledged.{host}': 0}
        if (doc := self.collections['outgoing_commands'].find_one(q, sort=[('_id', 1)])) is None:
            return None
        return doc['createdAt'].timestamp()

    def detector_ackd_command(self, detector, command=None):
        """
        Finds when the specified/most recent command was ack'd
        """
        if (oid := self.command_oid[detector][command]) is None:
            return True
        if (doc := self.collections['outoing_commands'].find_one({'_id': oid})) is None:
            self.log.error('No previous command found?')
            return True
        for h in doc['host']:
            # loop over doc['host'] because the 'acknowledged' field sometimes
            # contains extra entries (such as the GPS trigger)
            if doc['acknowledged'][h] == 0:
                return False
        return True

    def log_error(self, message, priority, etype):

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
                "user": "dispatcher",
                "message": message,
                "priority": self.loglevels[priority]
            })
        except:
            self.log.error('Database error, can\'t issue error message')
        self.log.info("Error message from dispatcher: %s" % (message))
        return

    def get_run_start(self, number):
        try:
            doc = self.collections['run'].find_one({"number": number}, {"start": 1})
        except Exception as e:
            self.log.error(f'Database is having a moment: {type(e)}, {e}')
            return None
        if doc is not None and 'start' in doc:
            return doc['start']
        return None

    def insert_run_doc(self, detector, goal_state):

        if (number := self.get_next_run_number()) == -1:
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
            'bootstrax': {'state': None},
            'end': None
        }

        # If there's a source add the source. Also add the complete ini file.
        cfg = self.get_run_mode(goal_state[detector]['mode'])
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
            start_time = self.get_ack_time(detector, 'start')
            if start_time is None:
                start_time = datetime.datetime.utcnow()-datetime.timedelta(seconds=2)
                run_doc['tags'] = [{'name': 'messy', 'user': 'daq', 'date': start_time}]
            run_doc['start'] = start_time

            self.collections['run'].insert_one(run_doc)
        except Exception as e:
            self.log.error(f'Database having a moment: {type(e)}, {e}')
            return -1
        return number

