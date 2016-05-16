
Ctrip branch for redis, Some extra function added
-------------------------------------------
# commands
## fsync
This command is used for creating replicationlog, while not sending rdb and replication stream.
## info lastmaster
This command returns the previous master infomation When slave state changes(promoted to master or slaveof another server).

> lastmaster_runid lastmaster_reploff current_replication_log_off

1. if lastermaster does not exist, then returns "none -1 ..."
2. if replication log doesn't exist, then current_replication_log_off = -1
