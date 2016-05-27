/*
 * ctrip.c
 *
 *  Created on: May 12, 2016
 *      Author: wenchao.meng
 */

#include "redis.h"

ctripServer ctrip;
lastMaster lmaster;


void sendGetRequest(metaConnection *connection);

void clearLastMaster(){

	lmaster.master_runid[REDIS_RUN_ID_SIZE] = '\0';
	lmaster.master_reploff = -1;
	lmaster.currentReploff = -1;
}

void initCtripConfig(){

	ctrip.clusterName = "default";
	ctrip.metaServerUrl = NULL;
	ctrip.shardName = NULL;


	ctrip.connection[0].metaServerState = META_SERVER_STATE_NONE;
	ctrip.connection[0].metaServerFd = -1;
	ctrip.connection[0].urlFormat = URL_FORMAT_GET_KEEPER_MASTER;
	ctrip.connection[0].processor = processKeeperMasterResponse;


	ctrip.connection[1].metaServerState = META_SERVER_STATE_NONE;
	ctrip.connection[1].metaServerFd = -1;
	ctrip.connection[1].urlFormat = URL_FORMAT_GET_REDIS_MASTER;
	ctrip.connection[1].processor = processRedisMasterResponse;


	clearLastMaster();
}

void saveLastMaster(){

	memcpy(lmaster.master_runid, server.repl_master_runid, REDIS_RUN_ID_SIZE);
	lmaster.master_reploff = server.master->reploff;
	if(server.repl_backlog != NULL){
		lmaster.currentReploff = server.master_repl_offset;
	}

}

sds lastMasterInfo(sds info){

	if(lmaster.master_reploff == -1){
		info = sdscatprintf(info, "%s %d %d\r\n", "none", lmaster.master_reploff, lmaster.currentReploff);
	}else{
		info = sdscatprintf(info, "%s %d %d\r\n", lmaster.master_runid, lmaster.master_reploff, lmaster.currentReploff);
	}

	return info;
}

/**
 * fake slave, just generate replication log
 */
void doFakeSync(redisClient *c){
    char buf[128];
    int buflen;

    c->flags |= REDIS_SLAVE;
    c->flags |= REDIS_FAKESLAVE;
    c->replstate = REDIS_REPL_ONLINE;
    c->repl_ack_time = server.unixtime;
    c->repl_put_online_on_ack = 0;
    listAddNodeTail(server.slaves,c);

    buflen = snprintf(buf,sizeof(buf),"+CONTINUE\r\n");
    if (write(c->fd,buf,buflen) != buflen) {
        freeClientAsync(c);
        redisLog(REDIS_NOTICE,
            "fake slave request from %s abort", replicationGetSlaveName(c));
        return;
    }

    if (listLength(server.slaves) == 1 && server.repl_backlog == NULL)
        createReplicationBacklog();

    refreshGoodSlavesCount();
}

int getLenUntilPath(sds hostPortPath){

	int i, len = sdslen(hostPortPath);

	for(i =0; i < len; i++){
		if(hostPortPath[i] == '/'){
			break;
		}
	}

	return i;

}

void getMetaServerHostPort(){

	int argc;
	char *err;

	sds *argv = sdssplitlen(ctrip.metaServerUrl, strlen(ctrip.metaServerUrl), "://", 3, &argc);

	if(argc != 2){
		err = "can not parse meta-server-url, split by ://";
		goto err;
	}

	if(strcasecmp(argv[0], "http")){

		err = "unsupported https protocol, only support http";
		goto err;
	}


	int hostPortLen;
	sds *hostPort = sdssplitlen(argv[1], getLenUntilPath(argv[1]), ":", 1, &hostPortLen);

	if(hostPortLen != 1 && hostPortLen != 2){
		err = "host port split by : length not right";
		goto err;
	}

	ctrip.metaServerHost = sdsdup(hostPort[0]);
	if(hostPortLen == 1){
		ctrip.metaServerPort = 80;
	}else{
		ctrip.metaServerPort = atoi(hostPort[1]);
		if(ctrip.metaServerPort <= 0){
			err = "metaServerPort < 0";
			goto err;
		}
	}

    redisLog(REDIS_NOTICE, "metaServer:(%s:%d)", ctrip.metaServerHost, ctrip.metaServerPort);

	sdsfreesplitres(hostPort, hostPortLen);
	sdsfreesplitres(argv, argc);
	return;

err:
	fprintf(stderr, "\n*** FATAL CONFIG FILE ERROR ***\n");
	fprintf(stderr, "%s %s\n", err, ctrip.metaServerUrl);
	exit(1);
}


