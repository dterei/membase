#!/bin/bash

# Startup script for easily running memcached with multiple buckets.
# The variables below are for configuration.

ADMIN='_admin'
PASSWORD='pass'
PORT=11211
BASE=`pwd`
DATADIR=${BASE}/run-temp
BUCKETS='first second third fourth'

# export ISASL_PWFILE=$BASE/install/var/lib/membase/data/isasl.pw
export ISASL_PWFILE=$BASE/run-temp/isasl.pw

(
# setup SASL and bucket file
mkdir ${DATADIR}
echo "_admin _admin" > ${ISASL_PWFILE}
for b in ${BUCKETS}; do
	echo "${b} ${PASSWORD}" >> ${ISASL_PWFILE}
	mkdir ${DATADIR}/${b}-data
done
)

(
# setup memcached process
sleep 2
for b in ${BUCKETS}; do
	echo ""
	echo "Setting up bucket: ${b}"
	python ./run-scripts/setup_bucket.py '127.0.0.1' ${PORT} ${ADMIN} ${ADMIN} ${BASE} ${DATADIR} ${b}
done
echo ""
echo "Membase Ready!"
) &

(
# launch memcached
${BASE}/install/bin/memcached \
	-p 11211 -E ${BASE}/install/lib/memcached/bucket_engine.so -B binary -r -c 10000 \
	-e admin="${ADMIN};auto_create=false"
#	-e admin="${ADMIN};default_bucket_name=${DEFAULT_BUCKET};auto_create=false"
)

wait

