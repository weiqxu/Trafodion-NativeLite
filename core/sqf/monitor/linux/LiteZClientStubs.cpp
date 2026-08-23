#include "lock.h"
#include "zclient.h"

void FreeStringVector(struct String_vector *v)
{
    if (v != NULL)
    {
        v->count = 0;
        v->data = NULL;
    }
}

CZClient::CZClient(const char *quorumHosts,
                   const char *rootNode,
                   const char *instanceNode)
    : eyecatcher_(0)
    , threadId_(0)
    , state_(ZC_DISABLED)
    , enabled_(false)
    , clusterWatchEnabled_(false)
    , resetMyZNodeFailedTime_(true)
    , shutdown_(false)
    , zcMonitoringRate_(0)
    , zkQuorumHosts_(quorumHosts ? quorumHosts : "")
    , zkRootNode_(rootNode ? rootNode : "")
    , zkRootNodeInstance_(instanceNode ? instanceNode : "")
    , zkQuorumPort_("")
    , zkSessionTimeout_(60)
{
}

CZClient::~CZClient(void)
{
}

int CZClient::ConfiguredZNodeCreate(const char *) { return ZOK; }
int CZClient::ConfiguredZNodeDelete(const char *) { return ZOK; }
int CZClient::ConfiguredZNodeWatchAdd(void) { return ZOK; }
int CZClient::ConfiguredZNodeWatchDelete(void) { return ZOK; }
void CZClient::ConfiguredZNodesDelete(void) {}
int CZClient::ConfiguredZNodesGet(String_vector *children)
{
    FreeStringVector(children);
    return ZOK;
}

int CZClient::ErrorZNodeCreate(const char *) { return ZOK; }
int CZClient::ErrorZNodeDelete(const char *) { return ZOK; }
int CZClient::ErrorZNodeWatchAdd(void) { return ZOK; }
int CZClient::ErrorZNodeWatchDelete(void) { return ZOK; }
void CZClient::ErrorZNodesDelete(void) {}
int CZClient::ErrorZNodesGet(String_vector *children, bool)
{
    FreeStringVector(children);
    return ZOK;
}
int CZClient::ErrorZNodesGetChild(const char *, String_vector *children)
{
    FreeStringVector(children);
    return ZOK;
}
void CZClient::HandleErrorChildZNodesForZNodeChild(const char *, bool) {}

bool CZClient::IsRunningZNodeExpired(const char *, int &zerr)
{
    zerr = ZOK;
    return false;
}

const char *CZClient::MasterWaitForAndReturn(bool)
{
    return "";
}

int CZClient::MasterZNodeCreate(const char *) { return ZOK; }
int CZClient::MasterZNodeDelete(const char *) { return ZOK; }
void CZClient::MonitorCluster(void) {}
int CZClient::RunningZNodeDelete(const char *) { return ZOK; }
int CZClient::RunningZNodeWatchAdd(const char *) { return ZOK; }
void CZClient::RunningZNodesDelete(void) {}

int CZClient::ShutdownWork(void)
{
    shutdown_ = true;
    state_ = ZC_SHUTDOWN;
    return ZOK;
}

void CZClient::StartMonitoring(void)
{
    state_ = ZC_DISABLED;
}

int CZClient::StartWork(void)
{
    return ZOK;
}

void CZClient::StopMonitoring(void)
{
    state_ = ZC_DISABLED;
}

void CZClient::StateSet(ZClientState_t state)
{
    state_ = state;
    if (state == ZC_SHUTDOWN)
        shutdown_ = true;
}

void CZClient::StateSet(int, ZClientState_t state, const char *)
{
    StateSet(state);
}

void CZClient::TriggerCheck(int, const char *)
{
}
