/*
 * ctrip.c
 *
 *  Created on: May 12, 2016
 *      Author: wenchao.meng
 */

#include "redis.h"

lastMaster lmaster;


void clearLastMaster(){

	lmaster.master_runid[REDIS_RUN_ID_SIZE] = '\0';
	lmaster.master_reploff = -1;
	lmaster.currentReploff = -1;
}

void initCtripConfig(){

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
