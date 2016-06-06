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
void processMetaResponse(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask);
void handleKeeperResponse(httpResponse *response);
void handleMasterResponse(httpResponse *response);
void processHttpInputBuff(metaConnection *connection);
void handleResponse(metaConnection *connection);
int isLocal(char *host);
void getLocalAddresses();

void clearLastMaster(){

	lmaster.master_runid[REDIS_RUN_ID_SIZE] = '\0';
	lmaster.master_reploff = -1;
	lmaster.currentReploff = -1;
}

void initConnection(metaConnection *connection){

	connection->connectionState = META_SERVER_STATE_NONE;
	connection->metaServerFd = -1;
	connection->receiveBuff = sdsempty();
	connection->sendBuff = NULL;
	connection->httpResponse.httpBody = sdsempty();
	connection->processor = processMetaResponse;
	memset(connection->connectionDesc, 0, CONNECTION_DESC_LENGTH);
}

void initMetaResponse(metaConnection *connection){

	connection->httpResponse.httpResponseState = HTTP_STATE_READ_STATUS;
	connection->httpResponse.currentLen = 0;
	connection->httpResponse.httpBodyType = HTTP_BODY_TYPE_UNKNOWN;
	connection->httpResponse.chunkedState = HTTP_STATE_CHUNKED_READING_LEN;

}