int loadCtripConfig(sds *argv, int argc){

	if(!strcasecmp(argv[0], CONFIG_NAME_META_SERVER_URL) && argc == 2){
		ctrip.metaServerUrl = zstrdup(argv[1]);
		getMetaServerHostPort();
		return 1;
	}
	if(!strcasecmp(argv[0], CONFIG_NAME_CLUSTER_NAME) && argc == 2){
		ctrip.clusterName = zstrdup(argv[1]);
		return 1;
	}
	if(!strcasecmp(argv[0], CONFIG_NAME_SHARD_NAME) && argc == 2){
		ctrip.shardName = zstrdup(argv[1]);
		return 1;
	}
	return 0;
}

void checkServerConfig(){

	char *err = NULL;
	if(ctrip.metaServerUrl == NULL){
		err = "meta-server-url not configurd!";
		goto configerr;
	}

	if(ctrip.shardName == NULL){
		err = "shard-name not configurd!";
		goto configerr;
	}

	return;

configerr:
    fprintf(stderr, "\n*** FATAL CONFIG FILE ERROR ***\n");
    fprintf(stderr, "%s\n", err);
    exit(1);
}

#define config_get_string_field(_name,_var) do { \
    if (stringmatch(pattern,_name,0)) { \
        addReplyBulkCString(c,_name); \
        addReplyBulkCString(c,_var ? _var : ""); \
        (*pmatches)++; \
    } \
} while(0);

void config_get_ctrip_field(char *pattern, redisClient *c, int *pmatches){

	config_get_string_field(CONFIG_NAME_META_SERVER_URL, ctrip.metaServerUrl);
	config_get_string_field(CONFIG_NAME_CLUSTER_NAME, ctrip.clusterName);
	config_get_string_field(CONFIG_NAME_SHARD_NAME, ctrip.shardName);
}

void metaServerConnect(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask){

	int sockerr = 0, errlen = sizeof(sockerr);
	metaConnection *connection = clientData;

    /* Check for errors in the socket. */
	int sockopt = getsockopt(fd, SOL_SOCKET, SO_ERROR, &sockerr, &errlen);
    if ( sockopt == -1){
        sockerr = errno;
    }
    if (sockerr) {
        redisLog(REDIS_WARNING,"Error connection on socket for metaserver: %s",
            strerror(sockerr));
        aeDeleteFileEvent(server.el,fd,AE_READABLE|AE_WRITABLE);
        close(fd);
        connection->metaServerFd = -1;
        connection->metaServerState = META_SERVER_STATE_NONE;
        return;
    }

    aeDeleteFileEvent(server.el,fd,AE_READABLE|AE_WRITABLE);
    connection->metaServerState = META_SERVER_STATE_CONNECTED;
    redisLog(REDIS_NOTICE, "Connect to server successed(%s:%d), fd:%d", ctrip.metaServerHost, ctrip.metaServerPort, fd);
}

int connectWithMetaServer(metaConnection *connection){

	int   fd;

    fd = anetTcpNonBlockBestEffortBindConnect(NULL,
        ctrip.metaServerHost, ctrip.metaServerPort,REDIS_BIND_ADDR);
    if (fd == -1) {
        redisLog(REDIS_WARNING,"Unable to connect to server(%s:%d): %s", ctrip.metaServerHost, ctrip.metaServerPort, strerror(errno));
        return REDIS_ERR;
    }

    if (aeCreateFileEvent(server.el,fd,AE_READABLE|AE_WRITABLE,metaServerConnect, connection) ==
            AE_ERR)
    {
        close(fd);
        redisLog(REDIS_WARNING,"Can't create readable event for meta");
        return REDIS_ERR;
    }


    connection->metaServerFd = fd;
    connection->metaServerState = META_SERVER_STATE_CONNECTING;

    return REDIS_OK;
}


void getCtripMetaInfo(){

	int i=0;

	for(i = 0; i < META_SERVER_CONNECTION_COUNT ; i++){

		if(ctrip.connection[i].metaServerState == META_SERVER_STATE_NONE){
			if(connectWithMetaServer(&ctrip.connection[i]) != REDIS_OK){
				continue;
			}
		}
		if(ctrip.connection[i].metaServerState == META_SERVER_STATE_CONNECTED){
			sendGetRequest(&ctrip.connection[i]);
		}
	}
}


void sendGetRequest(metaConnection *connection){



}


void processRedisMasterResponse(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask){

}

void processKeeperMasterResponse(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask){

}

