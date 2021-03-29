import datetime
from daqnt import DAQ_STATUS
import threading
import time
import pytz

"""
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
"""

def _all(values, target):
    ret = len(values) > 0
    for value in values:
        if value != target:
            return False
    return True

def now():
    return datetime.datetime.now(pytz.utc)
    #return datetime.datetime.utcnow() # wrong?

# Communicate between various parts of dispatcher that no new run was determined
NO_NEW_RUN = -1


def now():
    return datetime.datetime.now(pytz.utc)

class MongoConnect():

    def __init__(self, config, daq_config, log, control_mc, runs_mc, hypervisor, testing=False):

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

        # How long a node can be timing out or missed an ack before it gets fixed (TPC only)
        self.timeout_take_action = int(config['TimeoutActionThreshold'])

        # how long to give the CC to start the run
        self.cc_start_wait = int(config['StartCmdDelay']) + 1

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
        #                'readers': {
        #                    'reader_0_reader_0': {
        #                           'status': {enum},
        #                           'rate': {float},
        #                     },
        #                 'controller': {}
        #                 }
        #  }
        self.latest_status = {}
        self.host_config = {}
        self.dc = daq_config
        for detector in self.dc:
            self.latest_status[detector] = {'readers': {}, 'controller': {}}
            for reader in self.dc[detector]['readers']:
                self.latest_status[detector]['readers'][reader] = {}
                self.host_config[reader] = detector
            for controller in self.dc[detector]['controller']:
                self.latest_status[detector]['controller'][controller] = {}
                self.host_config[controller] = detector

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

    def get_update(self, dc):
        """
        Gets the latest documents from the database for
        each node we know about
        """
        try:
            for detector in dc.keys():
                for host in dc[detector]['readers'].keys():
                    doc = self.collections['node_status'].find_one({'host': host},
                                                                   sort=[('_id', -1)])
                    dc[detector]['readers'][host] = doc
                for host in dc[detector]['controller'].keys():
                    doc = self.collections['node_status'].find_one({'host': host},
                                                                    sort=[('_id', -1)])
                    dc[detector]['controller'][host] = doc
        except Exception as e:
            self.log.error(f'Got error while getting update: {type(e)}: {e}')
            return True

        self.latest_status = dc

        # Now compute aggregate status
        return self.aggregate_status() is not None

    def clear_error_timeouts(self):
        self.error_sent = {}

    def aggregate_status(self):
        """
        Compute the total status of each "detector" based on the most recent
        updates of its individual nodes. Here are some general rules:
         - Usually all nodes have the same status (i.e. 'running') and this is
           not very complicated
         - During changes of state (i.e. starting a run) some nodes might
           be faster than others. In this case the status can be 'unknown'.
           The main program should interpret whether 'unknown' is a reasonable
           thing, like was a command sent recently? If so then sure, a 'unknown'
           status will happpen.
         - If any single node reports error then the whole thing is in error
         - If any single node times out then the whole thing is in timeout
         - Rates, buffer usage, and PLL counters only apply to the physical
           detector, not the logical detector, while status and run number
           apply to both
        """
        time_time = time.time()
        ret = None
        aggstat = {
                k:{ 'status': -1,
                    'detector': k,
                    'rate': 0,
                    'time': now(),
                    'buff': 0,
                    'mode': None,
                    'pll_unlocks': 0,
                    'number': -1}
                for k in self.dc}
        for detector in self.latest_status.keys():
            # detector = logical
            statuses = {}
            status = None
            modes = []
            run_nums = []
            for doc in self.latest_status[detector]['readers'].values():
                phys_det = self.host_config[doc['host']]
                try:
                    aggstat[phys_det]['rate'] += doc['rate']
                    aggstat[phys_det]['buff'] += doc['buffer_size']
                    aggstat[phys_det]['pll_unlocks'] += doc.get('pll', 0)
                except Exception as e:
                    # This is not really important but it's nice if we have it
                    self.log.debug(f'Rate calculation ran into {type(e)}: {e}')
                    pass

                try:
                    status = DAQ_STATUS(doc['status'])
                    if self.is_timeout(doc, now):
                        self.status = DAQ_STATUS.TIMEOUT
                except Exception as e:
                    self.log.debug(f'Ran into {type(e)}, daq is in timeout. {e}')
                    status = DAQ_STATUS.UNKNOWN

                statuses[doc['host']] = status

            for doc in self.latest_status[detector]['controller'].values():
                phys_det = self.host_config[doc['host']]
                try:
                    status = DAQ_STATUS(doc['status'])

                    if self.is_timeout(doc, now):
                        status = DAQ_STATUS.TIMEOUT
                except Exception as e:
                    self.log.debug(f'Setting status to unknown because of {type(e)}: {e}')
                    status = DAQ_STATUS.UNKNOWN

                statuses[doc['host']] = status
                modes.append(doc.get('mode', 'none'))
                run_nums.append(doc.get('number', None))
                aggstat[phys_det]['status'] = status
                aggstat[phys_det]['mode'] = modes[-1]
                aggstat[phys_det]['number'] = run_nums[-1]

            mode = modes[0]
            run_num = run_nums[0]
            if not _all(modes, mode):
                self.log.error(f'Got differing modes: {modes}')
                # TODO handle better?
                ret = 1
                continue
            if not _all(run_nums, run_num):
                self.log.error(f'Got differing run numbers: {run_nums}')
                # TODO handle better?
                ret = 1
                continue

            if mode != 'none': # readout is "active":
                a,b = self.get_hosts_for_mode(mode)
                active = a + b
                status_list = [v for k,v in statuses.items() if k in active]
            else:
                status_list = list(statuses.values())

            # Now we aggregate the statuses
            # First, the "or" statuses
            for stat in ['ARMING','ERROR','TIMEOUT','UNKNOWN']:
                if DAQ_STATUS[stat] in status_list:
                    status = DAQ_STATUS[stat]
                    break
            else:
                # then the "and" statuses
                for stat in ['IDLE','ARMED','RUNNING']:
                    if _all(status_list, DAQ_STATUS[stat]):
                        status = DAQ_STATUS[stat]
                        break
                else:
                    # otherwise
                    status = DAQ_STATUS.UNKNOWN

            self.latest_status[detector]['status'] = status
            self.latest_status[detector]['number'] = run_num
            self.latest_status[detector]['mode'] = mode

        try:
            self.collections['aggregate_status'].insert_many(aggstat.values())
        except Exception as e:
            self.log.error(f'DB snafu? Couldn\'t update aggregate status. '
                            f'{type(e)}, {e}')
        return ret

    def is_timeout(self, doc, t):
        """
        Checks to see if the specified status doc corresponds to a timeout situation
        """
        host = doc['host']
        dt = t - int(str(doc['_id'])[:8], 16)
        has_ackd = self.host_ackd_command(host)
        ret = False
        if dt > self.timeout:
            self.log.debug(f'{host} last reported {int(dt)} sec ago')
            ret |= True
        if has_ackd is not None and t - has_ackd > self.timeout_take_action:
            self.log.debug(f'{host} hasn\'t ackd a command from {int(t-has_ackd)} sec ago')
            if self.host_config[host] == 'tpc':
                self.hypervisor.handle_timeout(host)
            ret |= True
        return ret

    def get_wanted_state(self):
        """
        Figure out what the system is supposed to be doing right now
        """
        try:
            latest_settings = {}
            for detector in self.dc:
                latest = None
                latest_settings[detector] = {}
                for key in self.control_keys:
                    doc = self.collections['incoming_commands'].find_one(
                            {'key': f'{detector}.{key}'}, sort=[('_id', -1)])
                    if doc is None:
                        self.log.error(f'No key {key} for {detector}???')
                        return None
                    latest_settings[detector][doc['field']] = doc['value']
                    if latest is None or doc['time'] > latest:
                        latest = doc['time']
                        latest_settings[detector]['user'] = doc['user']
            self.goal_state = latest_settings
            return self.goal_state
        except Exception as e:
            self.log.debug(f'get_wanted_state failed due to {type(e)} {e}')
            return None

    def is_linked(self, a, b):
        """
        Check if the detectors are in a compatible linked configuration.
        """
        mode_a = self.goal_state[a]["mode"]
        mode_b = self.goal_state[b]["mode"]
        doc_a = self.collections['options'].find_one({'name': mode_a})
        doc_b = self.collections['options'].find_one({'name': mode_b})
        detectors_a = doc_a['detector']
        detectors_b = doc_b['detector']

        # Check if the linked detectors share the same run mode and
        # if they are both present in the detectors list of that mode
        return mode_a == mode_b and a in detectors_b and b in detectors_a

    def get_super_detector(self):
        """
        Get the Super Detector configuration
        if the detectors are in a compatible linked mode.
        - case A: tpc, mv and nv all linked
        - case B: tpc, mv and nv all un-linked
        - case C: tpc and mv linked, nv un-linked
        - case D: tpc and nv linked, mv un-linked
        - case E: tpc unlinked, mv and nv linked
        We will check the compatibility of the linked mode for a pair of detectors per time.
        """
        ret = {'tpc': {'controller': list(self.dc['tpc']['controller'].keys()),
                       'readers': list(self.dc['tpc']['readers'].keys()),
                       'detectors': ['tpc']}}
        mv = self.dc['muon_veto']
        nv = self.dc['neutron_veto']

        tpc_mv = self.is_linked('tpc', 'muon_veto')
        tpc_nv = self.is_linked('tpc', 'neutron_veto')
        mv_nv = self.is_linked('muon_veto', 'neutron_veto')

        # tpc and muon_veto linked mode
        if tpc_mv:
            # case A or C
            ret['tpc']['controller'] += list(mv['controller'].keys())
            ret['tpc']['readers'] += list(mv['readers'].keys())
            ret['tpc']['detectors'] += ['muon_veto']
        else:
            # case B or E
            ret['muon_veto'] = {'controller': list(mv['controller'].keys()),
                    'readers': list(mv['readers'].keys()),
                    'detectors': ['muon_veto']}
        if tpc_nv:
            # case A or D
            ret['tpc']['controller'] += list(nv['controller'].keys())
            ret['tpc']['readers'] += list(nv['readers'].keys())
            ret['tpc']['detectors'] += ['neutron_veto']
        elif mv_nv and not tpc_mv:
            # case E
            ret['muon_veto']['controller'] += list(nv['controller'].keys())
            ret['muon_veto']['readers'] += list(nv['readers'].keys())
            ret['muon_veto']['detectors'] += ['neutron_veto']
        else:
            # case B or C
            ret['neutron_veto'] = {'controller': list(nv['controller'].keys()),
                    'readers': list(nv['readers'].keys()),
                    'detectors': ['neutron_veto']}

        # convert the host lists to dics for later
        for det in list(ret.keys()):
            ret[det]['controller'] = {c:{} for c in ret[det]['controller']}
            ret[det]['readers'] = {c:{} for c in ret[det]['readers']}
        return ret

    def get_run_mode(self, mode):
        """
        Pull a run doc from the options collection and add all the includes
        """
        if mode is None:
            return None
        base_doc = self.collections['options'].find_one({'name': mode})
        if base_doc is None:
            self.log_error("Mode '%s' doesn't exist" % mode, "info", "info")
            return None
        if 'includes' not in base_doc or len(base_doc['includes']) == 0:
            return base_doc
        try:
            if self.collections['options'].count_documents({'name':
                {'$in': base_doc['includes']}}) != len(base_doc['includes']):
                self.log_error("At least one subconfig for mode '%s' doesn't exist" % mode, "warn", "warn")
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
        """
        Get the nodes we need from the run mode
        """
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
            return NO_NEW_RUN
        if cursor.count() == 0:
            self.log.info("wtf, first run?")
            return 0
        return list(cursor)[0]['number']+1

    def set_stop_time(self, number, detectors, force):
        """
        Sets the 'end' field of the run doc to the time when the STOP command was ack'd
        """
        self.log.info(f"Updating run {number} with end time ({detectors})")
        if number == -1:
            return
        try:
            time.sleep(0.5) # this number depends on the CC command polling time
            if (endtime := self.get_ack_time(detectors, 'stop') ) is None:
                self.log.debug(f'No end time found for run {number}')
                endtime = now() -datetime.timedelta(seconds=1)
            query = {"number": int(number), "end": None, 'detectors': detectors}
            updates = {"$set": {"end": endtime}}
            if force:
                updates["$push"] = {"tags": {"name": "_messy", "user": "daq",
                    "date": now()}}
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

    def get_ack_time(self, detector, command, recurse=True):
        '''
        Finds the time when specified detector's crate controller ack'd the specified command
        '''
        # the first cc is the "master", so its ack time is what counts
        cc = list(self.latest_status[detector]['controller'].keys())[0]
        query = {'host': cc, f'acknowledged.{cc}': {'$ne': 0}, 'command': command}
        sort = [('_id', -1)]
        doc = self.collections['outgoing_commands'].find_one(query, sort=sort)
        dt = (now() - doc['acknowledged'][cc].replace(tzinfo=pytz.utc)).total_seconds()
        if dt > 30: # TODO make this a config value
            if recurse:
                # No way we found the correct command here, maybe we're too soon
                self.log.debug(f'Most recent ack for {detector}-{command} is {dt:.1f}?')
                time.sleep(2) # if in doubt
                return self.get_ack_time(detector, command, False)
            else:
                # Welp
                self.log.debug(f'No recent ack time for {detector}-{command}')
                return None
        return doc['acknowledged'][cc]

    def send_command(self, command, hosts, user, detector, mode="", delay=0, force=False):
        """
        Send this command to these hosts. If delay is set then wait that amount of time
        """
        number = None
        if command == 'stop' and not self.detector_ackd_command(detector, 'stop'):
            self.log.error(f"{detector} hasn't ack'd its last stop, let's not flog a dead horse")
            if not force:
                return 1
        try:
            if command == 'arm':
                number = self.get_next_run_number()
                if number == NO_NEW_RUN:
                    return -1
                self.latest_status[detector]['number'] = number
            doc_base = {
                "command": command,
                "user": user,
                "detector": detector,
                "mode": mode,
                "createdAt": now()
            }
            if command == 'arm':
                doc_base['options_override'] = {'number': number}
            if delay == 0:
                docs = doc_base
                docs['host'] = hosts[0]+hosts[1]
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
        """
        Process our internal command queue
        """
        sort = [('createdAt', 1)]
        incoming = self.collections['command_queue']
        outgoing = self.collections['outgoing_commands']
        while self.run == True:
            try:
                if (next_cmd := incoming.find_one({}, sort=sort)) is None:
                    dt = 10
                else:
                    dt = (next_cmd['createdAt'].replace(tzinfo=pytz.utc) - now()).total_seconds()
                if dt < 0.01:
                    oid = next_cmd.pop('_id')
                    outgoing.insert_one(next_cmd)
                    incoming.delete_one({'_id': oid})
            except Exception as e:
                dt = 10
                self.log.error(f"DB down? {type(e)}, {e}")
            self.event.wait(dt)
            self.event.clear()

    def host_ackd_command(self, host):
        """
        Finds the timestamp of the oldest unacknowledged command send to the specified host
        :param host: str, the process name to check
        :returns: float, the timestamp of the last unack'd command, or None if none exist
        """
        q = {f'acknowledged.{host}': 0}
        sort = [('_id', 1)]
        if (doc := self.collections['outgoing_commands'].find_one(q, sort=sort)) is None:
            return None
        return doc['createdAt'].timestamp()

    def detector_ackd_command(self, detector, command):
        """
        Finds when the specified/most recent command was ack'd
        """
        q = {'detector': detector}
        sort = [('_id', -1)]
        if command is not None:
            q['command'] = command
        if (doc := self.collections['outgoing_commands'].find_one(q, sort=sort)) is None:
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
        nowtime = now()
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

    def insert_run_doc(self, detector):

        if (number := self.get_next_run_number()) == NO_NEW_RUN:
            self.log.error("DB having a moment")
            return -1
        # the rundoc gets the physical detectors, not the logical
        detectors = self.goal_state[detector]['detectors']

        run_doc = {
            "number": number,
            'detectors': detectors,
            'user': self.goal_state[detector]['user'],
            'mode': self.goal_state[detector]['mode'],
            'bootstrax': {'state': None},
            'end': None
        }

        # If there's a source add the source. Also add the complete ini file.
        cfg = self.get_run_mode(self.goal_state[detector]['mode'])
        if cfg is not None and 'source' in cfg.keys():
            run_doc['source'] = {'type': cfg['source']}
        run_doc['daq_config'] = cfg

        # If the user started the run with a comment add that too
        if "comment" in self.goal_state[detector] and self.goal_state[detector]['comment'] != "":
            run_doc['comments'] = [{
                "user": self.goal_state[detector]['user'],
                "date": now(),
                "comment": self.goal_state[detector]['comment']
            }]

        # Make a data entry so bootstrax can find the thing
        if 'strax_output_path' in cfg:
            run_doc['data'] = [{
                'type': 'live',
                'host': 'daq',
                'location': cfg['strax_output_path']
            }]

        # The cc needs some time to get started
        time.sleep(self.cc_start_wait)
        try:
            start_time = self.get_ack_time(detector, 'start')
        except Exception as e:
            self.log.error('Couldn\'t find start time ack')
            start_time = None

        if start_time is None:
            start_time = now()-datetime.timedelta(seconds=2)
            # if we miss the ack time, we don't really know when the run started
            # so may as well tag it
            run_doc['tags'] = [{'name': 'messy', 'user': 'daq', 'date': start_time}]
        run_doc['start'] = start_time

        try:
            self.collections['run'].insert_one(run_doc)
        except Exception as e:
            self.log.error(f'Database having a moment: {type(e)}, {e}')
            return -1
        return None

