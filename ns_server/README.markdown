# The Membase Server

This application represents the top of the hierarchy of all memcached
smart services.  It is an application in the Erlang OTP sense.

TODO: this picture needs updating

<div>
    <img src="https://github.com/membase/ns_server/raw/master/doc/images/ns_server.png"
         alt="[ns server]" style="float: right"/>
</div>

## Building

Build dependencies include...

* erlang R14 (make sure to have functional crypto)

Building...

You should use top level make file and repo manifest as explained
here: https://github.com/membase/manifest/blob/master/README.markdown

## Runtime dependencies

Before you start the server, you may need to do the following
  * Make sure the needed ports are not being used (these include
    8091, 11211, 11212, etc).


## Running

After building everything via top level makefile you'll have
membase-server script in your $REPO/install/bin (or other prefix if
you specified so). You can run this script for normal single node
startup.

During development it's convenient to have several 'nodes' on your
machine. There's ./cluster_run script in root directory for achiving
that. Feel free to ask --help. You normally need something like -n2
where 2 is number of nodes you want.

It'll start REST API on ports 9000...9000+n. memcached on ports
12000+2*i and moxi ports on 12001+2*i ports.

Note that blank nodes are not configured and need to be setup. I
suggest trying web UI first to get the feeling of what's
possible. Just visit REST API port(s) via browser. For development
mode clusters it's port 9000 and higher. For production mode it's port
8091.

Other alternative is setting up and clustering nodes via REST
API. membase-cli allows that. And you can easily write your own
script(s).

There's ./cluster_connect script that eases cluster configuration for
development clusters. Ask --help.

Sometimes during debugging/development you want smaller number of
vbuckets. You can change vbuckets number by setting
MEMBASE_NUM_VBUCKET environment variable to desired number of vbuckets
before creating new membase bucket.

### Other tools

Membase ships with a bunch of nice tools. Feel free to check
$REPO/install/bin (or $PREFIX/bin). One of notable tools is
mbstats. It allows you to query buckets for all kinds of internal
stats.

Another notable tool is membase-cli. Script is called just membase.

* * * * *
Copyright (c) 2011, Couchbase, Inc.
