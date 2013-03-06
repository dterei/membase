#!/usr/bin/env python

import sys

import mc_bin_client

ENGINE = "/install/lib/memcached/ep.so"
CONFIG = ";ht_size=3079;ht_locks=5;db_shards=4;tap_noop_interval=20;max_txn_size=1000;max_size=1048576000;tap_keepalive=300;vb0=false;waitforwarmup=false;failpartialwarmup=false;shardpattern=%d/%b-%i.mb;db_strategy=multiMTVBDB;"

INIT_FILE="/install/etc/membase/init.sql;"

if __name__ == '__main__':
    host   = sys.argv[1]
    port   = sys.argv[2]
    user   = sys.argv[3]
    passwd = sys.argv[4]
    base   = sys.argv[5]
    data   = sys.argv[6]
    bucket = sys.argv[7]

    mc = mc_bin_client.MemcachedClient(host, int(port))
    mc.sasl_auth_plain(user, passwd)
    while (True):
        try:
            mc.bucket_select(bucket)
            break
        except:
            dbfile = data + "/" + bucket + "-data/" + bucket
            conf = "initfile=" + base + INIT_FILE + "dbname=" + dbfile \
                    + CONFIG
            mc.bucket_create(bucket, base + ENGINE, conf)
    for i in range(1024):
        mc.set_vbucket_state(i, 'active')

