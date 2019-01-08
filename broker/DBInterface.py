from pymongo import MongoClient
import os
import datetime

class DBInterface():
    def __init__(self, config, hostname):
        self.dax_db = MongoClient(
            config['DEFAULT']['ControlDatabaseURI']%os.environ['MONGO_PASSWORD'])['xenonnt']
        self.log_db = MongoClient(
            config['DEFAULT']['ControlDatabaseURI']%os.environ['MONGO_PASSWORD'])['xenonnt']
        self.runs_db = MongoClient(
            config['DEFAULT']['RunsDatabaseURI']%os.environ['MONGO_PASSWORD'])['xenonnt']
        self.STATUSES = ["Idle", "Arming", "Armed", "Running", "Error", "Timeout", "Unknown"]
        self.collections = {
            "incoming": self.dax_db["detector_control"],
            "status": self.dax_db["status"],
            "aggregate_status": self.dax_db["dispatcher_status"],
            "actual_aggregate_status": self.dax_db['aggregate_status'],
            "outgoing": self.dax_db["control"],
            "log": self.log_db["log"],
            "run": self.runs_db["run"],
            "options": self.dax_db["options"]
        }
        
        self.hostname = hostname
        
    def GetState(self):
        cursor = self.collections['incoming'].find()
        return list(cursor)

    def GetHostStatus(self):
        ret = []
        # 'distinct' would be good here but I've been getting errors calling distinct
        # on a capped collection. So manually geta  list of hosts

        # Limit to last 500 docs
        cursor = self.collections["status"].find().sort("_id", -1).limit(500)
        hosts = []
        for doc in cursor:
            if doc['host'] not in hosts:
                hosts.append(doc['host'])
        for host in hosts:
            doc = list(self.collections['status'].find({"host": host}).sort("_id", -1).limit(1))[0]
            ret.append(doc)
        return ret

    def SendCommand(self, command):
        self.collections['outgoing'].insert(command)

    def GetRunMode(self, mode):
        if mode is None:
            return None
        doc = self.collections["options"].find_one({"name": mode})
        try:
            newdoc = {**dict(doc)}
            if "includes" in doc.keys():
                for i in doc['includes']:
                    incdoc = self.collections["options"].find_one({"name": i})
                    newdoc = {**dict(newdoc), **dict(incdoc)}
                    
            print(newdoc)
            return newdoc
        except Exception as E:
            # LOG ERROR
            print(E)
        return None
        
    def GetHostsForMode(self, mode):
        print("Getting hosts for mode %s"%mode)
        if mode is None:
            return [], None
        doc = self.GetRunMode(mode)
        if doc is None:
            return [], None
        cc = None
        hostlist = []
        for b in doc['boards']:
            if b['type'] == 'V1724' and b['host'] not in hostlist:
                hostlist.append(b['host'])
            elif b['type'] == 'V2718':
                cc = b['host']

        return hostlist, cc

    def LoadDispatcherStatus(self):
        ret = {}
        cursor = self.collections["aggregate_status"].find()
        for doc in cursor:
            ret[doc['detector']] = doc
        return ret
    
    def UpdateDispatcherStatus(self, status):
        for det, doc in status.items():
            doc['detector'] = det
            doc['human_readable_status'] = self.STATUSES[doc['status']]
            doc['update_time'] = datetime.datetime.utcnow()
            self.collections["aggregate_status"].update({"detector": det},
                                                        doc, upsert=True)
        return

    def GetNextRunNumber(self):
        cursor = self.collections["run"].find().sort("number", -1).limit(1)
        if cursor.count() == 0:
            print("wtf, first run?")
            return 0
        return list(cursor)[0]['number']+1


    def InsertRunDoc(self, det, doc):
        run_doc = {
            "number": det['number'],
            'detector': doc['detector'],
            'user': doc['user'],
            'start': det['started_at'],
            'mode': det['mode'],
            'source': 'none'
        }

        ini = self.GetRunMode(det['mode'])
        if ini is not None and 'source' in ini.keys():
            run_doc['source'] = {'type': ini['source']}
        run_doc['ini'] = ini
            
        if "comment" in doc.keys() and doc['comment'] != "":
            run_doc['comments'] = [{
                "user": doc['user'],
                "date": datetime.datetime.utcnow(),
                "comment": doc['comment']
            }]
            # parse hashtags here if desired

        if 'strax_output_path' in ini:
            run_doc['data'] = [{
                'type': 'live',
                'host': 'daq',
                'location': ini['strax_output_path']
            }]

        self.collections['run'].insert_one(run_doc)

    def InsertAggregateStatus(self, status):
        for s in status:
            self.collections['actual_aggregate_status'].insert(s)

    def UpdateEndTime(self, run):
        self.collections['run'].update({"number": run}, {"$set": {"end": datetime.datetime.utcnow()}})
