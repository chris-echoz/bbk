#include "echoclienttask.h"
#include "echoclientconnection.h"

double EchoClientTask::start() {
    log() << "Will create EchoClientConnection";
    if (!addConnection(new EchoClientConnection(this, _hostname, _port)))
        setError("Cannot find server");
    return 1.0;
}

double EchoClientTask::timerEvent() {
    if (elapsed() > 15.0) {
        setResult("Timeout!");
        return 0;
    }
    log() << "Create another EchoClientConnection";
    if (!addConnection(new EchoClientConnection(this, _hostname, _port)))
        setError("Cannot find server");
    return 1.0;
}
