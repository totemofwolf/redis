/*
 * ctrip.h
 *
 *  Created on: May 12, 2016
 *      Author: wenchao.meng
 */

#ifndef CTRIP_H_
#define CTRIP_H_

#define REDIS_FAKESLAVE (1<<31)

#define CONFIG_NAME_META_SERVER_URL "meta-server-url"
#define CONFIG_NAME_CLUSTER_NAME	"cluster-name"
#define CONFIG_NAME_SHARD_NAME		"shard-name"


typedef struct ctrip{

	char* clusterName;
	char* shardName;
	char* metaServerUrl;
}ctripServer;

typedef struct lastMaster{

	char master_runid[REDIS_RUN_ID_SIZE + 1];
	int   master_reploff;
	/**
	 * if replication log exists, this == server.master_repl_offset
	 * else -1
	 */
	int   currentReploff;
}lastMaster;


void doFakeSync(redisClient *c);
void initCtripConfig();
void saveLastMaster();
void clearLastMaster();
sds lastMasterInfo(sds info);
int loadCtripConfig(sds *argv, int argc);
void checkServerConfig();
void config_get_ctrip_field(char *pattern, redisClient *c, int *pmatches);

#endif /* CTRIP_H_ */


