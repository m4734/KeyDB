#!/bin/bash

rm -rf /mnt/test/*

src/keydb-server key4.conf &
#src/keydb-server redis2.conf &
#redis-cli -p 6381 bgsave &
#redis-cli -p 6382 bgsave &


