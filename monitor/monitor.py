from pymongo import MongoClient
import os
import psutil
import time
import datetime
timeout = 2
import socket

client = MongoClient("mongodb://daq:%s@xenon1t-daq:27020/daq"%os.environ["MONGO_PASSWORD_DAQ"])
db = client['daq']
collection = db['system_monitor']

while(1):

    ret_doc = {'host': socket.gethostname()}

    # CPU
    ret_doc['cpu_times'] = dict(psutil.cpu_times()._asdict())
    ret_doc['cpu_percent'] = psutil.cpu_percent()
    ret_doc['cpu_percent_percpu'] = psutil.cpu_percent(percpu=True)
    ret_doc['cpu_count_logical'] = psutil.cpu_count()
    ret_doc['cpu_count'] = psutil.cpu_count(logical=False)


    # MEM
    ret_doc['virtual_memory'] = dict(psutil.virtual_memory()._asdict())
    ret_doc['swap_memory'] = dict(psutil.swap_memory()._asdict())

    # DISK
    disk = {}
    disk_partitions = psutil.disk_partitions(all=False)
    for partition in disk_partitions:
        mount = partition.mountpoint
        device = partition.device
        disk[mount] = dict(psutil.disk_usage(mount)._asdict())
        disk[mount]['device'] = device
    ret_doc['disk'] = disk
    
    #print(ret_doc)
    collection.insert(ret_doc)    
    time.sleep(timeout)
