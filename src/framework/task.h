// Copyright (c) 2019 Internetstiftelsen
// Written by Göran Andersson <initgoran@gmail.com>

// This is essentially an abstract base class. You must create subclasses
// and pass subclass objects to the EventLoop's addTask method.
// The purpose of a task is to manage socket connections, and/or to
// execute timers.
//
// When the EventLoop starts executing the task, it will call its start method.
//
// When the task is done, it should notify the EventLoop by calling the
// setResult method. Then the task's parent will be notified and the task
// will be deleted. The task may pass "messages" to its parent by calling
// the setMessage method.
//
// Timers are very simple; the start method must return the number of seconds
// until the timerEvent method should be called. The overridden timerEvent can
// do whatever it needs and then return the number of seconds until it should be
// called again. A value <= 0 means it will never be called again.
//
// Note that the application will be single threaded. Thus, timers (and other
// callbacks) must avoid blocking and generally try do be as quick as possible,
// otherwise all subsequent timers will be delayed.
//
// The Task may create SocketConnection objects and add then using the
// addConnection method. It may also create ServerSocket objects and
// add them with addServer; whenever a client connects to the server,
// you will be notified through the newClient method.

#pragma once

#include <set>
#include <fstream>
#include "logger.h"
#include "taskconfig.h"
#include "eventloop.h"

class Socket;
class ServerSocket;
class SocketConnection;
enum class PollState;
class SocketReceiver;
class WorkerProcess;

class Task : public Logger {
public:
    // Create a task with the given task_name.
    Task(const std::string &task_name);

    virtual ~Task();

    // TODO global default. DBG, INFO, WARN, ERR, NONE
    // dbg_log måste aktiveras vid kompileringen
    // STATIC_LOGLEVEL = INFO  DYNAMIC_LOGLEVEL = ... per task!
    // Man ska kunna klippa mer loggning vid kompileringen, per modul!
    // conn, child tasks ärver DYNAMIC_LOGLEVEL
    //void setLogLevel(Logger::LogLevel lvl);

    // Return number of seconds until timerEvent should
    // be called, or <= 0 if you don't want it to be called.
    virtual double start() {
        dbg_log() << "Task starting. No timer.";
        return 0;
    }

    // Return number of seconds until this method should be
    // called again, or <= 0 if you don't want it to be called again.
    virtual double timerEvent() {
        dbg_log() << "Default timerEvent: will kill task.";
        setTimeout();
        return 0;
    }

    // Run timerEvent after s seconds instead of previous value.
    void resetTimer(double s) {
        supervisor->resetTimer(this, s);
    }

    // Return true if task is finished.
    // Task will be deleted unless return value is false.
    // By default, this will return true if the task has called
    // the setResult method.
    virtual bool isFinished() {
        return is_finished;
    }

    // Return true if the task has finished normally.
    // This method is meant to be used in the taskFinished callback.
    bool finishedOK() const {
        return (has_started && is_finished && !was_killed &&
                !was_error && !was_timeout);
    }

    // Return true if the task is finished and was aborted by another task.
    // This method is meant to be used in the taskFinished callback.
    bool wasKilled() const {
        return was_killed;
    }

    bool wasError() const {
        return was_error;
    }

    bool wasTimeout() const {
        return was_timeout;
    }

    bool hasStarted() const {
        return has_started;
    }

    // Call this in the start() callback if you want all child tasks to be
    // killed when this task is finished.
    void killChildTaskWhenFinished() {
        kill_children = true;
    }

    // Ignore this unless the task is a server task.
    // If a new remote connection is made through any ServerSocket object
    // owned by us, the client socket, ip address and port number will
    // be passed to the below method. Override it to create and return
    // an object of a subclass to SocketConnection, otherwise the client
    // socket will be closed. The object will be owned by the implementation
    // and will be deleted when the connection has been closed. You _must_
    // create the object with new.
    virtual SocketConnection *newClient(int, const char *, uint16_t,
                                        ServerSocket *) {
        return nullptr;
    }

    // Request for me to adopt a socket owned by some other task.
    // Return false to reject. Otherwise set me as owner and return true.
    virtual bool adoptConnection(Socket *conn);

    // This will be called to notify us when a new client socket object
    // has been successfully added to this task.
    // If you need to know that, override this method.
    virtual void connAdded(SocketConnection *) {
    }

    // This will be called when a client socket object has been removed from
    // this task, just before it is deleted.
    // If you need to know that, override this method.
    virtual void connRemoved(SocketConnection *) {
    }

    // This will be called to notify us when a new server (listening) socket
    // object has been successfully added to this task.
    // If you need to know that, override this method.
    virtual void serverAdded(ServerSocket *) {
    }

    // This will be called when a server socket object has been removed from
    // this task, just before it is deleted.
    // If you need to know that, override this method.
    virtual void serverRemoved(ServerSocket *) {
    }

