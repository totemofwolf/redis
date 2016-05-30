/*
 * ctrip.h
 *
 *  Created on: May 12, 2016
 *      Author: wenchao.meng
 */

#ifndef CTRIP_H_
#define CTRIP_H_

#include <ifaddrs.h>
#include <arpa/inet.h>

#define HTTP_CRLF  "\r\n"

#define REDIS_FAKESLAVE (1<<31)

#define CTRIP_CURRENT_ROLE_UNKNOWN -1
#define CTRIP_CURRENT_ROLE_MASTER 0
#define CTRIP_CURRENT_ROLE_SLAVE  1

#define CONFIG_NAME_META_SERVER_URL "meta-server-url"
#define CONFIG_NAME_CLUSTER_NAME	"cluster-name"
#define CONFIG_NAME_SHARD_NAME		"shard-name"

#define CLUSTER_NAME_DEFAULT	"default"

#define META_SERVER_CONNECTION_COUNT   		2
#define META_SERVER_TIME_OUT 60

#define URL_FORMAT_GET_KEEPER_MASTER			"/api/v1/%s/%s/keeper/master?format=plain"
#define URL_FORMAT_GET_DEFAULT_KEEPER_MASTER	"/api/v1/%s/keeper/master?format=plain"
#define URL_FORMAT_GET_REDIS_MASTER				"/api/v1/%s/%s/redis/master?format=plain"
#define URL_FORMAT_GET_DEFAULT_REDIS_MASTER		"/api/v1/%s/redis/master?format=plain"

#define META_SERVER_STATE_NONE   		0
#define META_SERVER_STATE_CONNECTING   	1
#define META_SERVER_STATE_CONNECTED 	2
#define META_SERVER_STATE_READING_RESPONSE 	4

#define CONNECTION_DESC_LENGTH  (REDIS_PEER_ID_LEN * 2 + 10)

//for http
#define HTTP_STATE_READ_STATUS  0
#define HTTP_STATE_READ_HEADER  1
#define HTTP_STATE_READ_BODY  2

#define HTTP_BODY_TYPE_UNKNOWN -1
#define HTTP_BODY_TYPE_CHUNKED 0
#define HTTP_BODY_TYPE_LENGTH 1

#define HTTP_STATE_CHUNKED_READING_LEN  0
#define HTTP_STATE_CHUNKED_READING_BODY	1
#define HTTP_STATE_CHUNKED_READING_CRLF	2

typedef struct inetAddress{
	sds host;
	int port;
}inetAddress;


typedef struct httpStatus{

	int statusCode;
}httpStatus;

typedef struct httpResponse{

	int httpResponseState;

	httpStatus httpStatus;
	int httpBodyType;

	//for content-length
	int contentLength;


	//for chunked
	int chunkedState;
	int chunkedCurrentTotalLen;

	int currentLen;

	sds httpBody;

}httpResponse;

typedef void (*responseHandler)(httpResponse *httpResponse);

typedef struct metaConnection{

	char *urlFormat, *defaultUrlFormat;
	char connectionDesc[CONNECTION_DESC_LENGTH];

	int  connectionState;
	int  metaServerFd;

	aeFileProc *processor;

	time_t lastIoTime;

	sds sendBuff;
	int sentlen;

	sds receiveBuff;
	httpResponse httpResponse;
	responseHandler responseHandler;

}metaConnection;

typedef struct ctrip{

	int metaServerTimeout;
	char* clusterName;
	char* shardName;
	char* metaServerUrl;

	//generated from metaServerUrl
	sds   metaServerHost;
	int   metaServerPort;

	metaConnection connection[META_SERVER_CONNECTION_COUNT];

	//localaddress
	sds *localAddresses;
	int localAddressCount;

	//keeperAddress
	inetAddress keeper;
	//masterAddress
	inetAddress master;
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
void cronGetCtripMetaInfo();

#endif /* CTRIP_H_ */


