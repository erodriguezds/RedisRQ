#include <sys/time.h>
#include "../redismodule.h"
#include "../rmutil/util.h"
#include "../rmutil/strings.h"
#include "../rmutil/test_util.h"
#include "./module.h"
#include "./rqueue.h"

static RedisModuleType *RQueueRedisType;
#define RQUEUE_ENCODING_VERSION 0

/**
 * Return info on a given queue
 */
int qinfoCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
	RedisModule_AutoMemory(ctx);
	if (argc != 2) return RedisModule_WrongArity(ctx);
	RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
	int type = RedisModule_KeyType(key);
	if(type == REDISMODULE_KEYTYPE_EMPTY){
		return RedisModule_ReplyWithError(ctx, ERRORMSG_EMPTYKEY);
	}

	if(RedisModule_ModuleTypeGetType(key) != RQueueRedisType){
		return RedisModule_ReplyWithError(ctx,REDISMODULE_ERRORMSG_WRONGTYPE);
	}

	rqueue_t *rqueue = RedisModule_ModuleTypeGetValue(key);

	RedisModule_ReplyWithArray(ctx,6);

	RedisModule_ReplyWithCString(ctx, "last-id");
	RedisModule_ReplyWithString(
		ctx,
		RedisModule_CreateStringPrintf(ctx, MSG_ID_FORMAT, rqueue->last_id.ms, rqueue->last_id.seq)
	);

	RedisModule_ReplyWithCString(ctx, "undelivered-queue-length");
	RedisModule_ReplyWithLongLong(ctx, rqueue->undelivered.len);

	RedisModule_ReplyWithCString(ctx, "delivered-queue-length");
	RedisModule_ReplyWithLongLong(ctx, rqueue->delivered.len);

	return REDISMODULE_OK;
}


/**
 * mq.push <key> <msg1> [ <msg2> [...]]
 * Pushes 1 or more items into key
 */
int pushCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
	RedisModule_AutoMemory(ctx); /* Use automatic memory management. */

	rqueue_t *rqueue;
	struct Msg *newmsg;
	int count = argc - 2; // count of new messages being pushed

	if (argc < 3) return RedisModule_WrongArity(ctx);


   RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ|REDISMODULE_WRITE);
   int type = RedisModule_KeyType(key);
   
	if(type == REDISMODULE_KEYTYPE_EMPTY){
		// Key doesn't exist. Create...
		rqueue = RedisModule_Alloc(sizeof(*rqueue));
		rqueue->last_id.ms = 0;
    	rqueue->last_id.seq = 0;
		initQueue(&rqueue->undelivered);
		initQueue(&rqueue->delivered);
		RedisModule_ModuleTypeSetValue(key, RQueueRedisType, rqueue);
	} else if(RedisModule_ModuleTypeGetType(key) == RQueueRedisType){
		// key exists. Get and update
		rqueue = RedisModule_ModuleTypeGetValue(key);
	} else {
		// Key exists, but it's not an RQueueObject!!!
		return RedisModule_ReplyWithError(ctx,REDISMODULE_ERRORMSG_WRONGTYPE);
	}

	// create the new message
	newmsg = RedisModule_Alloc(sizeof(*newmsg) * count);

	// Init the new nodes
	RedisModule_ReplyWithArray(ctx, count);
	for(int i = 0, j = 1; i < count; i++, j++){
		if(i == 0){
			setNextMsgID(&rqueue->last_id, &newmsg[i].id);
		} else {
			setNextMsgID(&newmsg[i - 1].id, &newmsg[i].id);
		}
		newmsg[i].lastDelivery = 0;
		newmsg[i].deliveries = 0;
		newmsg[i].next = (
			j < count ?
			&newmsg[j] :
			NULL
		);
		newmsg[i].value = RedisModule_CreateStringFromString(NULL, argv[2 + i]);
		RedisModule_ReplyWithString(
			ctx,
			RedisModule_CreateStringPrintf(ctx, MSG_ID_FORMAT, newmsg[i].id.ms, newmsg[i].id.seq)
		);
	}

	//Update first node
	if(rqueue->undelivered.first == NULL){
		rqueue->undelivered.first = &newmsg[0];
	} else {
		rqueue->undelivered.last->next = &newmsg[0];
	}

	// Update last node
	rqueue->undelivered.last = &newmsg[count - 1];

	// Update count
	rqueue->undelivered.len += count;

	// Update last_id
	rqueue->last_id = newmsg[count - 1].id;

	return REDISMODULE_OK;
}

int rangeCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
	RedisModule_AutoMemory(ctx); /* Use automatic memory management. */

	if (argc < 4){
		return RedisModule_WrongArity(ctx);
	}

	RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
   
	if(RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY){
		return RedisModule_ReplyWithError(ctx,ERRORMSG_EMPTYKEY);
	}

	if(RedisModule_ModuleTypeGetType(key) != RQueueRedisType){
		return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
	}

	rqueue_t *rqueue = RedisModule_ModuleTypeGetValue(key);

	struct Msg *cur = (
		RMUtil_StringEqualsCaseC(argv[2], "PENDING") ?
		rqueue->delivered.first :
		rqueue->undelivered.first
	);

	long total = 0;
	RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);
	while(cur){
		RedisModule_ReplyWithArray(ctx,2);
		RedisModule_ReplyWithString(
			ctx,
			RedisModule_CreateStringPrintf(ctx, MSG_ID_FORMAT, cur->id.ms, cur->id.seq)
		);
		//RedisModule_ReplyWithLongLong(ctx, cur->lastDelivery);
		//RedisModule_ReplyWithLongLong(ctx, cur->deliveries);
		RedisModule_ReplyWithString(ctx, cur->value);
		total++;
		cur = cur->next;
	}
	RedisModule_ReplySetArrayLength(ctx, total);
	
	return REDISMODULE_OK;
}

/**
 * Usage: POP <count> <key>
 * 
 * Pops <count> elements from the reliable queue at <key>.
 * The poped elements are placed into the internal "delivered" list, for
 * later acknoledgment.
 */
int popCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
	RedisModule_AutoMemory(ctx);

	if (argc < 3) return RedisModule_WrongArity(ctx);

	RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[2], REDISMODULE_READ|REDISMODULE_WRITE);
    int type = RedisModule_KeyType(key);
   
	if(type == REDISMODULE_KEYTYPE_EMPTY){
		return RedisModule_ReplyWithNull(ctx);
	}
	
	if(RedisModule_ModuleTypeGetType(key) != RQueueRedisType){
		return RedisModule_ReplyWithError(ctx,REDISMODULE_ERRORMSG_WRONGTYPE);
	}

	// Retrieve the key content
	long long count = 1, actually_poped = 0;

	if(RMUtil_ParseArgs(argv, argc, 1, "l", &count) != REDISMODULE_OK){
		return RedisModule_ReplyWithError(ctx, "usage: MQ.POP <count:uint> <key:string>");
	}

	if(count <= 0){
		return RedisModule_ReplyWithError(ctx, "count must be >= 1");
	}

	rqueue_t *rqueue = RedisModule_ModuleTypeGetValue(key);

	if(rqueue->undelivered.first == NULL){
		return RedisModule_ReplyWithNull(ctx);
	}

	struct Msg *topop = rqueue->undelivered.first,
		*next;

	RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);
	while (count > 0 && topop != NULL)
	{
		next = topop->next;
		topop->lastDelivery = mstime();
		topop->deliveries += 1;

		// Update "undelivered" queue
		rqueue->undelivered.first = topop->next;
		rqueue->undelivered.len -= 1;

		// Update "delivered" queue
		if(rqueue->delivered.first == NULL || rqueue->delivered.last == NULL){
			rqueue->delivered.first = rqueue->delivered.last = topop;
		} else {
			rqueue->delivered.last->next = topop;
			rqueue->delivered.last = topop;
		}
		rqueue->delivered.len += 1;
		topop->next = NULL;

		// Finally: reply with a 2-element-array with: MsgID and the payload
		RedisModule_ReplyWithArray(ctx, 2);
		RedisModule_ReplyWithString(
			ctx,
			RedisModule_CreateStringPrintf(ctx, MSG_ID_FORMAT, topop->id.ms, topop->id.seq)
		);
		RedisModule_ReplyWithString(ctx, topop->value);

		// Move-on to the next element to pop
		count -= 1;
		actually_poped += 1;
		topop = next;
	}

	RedisModule_ReplySetArrayLength(ctx, actually_poped);

	return REDISMODULE_OK;
}


