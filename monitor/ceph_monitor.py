import subprocess
import re
import psutil
import os
import time
from pymongo import MongoClient
from pymongo.errors import NotMasterError
import datetime
timeout=2
factors = {'k': 1e3, 'M': 1e6, 'G': 1e9, 'T': 1e12, 'B': 1, 'MiB': 1e6, 'GiB': 1e9,
           'TiB': 1e12, 'kiB': 1e3, 'kB': 1e3, 'objects,': 1}


def CheckOSDs():
    result = subprocess.check_output(['ceph', 'osd', 'status'])
    lines = result.decode().split('\n')
    ansi_escape = re.compile(r'\x1B[@-_][0-?]*[ -/]*[@-~]')
    skipped_header = False
    headers = []
    ret = []
    for line in lines:
        res_list = line.split('|')
        if len(res_list) == 1:
            continue
        if skipped_header == False:
            skipped_header=True
            for h in res_list:
                if h.strip() != '':
                    headers.append(h.strip())
            continue
        thisdoc = {}

        hi = 0
        for i, res in enumerate(res_list):
            if res.strip() != '':
                field = ansi_escape.sub('', res).strip()
                if field.isdigit():
                    thisdoc[headers[hi]] = int(field)
                elif (field[len(field)-1] in ['k', 'M', 'G', 'T'] and
                      field[:-1].replace(".", "", 1).isdigit()):
                    thisdoc[headers[hi]] = float(float(field[:-1]) *
                                            factors[field[len(field)-1]])
                else:
                    thisdoc[headers[hi]] = field
                hi+=1
        ret.append(thisdoc)
    return ret

def CheckStatus():
    ''' This function is really fixed to the format of ceph status. 
    I don't really know how to do it otherwise since I only see the
    output of the cluster I have.'''
    
    result = subprocess.check_output(['ceph', 'status'])
    lines = result.decode().split('\n')
    ret = {'time' : datetime.datetime.utcnow()}
    for line in lines:
        
        res_list = [a.strip() for a in line.split(' ') if a != '']
        if len(res_list) < 2:
            continue
        if res_list[0] == 'health:':
            ret['health'] = res_list[1]
        #elif res_list[0] == 'mon:':
        #    ret['monitors'] = int(res_list[1])
        #    ret['monitor_hosts'] = res_list[4]
        elif res_list[0] == 'mgr:':
            ret['manager'] = res_list[1]
        elif res_list[0] == 'pools:':
            ret['pools'] = int(res_list[1])
            ret['pool_pgs'] = int(res_list[3])
        #elif res_list[0] == 'objects:':
        #    ret['objects'] = int(float(res_list[1]) * factors[res_list[2]])
        #    ret['object_size'] = int(float(res_list[3]) * factors[res_list[4]])
        elif res_list[0] == 'usage':
            ret['used_space'] = int(float(res_list[1])*factors[res_list[2]])
            ret['available_space'] = int(float(res_list[4]) * factors[res_list[5]])
            ret['total_space'] = int(float(res_list[7])*factors[res_list[8]])
        #elif res_list[0] == 'client:':
        #    ret['rd_s'] = int(res_list[1]) * factors[(res_list[2].split('/')[0])]
        #    ret['wt_s'] = int(res_list[4]) * factors[(res_list[5].split('/')[0])]
        #    ret['rd_op_s'] = int(res_list[7])
        #    ret['wt_op_s'] = int(res_list[10])
    return ret

client = MongoClient("mongodb://daq:%s@xenon1t-daq:27017/admin"%os.environ["MONGO_PASSWORD_DAQ"])
db = client['daq']
coll = db['system_monitor']
while(1):
    osds = CheckOSDs()
    stat = CheckStatus()
    stat['osds'] = osds
    
    statvfs = os.statvfs('/live_data')
    stat['host'] = 'ceph'
    stat['ceph_size'] = statvfs.f_frsize * statvfs.f_blocks
    stat['ceph_free'] = statvfs.f_frsize * statvfs.f_bfree
    stat['ceph_available'] = statvfs.f_frsize * statvfs.f_bavail
    try:
        coll.insert(stat)
    except:
        pass
    time.sleep(timeout)

