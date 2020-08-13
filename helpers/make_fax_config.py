import os
from pymongo import MongoClient
import argparse
import math


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--size', type=int, help='How many rings of PMTs', default=2)
    parser.add_argument('--rate', type=float, help='Rate in Hz', default=10)
    parser.add_argument('--e-livetime', type=float, help='Electron livetime given in units of TPC lengths', default=1.5)
    parser.add_argument('--drift-speed', type=float, help='Drift speed in PMTs/ns', default=1e-4)
    parser.add_argument('--name', type=str, help='Name of config', default='UNDEFINED')
    args = parser.parse_args()

    if args.name == 'UNDEFINED':
        print('Please specify a config name')
        return

    boards = []
    channel_map = {}
    thresholds = {}
    n_pmts = 2*(1+3*args.size*(args.size+1))
    print('PMTs: ', n_pmts) # 38
    n_boards = math.ceil(n_pmts / 8)
    print('Boards: ', n_boards) # 5
    n_links = math.ceil(n_boards / 8)
    print('Links: ', n_links) # 1
    boards_per_link = []
    for l in range(n_links):
        boards_per_link.append(n_boards//n_links)
        if l < n_boards % n_links:
            boards_per_link[l] += 1

    for l in range(n_links):
        for b in range(boards_per_link[l]):
            bid = b + sum(boards_per_link[:l])
            boards.append({
                'type': 'V1724_fax',
                'host': 'fdaq00_reader_0',
                'link': l,
                'crate': b,
                'vme_address': '0',
                'board': bid,
                })
            thresholds[str(bid)] = [15]*8
    for ch in range(n_pmts):
        bid = str(ch // 8)
        ch_i = ch % 8
        if bid not in channel_map:
            channel_map[bid] = [2*n_pmts]*8
        channel_map[bid][ch_i] = ch

    board_doc = {
            'name': 'fax_%i_boards' % n_boards,
            'user': 'darryl',
            'description': 'fax subconfig, %i digitizers' % n_boards,
            'detector': 'include',
            'boards': boards,
            'registers': [],
            }
    channel_doc = {
            'name': 'fax_%i_channels' % n_pmts,
            'user': 'darryl',
            'description': 'fax subconfig, %i channels' % n_pmts,
            'detector': 'include',
            'channels': channel_map,
            'thresholds': thresholds,
            }
    generic_doc = {
            'name': 'fax_common_opts',
            'user': 'darryl',
            'description': 'fax subconfig, generic options',
            'detector': 'include',
            "processing_threads": {
                "fdaq00_reader_0": 16
                },
            "baseline_dac_mode": "fixed",
            "run_start": 1,
            'buffer_type': 'dual',
            'detectors' : {'fdaq00_reader_0' : 'fax_freiburg'},
            }
    strax_opts = {
            "name": "fax_strax_options",
            'user': 'darryl',
            'description': 'fax subconfig, strax options',
            'detector': 'include',
            'strax_chunk_length': 5.0,
            'strax_chunk_overlap': 0.1,
            'strax_fragment_payload_bytes': 32,
            'compressor': 'lz4',
            'strax_buffer_num_chunks': 2,
            'strax_chunk_phase_limit': 2,
            'strax_output_path': '/home/xedaq/data',
            }
    doc = {
            'name': args.name,
            'user': 'darryl',
            'description': 'fax config, size %i' % args.size,
            'fax_options': {
                'rate': args.rate,
                'tpc_size': args.size,
                'e_absorbtion_length': args.e_livetime,
                'drift_speed': args.drift_speed
                },
            'detector': 'freiburg_fax',
            'includes': [
                'fax_%i_boards' % n_boards,
                'fax_%i_channels' % n_pmts,
                'fax_common_opts',
                'fax_strax_options',
                ]
        }
    with MongoClient(os.environ['MONGO']) as client:
        coll = client['fax_test']['options']
        coll.update_one({'name': args.name}, {'$set': doc}, upsert=True)
        coll.update_one({'name': 'fax_strax_options'}, {'$set': strax_opts}, upsert=True)
        coll.update_one({'name': 'fax_common_opts'}, {'$set': generic_doc}, upsert=True)
        coll.update_one({'name': 'fax_%i_channels' % n_pmts}, {'$set': channel_doc}, upsert=True)
        coll.update_one({'name': 'fax_%i_boards' % n_boards}, {'$set': board_doc}, upsert=True)
        print('Documents inserted')

if __name__ == '__main__':
    main()
