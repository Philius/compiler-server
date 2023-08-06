#include <csproto/request.pb.h>
#include <csproto/response.grpc.pb.h>

#include <grpc/grpc.h>
#include <grpcpp/create_channel.h>

#include <iostream>
#include <iomanip>
#include <sstream>

using namespace std;

int shutdown()
{
    cout << "Requesting server shutdown." << endl;
    ::google::protobuf::Empty request;
    compiler_server::CompileResponseArgs response;

    // Call
    auto channel = grpc::CreateChannel("localhost:50051", grpc::InsecureChannelCredentials());
    std::unique_ptr<compiler_server::Compiler::Stub> stub = compiler_server::Compiler::NewStub(channel);
    grpc::ClientContext context;
    grpc::Status status = stub->ShutdownServer(&context, request, &response);

    if(!status.ok()) {
        cerr << "Got a bad status: " << status.error_message() << endl;
        return 1;
    }
    return 0;
}
int main(int argc, char* /*argv*/[])
{
    if(argc == 1) {
        // Setup request
        compiler_server::CompileArgs request;
        compiler_server::CompileResponseArgs response;
        auto channel = grpc::CreateChannel("localhost:50051", grpc::InsecureChannelCredentials());
        std::unique_ptr<compiler_server::Compiler::Stub> stub = compiler_server::Compiler::NewStub(channel);
        for(uint32_t n = 0; n < 10; ++n) {
            string s;
            {
                stringstream ss;
                ss << setfill('0') << setw(3) << n;
                ss >> s;
            }
            request.set_id(n+1);
            request.set_src(s + ".cpp");
            request.set_obj(s + ".cpp.o");

            // Call
            grpc::ClientContext context;
            grpc::Status status = stub->Compile(&context, request, &response);
            if(!status.ok()) {
                cerr << "Got a bad status: " << status.error_message() << endl;
                return 1;
            }

            // Output result
            std::cout << "I got:" << std::endl;
            std::cout << "id: " << response.id() << ", pid: " << response.pid() << endl;
        }
        return shutdown();
    }else{
        return shutdown();
    }
    return 0;
}
