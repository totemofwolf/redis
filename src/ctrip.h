/*
 * ctrip.h
 *
 *  Created on: May 12, 2016
 *      Author: wenchao.meng
 */

#ifndef CTRIP_H_
#define CTRIP_H_

#define REDIS_FAKESLAVE (1<<31)

void doFakeSync(redisClient *c);

#endif /* CTRIP_H_ */


