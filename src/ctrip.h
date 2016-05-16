/*
 * ctrip.h
 *
 *  Created on: May 12, 2016
 *      Author: wenchao.meng
 */

#ifndef CTRIP_H_
#define CTRIP_H_

#define REDIS_FAKESLAVE (1<<31)

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

#endif /* CTRIP_H_ */