    // To get the "result" of the task after it has finished.
    std::string result() const {
        return the_result;
    }

    std::set<Socket *> getMyConnections() const;

    // Return true if the connection still exists.
    bool isActive(Socket *conn) const {
        return supervisor->isActive(conn);
    }

    // Restart all idle connections
    void wakeUp();

    // If s is idle, restart it and return true. Otherwise return false.
    bool wakeUpConnection(SocketConnection *s) {
        return supervisor->wakeUpConnection(s);
    }

    void cancelConnection(SocketConnection *s) {
        supervisor->cancelConnection(s);
    }

    std::string message() const {
        return the_message;
    }

    // Notify that this task will be observing task "to". This task will
    // be notified through a call to taskFinished if "to" terminates before me.
    // (A parent task is observing its child tasks by default.)
    bool startObserving(Task *to) {
        return supervisor->startObserving(this, to);
    }

    // Execute receiver's handleExecution method immediately. The call will be
    // ignored unless this task is observing the receiver.
    // Note that you could also call receiver->handleExecution() (or any
    // other method in receiver) directly. However, that might be dangerous
    // since your pointer to the receiver is a _weak reference_ and you
    // must somehow make sure that the receiver still exists.
    // The advantages of using this API instead of directly calling
    // methods in the receiver task are:
    //   1. Safer; the call will be ignored if the receiver task doesn't exist.
    //   2. The sender and receiver classes do not have to know each other.
    //   3. Since you are observing the receiver, you will be notified
    //      when the receiver task terminates.
    // Note: Essentially, the receiver's handleExecution method executes from
    //       within the sender's method (event handler). If the receiver in any
    //       way modifies the sender from within its handleExecution method,
    //       bad things may happen. Code the handleExecution methods carefully.
    void executeHandler(Task *receiver, const std::string &message) {
        if (supervisor->isObserving(this, receiver) && !receiver->isFinished())
            receiver->handleExecution(this, message);
        else
            log() << "Will not call handleExecution since task isn't observed.";
    }

    double elapsed() const {
        return secondsSince(start_time);
    }
#ifndef _WIN32
    // Override this if you intend to start child processes with createWorker().
    // It will be executed in the child process.
    // wno is the argument last parameter you supplied to createWorker().
    // You must create a Task with new and return its address.
    virtual Task *createWorkerTask(unsigned int wno) {
        log() << "missing createWorkerTask, cannot create worker " << wno;
        return nullptr;
    }

    // This will be called in child process directly before exit.
    // Override if you need to clean up after createWorkerTask.
    virtual void finishWorkerTask(unsigned int ) {
    }

    // Called during startup of worker process, once for each SocketReceiver
    // object. Will be called before serverAdded with the same SocketReceiver.
    // Overload this if your worker process has two or more SocketReceiver
    // objects with different confguration (i.e. different SSL keys).
    virtual void newWorkerChannel(SocketReceiver *, unsigned int ) {
    }

    // Called if parent/worker sends a message through a SocketReceiver:
    virtual void workerMessage(SocketReceiver *, const char *buf, size_t len) {
        log() << "Worker message: " << std::string(buf, len);
    }
#endif

    // Normally, SocketConnection objects are designed for a specific type of
    // Task, i.e. a HttpServerConnection might be designed for a WebServerTask.
    // However, some SocketConnection subclasses are "generic" and meant to work
    // with any Task ocbect as owner. When such SocketConnection objects want to
    // contact the owner, they may use the below methods.
    virtual PollState connectionReady(SocketConnection * /* conn */);
    virtual PollState msgFromConnection(SocketConnection * /* conn */,
                                        const std::string & /* msg */);

    // Number of bytes sent through SocketConnection objects owned by me:
    uint64_t bytesSent() const {
        return tot_bytes_sent;
    }

    // Number of bytes received through SocketConnection objects owned by me:
    uint64_t bytesReceived() const {
        return tot_bytes_received;
    }

    // Reset the values for the above two methods.
    void resetByteCount() {
        tot_bytes_sent = 0;
        tot_bytes_received = 0;
    }

    // Don't call the two below methods unless you know what you're doing.
    void notifyBytesSent(uint64_t n) {
        tot_bytes_sent += n;
    }
    void notifyBytesReceived(uint64_t n) {
        tot_bytes_received += n;
    }

protected:

    // To add a new client connection. Will delete conn and return false
    // on immediate failure. connRemoved will not be called in that case.
    //
    // The connection must belong to a _running_ task (probably this one).
    // I.e. the task must have been added to the EventLoop and the start()
    // method must have been called. You can't do this in the constructor!
    //
    // If successfully added, we will call connAdded and return true.
    // The EventLoop takes ownership of the SocketConnection object and
    // will call connRemoved and then delete it if the connection fails or
    // is closed or when the task is finished.
    bool addConnection(SocketConnection *conn);

