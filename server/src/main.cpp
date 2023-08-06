#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/strings/str_format.h"

#include <grpc/support/log.h>
#include <grpcpp/grpcpp.h>

#include <csproto/request.pb.h>
#include <csproto/response.grpc.pb.h>

using namespace std;

ABSL_FLAG(uint16_t, port, 50051, "Server port for the service");

using grpc::Server;
using grpc::ServerAsyncResponseWriter;
using grpc::ServerBuilder;
using grpc::ServerCompletionQueue;
using grpc::ServerContext;
using grpc::Status;
using compiler_server::Compiler;
using compiler_server::CompileArgs;
using compiler_server::CompileResponseArgs;

// Class encompasing the state and logic needed to serve a request.
class CallData {
public:
    // Take in the "service" instance (in this case representing an asynchronous
    // server) and the completion queue "cq" used for asynchronous communication
    // with the gRPC runtime.
    CallData(Compiler::AsyncService* service, ServerCompletionQueue* cq)
    : service_(service), cq_(cq), status_(CREATE)
    {
    }
    virtual ~CallData() {}
    virtual void create() = 0;
    virtual void process() = 0;
    void Proceed() {
        if (status_ == CREATE) {
            //cout << '[' << getpid() << "] status == CREATE" << endl;
            create();
            status_ = PROCESS;
        } else if (status_ == PROCESS) {
            //cout << '[' << getpid() << "] status == PROCESS" << endl;
            // The actual processing.
            process();
            status_ = FINISH;
        } else {
            //cout << '[' << getpid() << "] status == FINISH" << endl;
            GPR_ASSERT(status_ == FINISH);
            // Once in the FINISH state, deallocate ourselves (CallData).
            delete this;
        }
    }

protected:
    // The means of communication with the gRPC runtime for an asynchronous
    // server.
    Compiler::AsyncService* service_;
    // The producer-consumer queue where for asynchronous server notifications.
    ServerCompletionQueue* cq_;
    // Context for the rpc, allowing to tweak aspects of it such as the use
    // of compression, authentication, as well as to send metadata back to the
    // client.
    ServerContext ctx_;

    // Let's implement a tiny state machine with the following states.
    enum CallStatus { CREATE, PROCESS, FINISH };
    CallStatus status_;  // The current serving state.
};



class CompileCallData : public CallData
{
public:
    // Take in the "service" instance (in this case representing an asynchronous
    // server) and the completion queue "cq" used for asynchronous communication
    // with the gRPC runtime.
    CompileCallData(Compiler::AsyncService* service, ServerCompletionQueue* cq)
    : CallData(service, cq), responder_(&ctx_)
    {
        // Invoke the serving logic right away.
        Proceed();
    }

    virtual void create() override
    {
        cout << __PRETTY_FUNCTION__ << endl;
        // As part of the initial CREATE state, we *request* that the system
        // start processing SayHello requests. In this request, "this" acts as
        // the tag uniquely identifying the request so that different CallData
        // instances can serve different requests concurrently.
        service_->RequestCompile(&ctx_, &request_, &responder_, cq_, cq_,
                                 this);
    }
    virtual void process() override
    {
        cout << __PRETTY_FUNCTION__ << endl;
        // Spawn a new CallData instance to serve new clients while we process
        // the one for this CallData. The instance will deallocate itself as
        // part of its FINISH state.
        new CompileCallData(service_, cq_);

        // The actual processing.
        response_.set_id(request_.id());

        // And we are done! Let the gRPC runtime know we've finished, using the
        // memory address of this instance as the uniquely identifying tag for
        // the event.
    }
    void post_process() {
        responder_.Finish(response_, Status::OK, this);
    }

//private:
    // What we get from the client.
    CompileArgs request_;
    // What we send back to the client.
    CompileResponseArgs response_;

    // The means to get back to the client.
    ServerAsyncResponseWriter<CompileResponseArgs> responder_;
};

class ShutdownCallData : public CallData
{
public:
    // Take in the "service" instance (in this case representing an asynchronous
    // server) and the completion queue "cq" used for asynchronous communication
    // with the gRPC runtime.
    ShutdownCallData(Compiler::AsyncService* service, ServerCompletionQueue* cq)
        : CallData(service, cq), responder_(&ctx_)
    {
        // Invoke the serving logic right away.
        Proceed();
    }

    virtual void create() override
    {
        cout << __PRETTY_FUNCTION__ << endl;
        // As part of the initial CREATE state, we *request* that the system
        // start processing SayHello requests. In this request, "this" acts as
        // the tag uniquely identifying the request so that different CallData
        // instances can serve different requests concurrently.
        service_->RequestShutdownServer(&ctx_, &request_, &responder_, cq_, cq_,
                                 this);
    }
    virtual void process() override
    {
        cout << __PRETTY_FUNCTION__ << endl;
        // The actual processing.
        response_.set_id(0);
        response_.set_pid(0);
        // And we are done! Let the gRPC runtime know we've finished, using the
        // memory address of this instance as the uniquely identifying tag for
        // the event.
        responder_.Finish(response_, Status::OK, this);
    }

private:
    // What we get from the client.
    ::google::protobuf::Empty request_;
    // What we send back to the client.
    CompileResponseArgs response_;

