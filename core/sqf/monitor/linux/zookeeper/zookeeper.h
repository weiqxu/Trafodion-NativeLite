#ifndef LOCAL_LITE_ZOOKEEPER_H_
#define LOCAL_LITE_ZOOKEEPER_H_

#ifdef TRAF_LOCAL_LITE

#include <stdint.h>

#define ZOK 0
#define ZNONODE -101
#define ZNODEEXISTS -110

#define ZOO_EPHEMERAL 1
#define ZOO_SEQUENCE 2

#define ZOO_CREATED_EVENT 1
#define ZOO_DELETED_EVENT 2
#define ZOO_CHANGED_EVENT 3
#define ZOO_CHILD_EVENT 4
#define ZOO_SESSION_EVENT -1
#define ZOO_NOTWATCHING_EVENT -2

#define ZOO_EXPIRED_SESSION_STATE -112
#define ZOO_AUTH_FAILED_STATE -113
#define ZOO_CONNECTING_STATE 1
#define ZOO_ASSOCIATING_STATE 2
#define ZOO_CONNECTED_STATE 3

typedef struct _zhandle zhandle_t;

typedef struct {
    int64_t client_id;
    char passwd[16];
} clientid_t;

struct Stat {
    int64_t czxid;
    int64_t mzxid;
    int64_t ctime;
    int64_t mtime;
    int32_t version;
    int32_t cversion;
    int32_t aversion;
    int64_t ephemeralOwner;
    int32_t dataLength;
    int32_t numChildren;
    int64_t pzxid;
};

struct String_vector {
    int32_t count;
    char **data;
};

static inline const char *zerror(int)
{
    return "ZooKeeper disabled in local-lite";
}

#else
#include_next <zookeeper/zookeeper.h>
#endif

#endif