    // Use this if conn contains a socket that has already been connected.
    // Returns false (and deletes conn) on failure.
    // On success, returns true and calls connAdded on owner task,
    // then calls connected() on conn to get initial state.
    bool addConnected(SocketConnection *conn);

    // As above, but with a server connection.
    bool addServer(ServerSocket *conn);

    // Check config file for listening (server) sockets. The sockets
    // are added to the task, with the given log_label. E.g.
    //
    //    listen 80 192.36.30.2
    //    listen 8080
    //    listen 443 tls /etc/ssl/fd.crt /etc/ssl/fd.key 4lEGyLax
    //
    // The value of the listen parameter is either a port number,
    // e.g. "8080", or a port number followed by a space and an ip address,
    // e.g. "8080 192.168.0.1". The address is either ipv4 or ipv6.
    // If connections are to be encrypted, add "tls" followed by
    // paths to your SSL certificate and private key, optionally followed
    // by the password (if the key is protected by a password).
    bool parseListen(const TaskConfig &tc, const std::string &log_label);

#ifdef USE_GNUTLS
    virtual bool tlsSetKey(ServerSocket *conn, const std::string &crt_path,
                   const std::string &key_path, const std::string &password) {
        return supervisor->tlsSetKey(conn, crt_path, key_path, password);
    }
#endif

    // To signal the task as done and set the result:
    // Of course, subclasses can calculate more complex
    // "results" in addition to this simple string.
    void setResult(const std::string &res);

    // Called to signal fatal error. Override to "catch" errors.
    virtual void setError(const std::string &msg) {
        log() << "Task failure: " << msg;
        was_error = true;
        setResult("");
    }

    virtual void setTimeout() {
        was_timeout = true;
        setResult("");
    }

    // To signal that the task has a "message" to deliver.
    void setMessage(const std::string &msg);

    // Called when an observed task, e.g. a child task, terminates.
    // Override this method to handle such events.
    virtual void taskFinished(Task *task) {
        dbg_log() << "Task " << task->label() << " died, no handler defined.";
    }

    virtual void taskMessage(Task *task) {
        dbg_log() << "No taskMessage handler implemented for " << task->label();
    }

    virtual void handleExecution(Task *sender, const std::string &message) {
        dbg_log() << "Event " << message << " from "
                  << (sender ? sender->label() : "unknown")
                  << ", no handler implemented";
    }

    bool terminated() const {
        return is_finished;
    }

    // Insert another Task for execution by the EventLoop.
    void addNewTask(Task *task, Task *parent = nullptr) {
        supervisor->addTask(task, parent);
    }

#ifdef USE_THREADS
    void addNewThread(Task *task, const std::string &name="ThreadLoop",
                      std::ostream *log_file = nullptr,
                      Task *parent = nullptr) {
        supervisor->spawnThread(task, name, log_file, parent);
    }
#endif

    void getMyTasks(std::set<Task *> &tset) {
        supervisor->getChildTasks(tset, this);
    }

    void abortMyTasks() {
        supervisor->abortChildTasks(this);
    }

    void abortTask(Task *task) {
        supervisor->abortTask(task);
    }

    void abortAllTasks() {
        supervisor->abort();
    }

    // Start execution of external command, return an ID.
    // On immediate failure, return value is -1.
    int runProcess(const char *const argv[]);

    // Will be called to notify when an external process has terminated.
    virtual void processFinished(int pid, int wstatus);

#ifndef _WIN32
    // Run task returned by this->createWorkerTask in a child process.
    // Return nullptr on failure.
    WorkerProcess *createWorker(std::ostream *log_file = nullptr,
                                unsigned int channels = 1,
                                unsigned int wno = 0) {
        return supervisor->createWorker(this, log_file, channels, wno);
    }
    WorkerProcess *createWorker(const std::string &log_file_name,
                                unsigned int channels = 1,
                                unsigned int wno = 0) {
        return supervisor->createWorker(this, log_file_name, channels, wno);
    }
#endif

private:
    friend class EventLoop;
    void setTerminated() {
        is_finished = true;
    }
    // Called by EventLoop when task is scheduled for execution:
    double begin() {
        has_started = true;
        start_time = timeNow();
        return this->start();
    }

#ifdef USE_THREADS
    thread_local
#endif
    static EventLoop *supervisor;
    TimePoint start_time;
    std::string the_result;
    std::string the_message;
    uint64_t tot_bytes_sent = 0, tot_bytes_received = 0;
    bool is_finished = false;
    bool has_started = false;
    bool was_killed = false, was_error = false, was_timeout = false;
    bool kill_children = false;
    bool is_child_thread = false;
};
