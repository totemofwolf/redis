/*
 * ctrip.c
 *
 *  Created on: May 12, 2016
 *      Author: wenchao.meng
 */

#include "redis.h"

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