/**
 * ACK <queue> <msgid1> [ <msgid2> [ ... ] ]
 * 
 * Acknowledges the successful processing of 1 or more messages, removing them
 * from the internal "delivered" queue.
 * 
 * Returns: ARRAY of messages ID's found and removed
 */
int ackCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
	RedisModule_AutoMemory(ctx);

	if (argc < 3) return RedisModule_WrongArity(ctx);

	// Retrieve the key content
	RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ|REDISMODULE_WRITE);
    int type = RedisModule_KeyType(key);
   
	if(type == REDISMODULE_KEYTYPE_EMPTY){
		return RedisModule_ReplyWithNull(ctx);
	}
	
	if(RedisModule_ModuleTypeGetType(key) != RQueueRedisType){
		return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
	}

	rqueue_t *rqueue = RedisModule_ModuleTypeGetValue(key);

	if(rqueue->delivered.first == NULL){
		return RedisModule_ReplyWithArray(ctx,0);
	}

	struct Msg *cur, *prev, *next;
	struct MsgID id;
	char *idptr;
	size_t idlen;
	char idbuf[128];
	long removed = 0;
	
	RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);

	for(int i = 2; i < argc; i++){
		idptr = RedisModule_StringPtrLen(argv[i], &idlen);
		memcpy(idbuf, idptr, idlen);
		idbuf[idlen < 128 ? idlen : 127] = (char) 0;
		sscanf(idbuf, MSG_ID_FORMAT, &id.ms, &id.seq);
		cur = rqueue->delivered.first;
		prev = NULL;
		while(cur){
			next = cur->next;
			if(cur->id.ms == id.ms && cur->id.seq == id.seq){

				if(cur == rqueue->delivered.first){
					rqueue->delivered.first = cur->next;
				}

				if(cur == rqueue->delivered.last){
					rqueue->delivered.last = prev;
				}
				
				if(prev != NULL){
					// Link previous node to next node
					prev->next = cur->next;
				}
				
				RedisModule_FreeString(NULL, cur->value);
				RedisModule_Free(cur);
				removed++;
				rqueue->delivered.len -= 1;
				RedisModule_ReplyWithString(ctx, argv[i]);
				break;
			}
			prev = cur;
			cur = next;
		};
	}

	RedisModule_ReplySetArrayLength(ctx, removed);

	return REDISMODULE_OK;
}






//TODO
void QueueRDBSave(RedisModuleIO *io, void *ptr) {
}

//TODO: Implement RDBLoad
void *QueueRDBLoad(RedisModuleIO *io, int encver) {
	return NULL;
}

void QueueAofRewrite(RedisModuleIO *aof, RedisModuleString *key, void *value) {
   rqueue_t *rqueue = value;
   struct Msg *node;
	 
	node = rqueue->undelivered.first;
   while(node) {
   	RedisModule_EmitAOF(aof,"mq.push","sl",key,node->value);
   	node = node->next;
   }
}

void QueueReleaseObject(void *value) {
	rqueue_t *rqueue = value;
	struct Msg *cur, *next;

	 // Free all undelivered message
    cur = rqueue->undelivered.first;
    while(cur) {
		next = cur->next;
		RedisModule_FreeString(NULL, cur->value);
        RedisModule_Free(cur);
        cur = next;
    }

	 // Free all delivered message
    cur = rqueue->delivered.first;
    while(cur) {
        next = cur->next;
        RedisModule_Free(cur);
        cur = next;
    }

    RedisModule_Free(value);
}


/* EXAMPLE.PARSE [SUM <x> <y>] | [PROD <x> <y>]
*  Demonstrates the automatic arg parsing utility.
*  If the command receives "SUM <x> <y>" it returns their sum
*  If it receives "PROD <x> <y>" it returns their product
*/
int ParseCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
	RedisModule_AutoMemory(ctx);

  // we must have at least 4 args
  if (argc < 4) {
    return RedisModule_WrongArity(ctx);
  }

  // init auto memory for created strings
  
  long long x, y;

  // If we got SUM - return the sum of 2 consecutive arguments
  if (RMUtil_ParseArgsAfter("SUM", argv, argc, "ll", &x, &y) ==
      REDISMODULE_OK) {
    RedisModule_ReplyWithLongLong(ctx, x + y);
    return REDISMODULE_OK;
  }

  // If we got PROD - return the product of 2 consecutive arguments
  if (RMUtil_ParseArgsAfter("PROD", argv, argc, "ll", &x, &y) ==
      REDISMODULE_OK) {
    RedisModule_ReplyWithLongLong(ctx, x * y);
    return REDISMODULE_OK;
  }

  // something is fishy...
  RedisModule_ReplyWithError(ctx, "Invalid arguments");

  return REDISMODULE_ERR;
}





