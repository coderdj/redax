from pymongo import MongoClient
import os
import psutil
timeout = 2
import socket
import re
import signal
import threading
import time

client = MongoClient("mongodb://daq:%s@xenon1t-daq:27017/admin"%os.environ["MONGO_PASSWORD_DAQ"])
db = client['daq']
collection = db['system_monitor']

ev = threading.Event()

signal.signal(signal.SIGINT, lambda num, frame : ev.set())
signal.signal(signal.SIGTERM, lambda num, frame : ev.set())

while not ev.is_set():

    ret_doc = {'host': socket.gethostname(), 'time' : time.time()*1000}

    # CPU
    ret_doc['cpu_percent'] = psutil.cpu_percent()
    ret_doc['cpu_count'] = psutil.cpu_count(False)
    ret_doc['cpu_count_logical'] = psutil.cpu_count()

    # MEM
    keep = ['total', 'percent']
    virt_mem = dict(psutil.virtual_memory()._asdict())
    ret_doc['virtual_memory'] = {k:virt_mem[k] for k in keep}
    swap_mem = dict(psutil.swap_memory()._asdict())
    ret_doc['swap_memory'] = {k:swap_mem[k] for k in keep}

    # DISK
    disk = {}
    pattern = '^/(data[0-9]?)?$'
    for partition in psutil.disk_partitions():
        mount = partition.mountpoint
        if re.match(pattern, mount) is not None:
            d = dict(psutil.disk_usage(mount)._asdict())
            disk[mount] = {k:d[k] for k in keep}
    ret_doc['disk'] = disk

    collection.insert(ret_doc)
    ev.wait(timeout)

