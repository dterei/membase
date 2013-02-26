# Membase

Note that the company that develops Membase (previously called
Northscale, now Couchbase) doesn't promote or develop it any more.
Instead they push CouchDB now.

[Membase Download](http://www.couchbase.com/downloads-all#couchbase-server-1-7)
[Membase Userguide](http://www.couchbase.com/docs/membase-manual-1.7/)

## Building

There is a top-level single Makefile that build everything else. The
rest of the directories use the standard GNU Automake / Autoconf
toolkit.

By default `make` will build everything, installing at the
subdirectory 'install'. You can try setting the 'DESTDIR' variable to
choose an alternate installation location but I'm not sure if it
works.

~~~~ {.sh}
$ make -j8
~~~~

You can build invidual components by changing to their directories and
issuing the `make` command or even starting with the `configure` step.

## Source Code

Membase is a collection of components that must be invididualy built.
The most important folders are:

* memcached/ - The memcached server. This is a variant of the usual
  memcached server in that it takes plugable 'storage engines' for how
  data should actually be stored, similar to say MySQL.

* bucket_engine/ - Manages the multi-tenancy aspect of membase,
  allowing for multiple instances of storage engines.

* ep-engine/ - A persistent storage engine.

* ns_server/ - The controller process (erlang).

* moxi/ - A memcached proxy that also implements the membase
  extensions (vbuckets). Supports both text and binary as input but
  only binary as output.

## Bucket (multi-tenancy)

* A isolated container.
* Spread across all servers in cluster. (no choice in this)
* Choose bucket type: memcache or membase (persistent)
* One bucket is the 'default', meaning connecting to a server on 11211
  with NO SASL auth gets you that bucket.
* Other buckets are connected to by either using SASL auth, or
  connecting to a specific port (which way is used is configured per
  bucket).

## Moxi (multi-tenancy)

* Moxi is a general proxy for the memcached protocol. Supports text
  and binary (with SASL).
* Membase launches the underlying memcached process with it set to use
  the binary protocol.
* Moxi translates any clients using the text protocol to the binary
  protocol when actually talking to the memcached process.
* A bucket is chosen through (and only through) SASL authentication.
  The 'default' bucket is logged into using the username 'default' and
  the empty password ('').
* When a bucket is configured to be accessed through a dedicated port
  (and so not needing SASL), membase starts another moxi process
  listening on that port. The username of the bucket is its name and
  the password is the empty password.

* So you can avoid the proxy layer by connecting on port 11210, using
  binary and authenticating with SASL to choose the correct bucket.

## vBucket

* [VBucket Explanation](http://blog.couchbase.com/scaling-memcached-vbuckets)

* A vBucket is the "owner" of a subset of the key-space.
* vBucket (id) decided by hashing.
* A vBucket reside on one and only one Server.
* Servers host multiple vBuckets. (e.g., one-to-many relationship)
* Layer of indirection between the hashing algorithm and servers
  (e.g., move a key-space / vBucket by simply updating the table that
  maps a vBucketID -> Server, enables migration)
* So hashing is: h(k) -> vb\_<id> -> server\_<id>
* Number of vBuckets is static (decided at process create time).
* Mapping of vBucket -> server is static, updated as part of servers
  entering/leaving cluster and migration.
* vBucket gives fine granularity to key space and a level of
  indirection to make migration and cluster membership easier.

* I don't believe you explicitly manage vBuckets, or at least not
  through the Web Admin interface, potentially you can through a smart
  client.

* Migration and replication done through vBuckets.

* I assume vBuckets are implemented by using a second (in addition to
  regular LRU) linked list on items. Each vBucket is a linked list,
  may make sense that this linked list is managed as an LRU as that
  makes some sense for migration.

## Processes

### Memcached

* Listens by default on 11210.
* Multi-tenant and can use different storage engines per tenant (or
  called, 'buckets' in membase terminology).
* Also support vBuckets, which seem to be a further abstraction of the
  key-space for easier replication, migration features. (not sure how
  managed yet, I think there is an extension to the memcache protocol
  for managing vBuckets).
* Supports text, binary and sasl protocols. (but not together, only
  supports text or protoocl, choose which one at startup).
* Membase starts it with the binary protocol.
* SASL authentication used for distinguishing which bucket a
  connection is going to use.
* (May be a way to specify the bucket outside of SASL as you can have
  a bucket be distinguished through a specific listen port. However,
  moxi does the actual listen, not memcached. So not sure how yet this
  bucket info is passed from moxi to memcached (doing SASL between
  moxi and memcached seems plausible).

### Moxi

* Listens by default on 11211, running on each server in the cluster
  as an 'embedded proxy'.
* Pass-through proxy for memcached protocol (text, binary, SASL).
* Membase spawns more for listening on extra ports when assinging a
  bucket to a certain port.
* Not sure if it supports anything beyond standard memcached protocol
  when talking to membase's memcached.
* While by-default it is run on each server tha is in the cluster (and
  so while a client talks to server A, moxi running at A may forward
  the request to server B), it can instead be run standalone. So
  either run one proxy for each client machine and avoid an extra hop,
  or have a common one among many client machines.
* 'Smart clients' can just include the needed mechanisms and avoid
  moxi altogether (e.g., connect on port 11210).

### ns\_server (beam.smp)

* Started by 'membase_server' (simply a shell script).
* Launches the whole node (e.g., entry point)
* Runs the web admin interface and the per-server controller.
* Controller does cluster management:
  * Brings servers into the cluster and out.
  * Cluster admin (buckets, access control)
  * Node admin (start, stop, restart...)
  * Monitoring
  * Run-time logging
  * Manages fail-over.

### beam.smp, empd, inet\_gethost

* beamp.smp: Erlang SMP runtime engine.
* empd: Erlang port mapper daemon (name server for all hosts in
  distributed erlang computation).
* inet\_gethost: Provides a port for Erlang processes to query for
  namelook up services.

### disksup, memsup, cpu\_sup, portsigar

* Resource monitors.
* portsigar: https://github.com/alk/portsigar