// Test the the PARSE command
int testParse(RedisModuleCtx *ctx) {

  RedisModuleCallReply *r =
      RedisModule_Call(ctx, "example.parse", "ccc", "SUM", "5", "2");
  RMUtil_Assert(RedisModule_CallReplyType(r) == REDISMODULE_REPLY_INTEGER);
  RMUtil_AssertReplyEquals(r, "7");

  r = RedisModule_Call(ctx, "example.parse", "ccc", "PROD", "5", "2");
  RMUtil_Assert(RedisModule_CallReplyType(r) == REDISMODULE_REPLY_INTEGER);
  RMUtil_AssertReplyEquals(r, "10");
  return 0;
}

// test the HGETSET command
int testHgetSet(RedisModuleCtx *ctx) {
  RedisModuleCallReply *r =
      RedisModule_Call(ctx, "example.hgetset", "ccc", "foo", "bar", "baz");
  RMUtil_Assert(RedisModule_CallReplyType(r) != REDISMODULE_REPLY_ERROR);

  r = RedisModule_Call(ctx, "example.hgetset", "ccc", "foo", "bar", "bag");
  RMUtil_Assert(RedisModule_CallReplyType(r) == REDISMODULE_REPLY_STRING);
  RMUtil_AssertReplyEquals(r, "baz");
  r = RedisModule_Call(ctx, "example.hgetset", "ccc", "foo", "bar", "bang");
  RMUtil_AssertReplyEquals(r, "bag");
  return 0;
}

// Unit test entry point for the module
int TestModule(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  RedisModule_AutoMemory(ctx);

  RMUtil_Test(testParse);
  RMUtil_Test(testHgetSet);

  RedisModule_ReplyWithSimpleString(ctx, "PASS");
  return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx) {

	// Register the module itself
	if (RedisModule_Init(ctx, "mq", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR) {
		return REDISMODULE_ERR;
	}

	// Register the ReliableQueue Type
	RedisModuleTypeMethods tm = {
		.version = REDISMODULE_TYPE_METHOD_VERSION,
    	.rdb_load = QueueRDBLoad,
    	.rdb_save = QueueRDBSave,
    	.aof_rewrite = QueueAofRewrite,
    	.free = QueueReleaseObject
	};

   RQueueRedisType = RedisModule_CreateDataType(
		ctx,
		"Queue-EDU",
		RQUEUE_ENCODING_VERSION,
		&tm
	);

   if (RQueueRedisType == NULL) return REDISMODULE_ERR;

	if (RedisModule_CreateCommand(ctx,"mq.push", pushCommand,"write deny-oom",1,1,1) == REDISMODULE_ERR)
      return REDISMODULE_ERR;

	if (RedisModule_CreateCommand(ctx,"mq.pop", popCommand,"write",2,2,1) == REDISMODULE_ERR)
		return REDISMODULE_ERR;

	if (RedisModule_CreateCommand(ctx,"mq.ack", ackCommand,"write",1,1,1) == REDISMODULE_ERR)
		return REDISMODULE_ERR;

	// register xq.info - the default registration syntax
	if (RedisModule_CreateCommand(ctx, "mq.qinfo", qinfoCommand, "readonly", 1, 1, 1) == REDISMODULE_ERR) {
		return REDISMODULE_ERR;
	}

	// register xq.parse - the default registration syntax
	if (RedisModule_CreateCommand(ctx, "mq.range", rangeCommand, "readonly", 1, 1, 1) == REDISMODULE_ERR) {
		return REDISMODULE_ERR;
	}

	// register the unit test
	RMUtil_RegisterWriteCmd(ctx, "mq.test", TestModule);
	

	return REDISMODULE_OK;
}
