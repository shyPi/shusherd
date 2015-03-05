#!/usr/bin/python

import sys
import argparse
from subprocess import Popen
import time
import os
import atexit
import json
import requests
from requests.exceptions import ConnectionError

# XXX: always kill process

SHUSHER_HELPER = './shusherd'
SHUSHER_CONFIG = 'shusherrc'
CONFIG_SLEEP = 10

DEFAULT_MIN_THRESHOLD = 40
DEFAULT_MAX_THRESHOLD = 120
DEFAULT_COOLDOWN = 60

child_process = None


@atexit.register
def cleanup_child():
    if child_process:
        child_process.kill()


class Shusher(object):
    def __init__(self, args):
        self.host = args.host
        self.mac_addr = args.mac_addr
        self.input_device = args.input_device
        self.output_device = args.output_device
        self.config = args.config

    def run(self):
        global child_process
        config = self.get_config()
        while not config:
            print("Failed to get config, sleeping")
            time.sleep(CONFIG_SLEEP)
            config = self.get_config()

        self.write_config(config)
        while True:
            child_process = Popen([SHUSHER_HELPER])
            reload = False
            while not reload:
                new_cfg = self.get_config()
                if new_cfg and (config != new_cfg):
                    config = new_cfg
                    self.write_config(config)
                    child_process.terminate()
                    reload = True
                else:
                    time.sleep(new_cfg['poll_interval'])
            print(child_process.returncode)

    def get_config(self):
        if self.host:
            try:
                r = requests.get('http://{}/shushers/device_config?mac_address={}'.format(
                    self.host,
                    self.mac_addr))
                if r.status_code == 200:
                    return r.json()
                else:
                    print("Unable to get config: " + str(r))
                    return None
            except ConnectionError as e:
                print(e)
                return None
        else:
            return json.load(open(self.config))

    def calc_threshold(self, cfg):
        if int(cfg.get('sound_threshold')) < 0:
            return -1

        min_threshold = cfg.get('min_threshold', DEFAULT_MIN_THRESHOLD)
        max_threshold = cfg.get('max_threshold', DEFAULT_MAX_THRESHOLD)
        threshold = cfg.get('sound_threshold') / 100.0
        return int(((max_threshold - min_threshold) * threshold) + min_threshold)

    def write_config(self, cfg):
        print("writing a config", cfg)
        tmpcfg = SHUSHER_CONFIG + '.tmp'
        with open(tmpcfg, 'w') as f:
            if 'decay' in cfg:
                f.write('decay = {:.2}\n'.format(cfg['decay']))
            if 'sound_threshold' in cfg:
                threshold = self.calc_threshold(cfg)
                f.write('threshold = {}\n'.format(threshold))
            if 'filename' in cfg:
                f.write('shush_file = "{}.wav"\n'.format(cfg['filename']))
            if self.input_device:
                f.write('input_device = "{}"\n'.format(self.input_device))
            if self.output_device:
                f.write('output_device = "{}"\n'.format(self.output_device))
            if 'cooldown' in cfg:
                f.write('cooldown = {}\n'.format(cfg['cooldown']))

        os.rename(tmpcfg, SHUSHER_CONFIG)


def main(argv):
    parser = argparse.ArgumentParser(
        prog='shusherd',
        description='Shusher daemon')
    parser.add_argument('-H', '--host')
    parser.add_argument('-C', '--config', default='config.json')
    parser.add_argument('-M', '--mac-addr', required=True)
    parser.add_argument('-f', '--foreground', action='store_true')
    parser.add_argument('-I', '--input-device')
    parser.add_argument('-O', '--output-device')

    args = parser.parse_args(argv[1:])

    shusher = Shusher(args)
    shusher.run()

if __name__ == '__main__':
    sys.exit(main(sys.argv))
