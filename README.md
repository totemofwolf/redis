
Ctrip branch for redis
-------------------------------------------

<!-- MarkdownTOC -->

1. [config](#config)
	1. [config added to get ctrip redis cluster information](#config-added-to-get-ctrip-redis-cluster-information)
2. [commands](#commands)
	1. [fsync](#fsync)
	2. [info lastmaster](#info-lastmaster)

<!-- /MarkdownTOC -->


<a name="config"></a>
# config
<a name="config-added-to-get-ctrip-redis-cluster-information"></a>
## config added to get ctrip redis cluster information

> these config should be put in the redis config file

   1. meta-server-url  
   the address for x-pipe meta server
   1. cluster-name 
   1. shard-name 
 
<a name="commands"></a>
# commands
<a name="fsync"></a>
## fsync
This command is used for creating replicationlog, while 

not sending rdb and replication stream.
<a name="info-lastmaster"></a>
## info lastmaster
This command returns the previous master infomation When slave state changes(promoted to master or slaveof another server).

> lastmaster_runid lastmaster_reploff current_replication_log_off

1. if lastermaster does not exist, then returns "none -1 ..."
2. if replication log doesn't exist, then current_replication_log_off = -1
