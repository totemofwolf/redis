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

#define CLUSTER_NAME_DEFAULT	"default"

#define META_SERVER_CONNECTION_COUNT   		2
#define URL_FORMAT_GET_KEEPER_MASTER		"/vpi/v1/%s/%s/keeper/master"
#define URL_FORMAT_GET_REDIS_MASTER			"/vpi/v1/%s/%s/redis/master"

#define META_SERVER_STATE_NONE   		0
#define META_SERVER_STATE_CONNECTING   	1
#define META_SERVER_STATE_CONNECTED 	2



typedef struct metaConnection{

	char *urlFormat;

	int  metaServerState;
	int  metaServerFd;

	aeFileProc *processor;
}metaConnection;

typedef struct ctrip{

	char* clusterName;
	char* shardName;
	char* metaServerUrl;

	//generated from metaServerUrl
	sds   metaServerHost;
	int   metaServerPort;

	metaConnection connection[META_SERVER_CONNECTION_COUNT];
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
void processRedisMasterResponse(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask);
void processKeeperMasterResponse(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask);
void getCtripMetaInfo();

#endif /* CTRIP_H_ */


