/*
 * ctrip.c
 *
 *  Created on: May 12, 2016
 *      Author: wenchao.meng
 */

#include "redis.h"

ctripServer ctrip;
lastMaster lmaster;


void clearLastMaster(){

	lmaster.master_runid[REDIS_RUN_ID_SIZE] = '\0';
	lmaster.master_reploff = -1;
	lmaster.currentReploff = -1;
}

void initCtripConfig(){

	ctrip.clusterName = "default";
	ctrip.metaServerUrl = NULL;
	ctrip.shardName = NULL;
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

int loadCtripConfig(sds *argv, int argc){

	if(!strcasecmp(argv[0], CONFIG_NAME_META_SERVER_URL) && argc == 2){
		ctrip.metaServerUrl = zstrdup(argv[1]);
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


//simple http support