void initCtripConfig(){

	getLocalAddresses();

	ctrip.clusterName = CLUSTER_NAME_DEFAULT;
	ctrip.metaServerUrl = NULL;
	ctrip.shardName = NULL;
	ctrip.metaServerTimeout = META_SERVER_TIME_OUT;



	initConnection(&ctrip.connection[0]);
	ctrip.connection[0].urlFormat = URL_FORMAT_GET_KEEPER_MASTER;
	ctrip.connection[0].responseHandler = handleKeeperResponse;

	initConnection(&ctrip.connection[1]);
	ctrip.connection[1].urlFormat = URL_FORMAT_GET_REDIS_MASTER;
	ctrip.connection[1].responseHandler = handleMasterResponse;

	ctrip.keeper.host = NULL;
	ctrip.keeper.port = -1;
	ctrip.master.host = NULL;
	ctrip.master.port = -1;
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

int getHostPort(sds buff, sds *host, int *dstPort){

	int   port;
	char *colon;

	colon = strchr(buff, ':');
	if(colon == NULL){
		return REDIS_ERR;
	}

	if((colon - buff) > HTTP_MAX_HOST_LENGTH || strlen(colon + 1) > HTTP_MAX_PORT_LENGTH){
		return REDIS_ERR;
	}

	port = atoi(colon + 1);
	if(port <= 0){
		return REDIS_ERR;
	}

	*dstPort = port;
	*host = sdsnewlen(buff, colon - buff);

	return REDIS_OK;
}

void connectToKeeper(){

	if(!ctrip.keeper.host){
		return;
	}

	if(server.masterhost
			&& !strcasecmp(server.masterhost, ctrip.keeper.host) && server.masterport == ctrip.keeper.port){
		redisLog(REDIS_NOTICE, "keeper not changed!");
		return;
	}

	if(!server.masterhost){

		redisLog(REDIS_NOTICE, "connect to new master %s:%d", ctrip.keeper.host, ctrip.keeper.port);
		changeAndLogMaster(ctrip.keeper.host, ctrip.keeper.port);
	}else{
		redisLog(REDIS_NOTICE, "change redis master %s:%d  -> %s:%d",
				server.masterhost, server.masterport, ctrip.keeper.host, ctrip.keeper.port);
		sdsfree(server.masterhost);
		server.masterhost = sdsnew(ctrip.keeper.host);
		server.masterport = ctrip.keeper.port;
		if(server.master){
			freeClient(server.master);
		}

	}
}

int currentRole(){

	if(ctrip.master.host == NULL){
		return CTRIP_CURRENT_ROLE_UNKNOWN;
	}

	if(isLocal(ctrip.master.host) && server.port == ctrip.master.port){
		return CTRIP_CURRENT_ROLE_MASTER;
	}

	return CTRIP_CURRENT_ROLE_SLAVE;
}

void keeperAddressChanged(){

	if(currentRole() == CTRIP_CURRENT_ROLE_SLAVE){
		connectToKeeper();
	}

}

void masterAddressChanged(){

	if(currentRole() == CTRIP_CURRENT_ROLE_SLAVE){
		connectToKeeper();
	}
}

void handleKeeperResponse(httpResponse *response){

	sds host;
	int port;

	if(getHostPort(response->httpBody, &host, &port) == REDIS_ERR){
		redisLog(REDIS_WARNING, "wrong keeper meta response:%s", response->httpBody);
		return;
	}

	if(ctrip.keeper.host == NULL){
		redisLog(REDIS_NOTICE, "find new keeper:%s:%d",host, port);
		ctrip.keeper.host = sdsdup(host);
		ctrip.keeper.port = port;
		keeperAddressChanged();
	}else if(strcasecmp(ctrip.keeper.host, host) || ctrip.keeper.port != port){
		redisLog(REDIS_NOTICE, "keeper changed :%s:%d  -> %s:%d",ctrip.keeper.host, ctrip.keeper.port, host, port);

		sdsfree(ctrip.keeper.host);
		ctrip.keeper.host = sdsdup(host);
		ctrip.keeper.port = port;

		keeperAddressChanged();
	}

	sdsfree(host);
}

void handleMasterResponse(httpResponse *response){

	sds host;
	int port;

	if(getHostPort(response->httpBody, &host, &port) == REDIS_ERR){
		redisLog(REDIS_WARNING, "wrong master meta response:%s", response->httpBody);
		return;
	}

	if(ctrip.master.host == NULL){

		redisLog(REDIS_NOTICE, "find new master:%s:%d", host, port);
		ctrip.master.host = sdsdup(host);
		ctrip.master.port = port;
		masterAddressChanged();
	}else if(strcasecmp(ctrip.master.host, host) || ctrip.master.port != port){
		redisLog(REDIS_NOTICE, "master changed :%s:%d  -> %s:%d",ctrip.master.host, ctrip.master.port, host, port);

		sdsfree(ctrip.master.host);

		ctrip.master.host = sdsdup(host);
		ctrip.master.port = port;

		masterAddressChanged();
	}

	sdsfree(host);
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

void setConnectionState(metaConnection *connection, int state){

	connection->connectionState = state;
	connection->lastIoTime = server.unixtime;

}

void freeMemory(metaConnection *connection){

	sdsclear(connection->receiveBuff);
	sdsclear(connection->httpResponse.httpBody);
}

void closeConnection(metaConnection *connection){

	freeMemory(connection);

	if(connection->metaServerFd == -1){
		redisLog(REDIS_WARNING, "already closed %s", connection->connectionDesc);
		return;
	}

	redisLog(REDIS_NOTICE, "close %s", connection->connectionDesc);

	int fd = connection->metaServerFd;
    aeDeleteFileEvent(server.el,fd,AE_READABLE|AE_WRITABLE);
    close(fd);

    connection->metaServerFd = -1;
    setConnectionState(connection, META_SERVER_STATE_NONE);
}

void metaServerConnect(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask){

	int sockerr = 0;
	socklen_t errlen = sizeof(sockerr);
	metaConnection *connection = clientData;
    char localHost[REDIS_IP_STR_LEN] , remoteHost[REDIS_IP_STR_LEN];
    int  localPort, remotePort;

    REDIS_NOTUSED(mask);

    /* Check for errors in the socket. */
	int sockopt = getsockopt(fd, SOL_SOCKET, SO_ERROR, &sockerr, &errlen);
    if ( sockopt == -1){
        sockerr = errno;
    }
    if (sockerr) {
        redisLog(REDIS_WARNING,"Error connection on socket for metaserver: %s",
            strerror(sockerr));
        closeConnection(connection);
        return;
    }


    aeDeleteFileEvent(eventLoop,fd,AE_READABLE|AE_WRITABLE);
    setConnectionState(connection, META_SERVER_STATE_CONNECTED);


    anetPeerToString(connection->metaServerFd, remoteHost, sizeof(remoteHost), &remotePort);
    anetSockName(connection->metaServerFd, localHost, sizeof(localHost), &localPort);
    sprintf(connection->connectionDesc, "%s:%d->%s:%d", localHost, localPort, remoteHost, remotePort);


    redisLog(REDIS_NOTICE, "Connect to server successed(%s), fd:%d", connection->connectionDesc, fd);
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
    setConnectionState(connection, META_SERVER_STATE_CONNECTING);


    return REDIS_OK;
}


void cronGetCtripMetaInfo(){

	int i=0;
	metaConnection *connection;

	for(i = 0; i < META_SERVER_CONNECTION_COUNT ; i++){

		connection = &ctrip.connection[i];

	    redisLog(REDIS_VERBOSE, "receiveBuff:%lu", sdslen(connection->receiveBuff) + sdsavail(connection->receiveBuff));
	    redisLog(REDIS_VERBOSE, "body:%lu", sdslen(connection->httpResponse.httpBody) + sdsavail(connection->httpResponse.httpBody));

		if(connection->connectionState == META_SERVER_STATE_NONE){
			if(connectWithMetaServer(connection) != REDIS_OK){
				continue;
			}
		}
		if(connection->connectionState == META_SERVER_STATE_CONNECTED){
			sendGetRequest(connection);
		}

		//check time out
		if(connection->connectionState == META_SERVER_STATE_CONNECTING && (time(NULL) - connection->lastIoTime) > ctrip.metaServerTimeout){
			redisLog(REDIS_WARNING, "connect time out %s", ctrip.metaServerUrl);
			closeConnection(connection);
			continue;
		}

		if(connection->connectionState == META_SERVER_STATE_READING_RESPONSE && (time(NULL) - connection->lastIoTime) > ctrip.metaServerTimeout){
			redisLog(REDIS_WARNING, "wait for reponse time out %s", connection->connectionDesc);
			closeConnection(connection);
			continue;
		}
	}
}

void writeRequest(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask){

	metaConnection *connection = clientData;
	int nwritten = 0, sendBuffLen;
	int len = sdslen(connection->sendBuff) - connection->sentlen;

	REDIS_NOTUSED(mask);

    nwritten = write(fd, connection->sendBuff + connection->sentlen, len);
	connection->lastIoTime = server.unixtime;

    if (nwritten <= 0){

        if (nwritten == -1) {
            if (errno == EAGAIN) {
                nwritten = 0;
                return;
            }
        }
        redisLog(REDIS_WARNING,
            "Error writing to client: %s", strerror(errno));
        closeConnection(connection);
        return;
    }

    connection->sentlen += nwritten;
    sendBuffLen = sdslen(connection->sendBuff);
    if(connection->sentlen >= sendBuffLen){

    	setConnectionState(connection, META_SERVER_STATE_READING_RESPONSE);
        aeDeleteFileEvent(eventLoop, fd,AE_WRITABLE);

        initMetaResponse(connection);

        if (aeCreateFileEvent(eventLoop, connection->metaServerFd, AE_READABLE, processMetaResponse, connection) == AE_ERR){
        	closeConnection(connection);
        	redisLog(REDIS_WARNING, "can not add readable event %s", connection->connectionDesc);
        }
    }
}


void sendGetRequest(metaConnection *connection){

	sds buff = sdsnew("GET ");
	buff = sdscatprintf(buff, connection->urlFormat, ctrip.clusterName, ctrip.shardName);
	buff = sdscat(buff, " HTTP/1.1"HTTP_CRLF);
	buff = sdscatprintf(buff, "Host:%s:%d"HTTP_CRLF, ctrip.metaServerHost, ctrip.metaServerPort);
	buff = sdscat(buff, "User-Agent: redis"HTTP_CRLF);
	buff = sdscat(buff, "Accept: */*"HTTP_CRLF);
	buff = sdscat(buff, HTTP_CRLF);

	if(connection->sendBuff != NULL){
		sdsfree(connection->sendBuff);
		connection->sendBuff = NULL;
	}

	connection->sendBuff = buff;
	connection->sentlen = 0;

    if (aeCreateFileEvent(server.el, connection->metaServerFd, AE_WRITABLE, writeRequest, connection) ==
            AE_ERR)
    {
    	closeConnection(connection);
    	return;
    }
}

void processMetaResponse(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask){

	int readlen = REDIS_IOBUF_LEN, nread, qblen;
	metaConnection *connection = clientData;


	REDIS_NOTUSED(eventLoop);
    REDIS_NOTUSED(mask);

	qblen = sdslen(connection->receiveBuff);
	connection->receiveBuff = sdsMakeRoomFor(connection->receiveBuff, readlen);
	connection->lastIoTime = server.unixtime;


    nread = read(fd, connection->receiveBuff + qblen, readlen);
    if (nread == -1) {
        if (errno == EAGAIN) {
            nread = 0;
        } else {
            redisLog(REDIS_VERBOSE, "Reading from client: %s, %s",strerror(errno), connection->connectionDesc);
            closeConnection(connection);
            return;
        }
    } else if (nread == 0) {
        redisLog(REDIS_VERBOSE, "Client closed connection");
        closeConnection(connection);
        return;
    }

    if (nread) {
        sdsIncrLen(connection->receiveBuff, nread);
        processHttpInputBuff(connection);
    }
}

void ctripLog(int level, metaConnection *connection, const char *fmt, ...){

    va_list ap;
    char msg[REDIS_MAX_LOGMSG_LEN];
    int  msglen;

    if ((level&0xff) < server.verbosity) return;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    msglen = strlen(msg);
    sprintf(msg + msglen, "(%s)", connection->connectionDesc);

    redisLogRaw(level,msg);
}

void ctripLogSE(int level, char *start, char *end, char *info, metaConnection *connection){

	char format[100];
	int length = end - start + 1;

	sprintf(format, "%%s:%%.%ds (%%s)", length);
	redisLog(level, format, info, start, connection->connectionDesc);

}

void readStatus(metaConnection *connection){

	sds receiveBuff = connection->receiveBuff;
	httpResponse *httpResponse = &connection->httpResponse;

	char *line;
	int   argc, i, index, statusCode;
	sds   *argv;


	line = strchr(receiveBuff, '\n');
	if(line == NULL){
		return;
	}

	ctripLogSE(REDIS_VERBOSE, receiveBuff, line - 2, "status", connection);

	argv = sdssplitlen(receiveBuff, line - receiveBuff -1 ," ", 1, &argc);

	index = 0;
	for(i=0; i < argc ; i++){

		if(sdslen(argv[i]) == 0){
			continue;
		}

		if(index == 1){
			statusCode = atoi(argv[i]);
			redisLog(REDIS_VERBOSE, "reponse state %s:%d(%s)", argv[i], statusCode, connection->connectionDesc);
			if(statusCode <= 0){
				redisLog(REDIS_WARNING, "meta server return status %s, not legal", line);
			}
			httpResponse->httpStatus.statusCode = statusCode;
			break;
		}
		index++;
	}

	sdsfreesplitres(argv, argc);

	sdsrange(receiveBuff, line - receiveBuff + 1, -1);
	connection->httpResponse.httpResponseState = HTTP_STATE_READ_HEADER;
}

void readHeader(metaConnection *connection){

	char  *line;
	char  *start = connection->receiveBuff;
	int   contentLength;
	char  *colon;



	while(1){
		line = strchr(start, '\n');
		if(line == NULL){
			break;
		}
		if((line - start) == 1){

			ctripLog(REDIS_VERBOSE, connection, "header end");
			if(connection->httpResponse.httpBodyType == HTTP_BODY_TYPE_UNKNOWN){
				ctripLog(REDIS_ERR, connection, "content type not given!");
				closeConnection(connection);
				break;
			}
			connection->httpResponse.httpResponseState = HTTP_STATE_READ_BODY;
			break;
		}

		ctripLogSE(REDIS_VERBOSE, start, line - 2, "header", connection);

		colon = strchr(start, ':');
		if(colon == NULL || colon > line){
			start = line+1;
			redisLog(REDIS_ERR, "header can not find :");
			continue;
		}

		sds key = sdsnewlen(start, colon - start);
		sds value = sdstrim(sdsnewlen(colon + 1, line - colon - 2), " ");

		if(!strcasecmp(key, "Transfer-Encoding") && !strcasecmp(value, "chunked")){
			redisLog(REDIS_VERBOSE, "body chunked(%s)", connection->connectionDesc);
			connection->httpResponse.httpBodyType = HTTP_BODY_TYPE_CHUNKED;
		}

		if(!strcasecmp(key, "Content-Length")){
			contentLength = atoi(value);
			if(contentLength < 0){
				redisLog(REDIS_ERR, "content-length wrong %s", value);
			}else{
				redisLog(REDIS_VERBOSE, "body length(%s)", connection->connectionDesc);
				connection->httpResponse.httpBodyType = HTTP_BODY_TYPE_LENGTH;
				connection->httpResponse.contentLength = contentLength;
			}
		}

		start = line + 1;
		sdsfree(key);
		sdsfree(value);
	}

	sdsrange(connection->receiveBuff, (line - connection->receiveBuff) + 1, -1);
}

void readLenBody(metaConnection *connection){

	sds buff = connection->receiveBuff;
	httpResponse *response = &connection->httpResponse;
	int readlen = 0, finished = 0;

	redisLog(REDIS_VERBOSE, "body:%s", connection->receiveBuff);

	readlen = sdslen(buff);
	if(readlen >= (response->contentLength - response->currentLen)){
		readlen = response->contentLength - response->currentLen;
		finished = 1;
	}

	response->currentLen += readlen;
	response->httpBody = sdscatlen(response->httpBody, buff, readlen);
	sdsrange(buff, readlen, -1);

	if(finished){
		handleResponse(connection);
	}
}

void handleResponse(metaConnection *connection){

	if(connection->httpResponse.httpStatus.statusCode == HTTP_STATUS_CODE_200){

		ctripLog(REDIS_VERBOSE, connection, "begin handleResponse(%s)", connection->httpResponse.httpBody);
		connection->responseHandler(&connection->httpResponse);
	}else{
		ctripLog(REDIS_WARNING, connection, "statusCode:%d", connection->httpResponse.httpStatus.statusCode);
	}

	freeMemory(connection);
	//request again
	connection->connectionState = META_SERVER_STATE_CONNECTED;
}

void readChunkedBody(metaConnection *connection){

	httpResponse *response = &connection->httpResponse;
	int   len, cutLen;
	char *start, *line;

	ctripLog(REDIS_VERBOSE, connection, "body:%s", connection->receiveBuff);

	while(1){

		if(response->chunkedState == HTTP_STATE_CHUNKED_READING_LEN){

			start = connection->receiveBuff;
			line = strstr(start, HTTP_CRLF);
			if(line == NULL){
				break;
			}

			len = (int)strtol(start, NULL, 16);
			if(len <= 0){
				//body ended!
				handleResponse(connection);
				break;
			}

			ctripLog(REDIS_VERBOSE, connection, "chunked len:%d", len);

			response->chunkedState = HTTP_STATE_CHUNKED_READING_BODY;
			response->chunkedCurrentTotalLen = len;
			response->currentLen = 0;
			sdsrange(connection->receiveBuff, line - connection->receiveBuff + 2, -1);
		}

		int   shouldBreak = 0;
		if(response->chunkedState == HTTP_STATE_CHUNKED_READING_BODY){

			cutLen = sdslen(connection->receiveBuff);
			if(cutLen >= (response->chunkedCurrentTotalLen - response->currentLen)){
				cutLen = response->chunkedCurrentTotalLen - response->currentLen;
			}else{
				shouldBreak = 1;
			}

			ctripLogSE(REDIS_VERBOSE, connection->receiveBuff, connection->receiveBuff + cutLen - 1, "chunked", connection);

			response->httpBody = sdscatlen(response->httpBody, connection->receiveBuff, cutLen);
			ctripLog(REDIS_VERBOSE, connection, "body:%s", response->httpBody);

			response->currentLen += cutLen;
			sdsrange(connection->receiveBuff, cutLen, -1);
			if(shouldBreak){
				break;
			}
			response->chunkedState = HTTP_STATE_CHUNKED_READING_CRLF;
		}

		if(response->chunkedState == HTTP_STATE_CHUNKED_READING_CRLF){

			sds buff = connection->receiveBuff;
			if(sdslen(buff) < 2){
				break;
			}

			if(buff[0] == '\r' && buff[1] == '\n'){
				sdsrange(connection->receiveBuff, 2, -1);
			}
			response->chunkedState = HTTP_STATE_CHUNKED_READING_LEN;
		}
	}
}

void readBody(metaConnection *connection){

	if(connection->httpResponse.httpBodyType == HTTP_BODY_TYPE_LENGTH){
		readLenBody(connection);
	}else if(connection->httpResponse.httpBodyType == HTTP_BODY_TYPE_CHUNKED){
		readChunkedBody(connection);
	}else{
		redisLog(REDIS_ERR, "unsupported content type:%d(%s)", connection->httpResponse.httpBodyType, connection->connectionDesc);
		closeConnection(connection);
	}
}

void processHttpInputBuff(metaConnection *connection){

	if(connection->httpResponse.httpResponseState == HTTP_STATE_READ_STATUS){
		readStatus(connection);
	}

	if(connection->httpResponse.httpResponseState == HTTP_STATE_READ_HEADER){
		readHeader(connection);
	}

	if(connection->httpResponse.httpResponseState == HTTP_STATE_READ_BODY){
		readBody(connection);
	}
}

int isLocal(char *host){
	int i = 0;

	if(host == NULL){
		return 0;
	}

	for(i = 0; i < ctrip.localAddressCount; i++){
		if(!strcasecmp(host, ctrip.localAddresses[i])){
			return 1;
		}
	}

	return 0;
}

void getLocalAddresses(){

	int    slots = 5, addressSize = 0;
	sds    *addresses;

	struct ifaddrs *ifAddrStruct=NULL;
	void * tmpAddrPtr=NULL;

	addresses = zmalloc(slots * sizeof(sds));
	getifaddrs(&ifAddrStruct);

	while (ifAddrStruct!=NULL) {

		if (ifAddrStruct->ifa_addr->sa_family==AF_INET) { // check it is IP4
	            // is a valid IP4 Address
	            tmpAddrPtr=&((struct sockaddr_in *)ifAddrStruct->ifa_addr)->sin_addr;
	            char addressBuffer[INET_ADDRSTRLEN];
	            inet_ntop(AF_INET, tmpAddrPtr, addressBuffer, INET_ADDRSTRLEN);

	            addresses[addressSize] = sdsnewlen(addressBuffer, strlen(addressBuffer));
	            addressSize++;
		} else if (ifAddrStruct->ifa_addr->sa_family==AF_INET6) { // check it is IP6
	            // is a valid IP6 Address
	            tmpAddrPtr=&((struct sockaddr_in *)ifAddrStruct->ifa_addr)->sin_addr;
	            char addressBuffer[INET6_ADDRSTRLEN];
	            inet_ntop(AF_INET6, tmpAddrPtr, addressBuffer, INET6_ADDRSTRLEN);
	            addresses[addressSize] = sdsnewlen(addressBuffer, strlen(addressBuffer));
	            addressSize++;
		}

		ifAddrStruct=ifAddrStruct->ifa_next;
		if(addressSize == slots){
			slots *= 2;
			addresses = zrealloc(addresses,  slots *sizeof(sds));
		}
	}

	ctrip.localAddressCount = addressSize;
	ctrip.localAddresses = addresses;


	for(int i=0;i<ctrip.localAddressCount;i++){
		redisLog(REDIS_NOTICE, "localAddress:%s", ctrip.localAddresses[i]);
	}
}

