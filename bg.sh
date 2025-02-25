#!/bin/bash

src/keydb-cli -p 6379 bgsave &
src/keydb-cli -p 6380 bgsave &
#redis-cli -p 6381 bgsave &
#redis-cli -p 6382 bgsave &


