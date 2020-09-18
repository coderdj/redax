import os
from pymongo import MongoClient
import argparse
import math

PMTsPerBoard = 8
BoardsPerLink = 8

def main():
    parser = argparse.ArgumentParser()
    default_size = 2
    default_rate=10
    default_lifetime = 1.5
    default_speed = 1e-4
    parser.add_argument('--size', type=int, default=default_size,
            help='How many rings of PMTs, default %i' % default_size)
    parser.add_argument('--rate', type=float, default=default_rate,
            help='Rate in Hz, default %.1f' % default_rate)
    parser.add_argument('--e-lifetime', type=float, default=default_lifetime,
            help='Electron livetime/TPC lengths, default %.1f' % default_lifetime)
    parser.add_argument('--drift-speed', type=float, default=default_speed,
            help='Drift speed in PMTs/ns, default %.1e' % default_speed)
    parser.add_argument('--name', type=str, help='Name of config', default='UNDEFINED')
    args = parser.parse_args()

    if args.name == 'UNDEFINED':
        print('Please specify a config name')
        return

    boards = []
    channel_map = {}
    thresholds = {}
    n_pmts = 2*(1+3*args.size*(args.size+1))
    n_boards = math.ceil(n_pmts/PMTsPerBoard)
    n_links = math.ceil(n_boards/BoardsPerLink)
    boards_per_link = []
    for l in range(n_links):
        boards_per_link.append(n_boards//n_links)
        if l < n_boards % n_links:
            boards_per_link[l] += 1

    bid = 0
    for l in range(n_links):
        for b in range(boards_per_link[l]):
            boards.append({
                "type": "V1724_fax",
                "host": "reader4_reader_0",
                "link": l,
                "crate": b,
                "vme_address": '0',
                "board": bid
                })
            thresholds[str(bid)] = [15]*PMTsPerBoard
            bid += 1
    for ch in range(n_pmts):
        bid = str(ch%n_boards)
        ch_i = ch // n_boards
        if bid not in channel_map:
            channel_map[bid] = [2*n_pmts]*PMTsPerBoard
        channel_map[bid][ch_i] = ch

    print('%i links/%i boards/%i PMTs' % (n_links, n_boards, n_pmts))

    board_doc = {
            "name": "fax_%i_boards" % n_boards,
            "user": "darryl",
            "description": "fax subconfig",
            "detector": "include",
            "boards": boards,
            "registers": []
            }
    channel_doc = {
            "name": "fax_%i_channels" % n_pmts,
            "user": "darryl",
            "description": "fax subconfig",
            "detector": "include",
            "channels": channel_map,
            "thresholds": thresholds,
            }
    generic_doc = {
            "name": "fax_common_opts",
            "user": "darryl",
            "description": "fax subconfig",
            "detector": "include",
            "processing_threads": {"reader4_reader_0": "auto"},
            "baseline_dac_mode": "fixed",
            "run_start": 1,
            "detectors": {"reader4_reader_0": "fax"},
            }
    strax_opts = {
            "name": "fax_strax_options",
            "user": "darryl",
            "description": "fax subconfig",
            "detector": "include",
            "strax_chunk_length": 5.0,
            "strax_chunk_overlap": 0.5,
            "strax_fragment_payload_bytes": 40,
            "compressor": "lz4",
            "strax_buffer_num_chunks": 2,
            "strax_chunk_phase_limit": 1,
            "strax_output_path": "/live_data/test",
            "output_files": {"reader4_reader_0": 4}
            }
    doc = {
            "name": args.name,
            "user": "darryl",
            "desription": "fax config, size %i" % args.size,
            "fax_options": {
                "rate": args.rate,
                "tpc_size": args.size,
                "e_absorbtion_length": args.e_lifetime,
                "drift_speed": args.drift_speed,
                },
            "run_start": 0,
            "detector": "fax",
            "includes": [
                board_doc['name'],
                channel_doc['name'],
                generic_doc['name'],
                strax_opts['name'],
                ]
            }
    with MongoClient("mongodb://daq:%s@xenon1t-daq:27017/admin" % os.environ['MONGO_PASSWORD_DAQ']) as client:
        coll = client["testdb"]["options"]
        coll.update_one({"name": args.name}, {"$set": doc}, upsert=True)
        coll.update_one({"name": strax_opts['name']}, {'$set': strax_opts}, upsert=True)
        coll.update_one({'name': generic_doc['name']}, {'$set': generic_doc}, upsert=True)
        coll.update_one({'name': board_doc['name']}, {'$set': board_doc}, upsert=True)
        coll.update_one({'name': channel_doc['name']}, {'$set': channel_doc}, upsert=True)

        print('Documents inserted')

    return

if __name__ == '__main__':
    main()