    // The means to get back to the client.
    ServerAsyncResponseWriter<CompileResponseArgs> responder_;
};

// https://stackoverflow.com/questions/70959328/grpc-c-server-shutdown
pthread_t tid; // Shutdown thread handle
pid_t server_pid;
class ServerImpl final {
public:
    ~ServerImpl() {
        server_->Shutdown();
        // Always shutdown the completion queue after the server.
        compile_cq_->Shutdown();
    }

    // There is no shutdown handling in this code.
    void Run(uint16_t port) {
        std::string server_address = absl::StrFormat("0.0.0.0:%d", port);
        server_pid = getpid();

        ServerBuilder builder;
        // Listen on the given address without any authentication mechanism.
        builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
        // Register "service_" as the instance through which we'll communicate with
        // clients. In this case it corresponds to an *asynchronous* service.
        builder.RegisterService(&service_);
        // Get hold of the completion queue used for the asynchronous communication
        // with the gRPC runtime.
        compile_cq_ = builder.AddCompletionQueue();
        shutdown_cq_ = builder.AddCompletionQueue();
        // Finally assemble the server.
        server_ = builder.BuildAndStart();
        cout << '[' << getpid() << "] Server listening on " << server_address << endl;

        int err = pthread_create(&(tid), NULL, &shutdownThreadFn, this);
        if (err != 0) {
            cerr << "Can't create Shutdown thread [" << strerror(err) << "]." << endl;
            return;
        }

        // Proceed to the server's main loop.
        HandleCompileRPCs();
    }
private:
    static void * shutdownThreadFn(void * server)
    {
        ((ServerImpl *)server)->HandleShutdownRPCs();
        return nullptr;
    }
    // This can be run in multiple threads if needed.
    void HandleShutdownRPCs() {
        // Spawn a new CallData instance to serve new clients.
        new ShutdownCallData(&service_, shutdown_cq_.get());
        void* tag;  // uniquely identifies a request.
        bool ok;
        // Block waiting to read the next event from the completion queue. The
        // event is uniquely identified by its tag, which in this case is the
        // memory address of a CallData instance.
        // The return value of Next should always be checked. This return value
        // tells us whether there is any kind of event or cq_ is shutting down.
        GPR_ASSERT(shutdown_cq_->Next(&tag, &ok));
        if(!ok) {
            cout << '[' << getpid() << "] shutdown queue done." << endl;
            return;
        }
        static_cast<CallData*>(tag)->Proceed();

        gpr_timespec ts = {0, 50*1000*1000, GPR_TIMESPAN}; // 50 ms POOMA
        server_->Shutdown(ts);
    }

    // This can be run in multiple threads if needed.
    void HandleCompileRPCs() {
        // Spawn a new CallData instance to serve new clients.
        new CompileCallData(&service_, compile_cq_.get());
        void* tag;  // uniquely identifies a request.
        bool ok;
        uint32_t last_id = 0;
        while (true) {
            // Block waiting to read the next event from the completion queue. The
            // event is uniquely identified by its tag, which in this case is the
            // memory address of a CallData instance.
            // The return value of Next should always be checked. This return value
            // tells us whether there is any kind of event or cq_ is shutting down.
            GPR_ASSERT(compile_cq_->Next(&tag, &ok));
            if(!ok) {
                cout << '[' << getpid() << "] compile queue done." << endl;
                break;
            }
            CompileCallData * p = static_cast<CompileCallData*>(tag);
            p->Proceed();
            // Somehow we get here twice per client call.
            // This detects that.
            if(last_id != p->request_.id()) {
                last_id = p->request_.id();
                int pid = fork();
                if(!pid) { // Child
                    cout << '[' << getpid() << "] process started." << endl;
                    sleep(1);
                    break;
                }else if(pid > 0) { // Parent
                    p->response_.set_pid((uint32_t)pid);
                    p->post_process();
                    continue; // Continue serving requests.
                }else{
                    cerr << "fork() failed: " << pid << endl;
                    return;
                }
            }
        }
    }

    std::unique_ptr<ServerCompletionQueue> compile_cq_;
    std::unique_ptr<ServerCompletionQueue> shutdown_cq_;
    Compiler::AsyncService service_;
    std::unique_ptr<Server> server_;
};

int main(int argc, char** argv) {
    absl::ParseCommandLine(argc, argv);
    ServerImpl server;
    server.Run(absl::GetFlag(FLAGS_port));
    if(getpid() == server_pid) {
        sleep(1);
        cout << '[' << getpid() << "] SERVER process finished." << endl;
    }else
        cout << '[' << getpid() << "] process finished." << endl;

    return 0;
}
