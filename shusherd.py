import sys
import argparse
from subprocess import Popen
import time
import os
import atexit
import json
import requests


# XXX: always kill process

SHUSHER_HELPER = './shusherd'
SHUSHER_CONFIG = 'shusherrc'

AUDIO = "_x_________p__"
child_process = None


@atexit.register
def cleanup_child():
    if child_process:
        child_process.kill()


class Shusher(object):
    def __init__(self, args):
        self.host = args.host
        self.mac_addr = args.mac_addr

    def run(self):
        config = self.get_config()
        if not config:
            raise Exception('Could not get initial config')
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
            print("return code", child_process.returncode)
            print(child_process.returncode)

    def get_config(self):
        r = requests.get('http://{}/shushers/?mac_address={}'.format(
            self.host,
            self.mac_addr))
        if r.status_code == 200:
            return r.json()
        else:
            print("Unable to get config: " + str(r))
            return None

        #print(json.load(open("config.json")))
        #return json.load(open("config.json"))

    def write_config(self, cfg):
        print("writing a config", cfg)
        tmpcfg = SHUSHER_CONFIG + '.tmp'
        with open(tmpcfg, 'w') as f:
            if 'decay' in cfg:
                f.write('decay = {:.2}\n'.format(cfg['decay']))
            if 'sound_threshold' in cfg:
                f.write('threshold = {}\n'.format(cfg['sound_threshold']))
            if 'shout_msg' in cfg:
                f.write('shush_file = "{}.wav"\n'.format(cfg['shout_msg']))
            f.write('input_file = "{}"\n'.format(AUDIO))

            #f.write('verbosity = {}\n'.format(cfg['verbosity']))
            #f.write('input_file = "{}"\n'.format(cfg['input_file']))
        os.rename(tmpcfg, SHUSHER_CONFIG)


def main(argv):
    parser = argparse.ArgumentParser(
        prog='shusherd',
        description='Shusher daemon')
    parser.add_argument('-H', '--host', required=True)
    parser.add_argument('-M', '--mac-addr', required=True)
    parser.add_argument('-f', '--foreground', action='store_true')

    args = parser.parse_args(argv[1:])

    shusher = Shusher(args)
    shusher.run()

if __name__ == '__main__':
    sys.exit(main(sys.argv))
