#!/bin/bash

numactl -C 0,1 src/keydb-server redis.conf &
numactl -C 2,3 src/keydb-server redis2.conf &
#redis-cli -p 6381 bgsave &
#redis-cli -p 6382 bgsave &


