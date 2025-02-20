#!/bin/bash

du /mnt/test
echo 3 | sudo tee /proc/sys/vm/drop_caches

src/keydb-server redis.conf &
#src/keydb-server redis2.conf &
#redis-cli -p 6381 bgsave &
#redis-cli -p 6382 bgsave &


