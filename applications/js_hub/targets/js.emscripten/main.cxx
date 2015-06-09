/** \copyright
 * Copyright (c) 2013, Balazs Racz
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are  permitted provided that the following conditions are met:
 *
 *  - Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 *  - Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * \file main.cxx
 *
 * An application which acts as an openlcb hub with the GC protocol.
 *
 * @author Balazs Racz
 * @date 3 Aug 2013
 */

#include <emscripten.h>
#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <memory>

#include "os/os.h"
#include "utils/constants.hxx"
#include "utils/Hub.hxx"
#include "utils/GridConnectHub.hxx"
#include "utils/GcTcpHub.hxx"
#include "executor/Executor.hxx"
#include "executor/Service.hxx"

Executor<1> g_executor{NO_THREAD()};
Service g_service(&g_executor);
CanHubFlow can_hub0(&g_service);
GcPacketPrinter packet_printer(&can_hub0);

OVERRIDE_CONST(gc_generate_newlines, 1);

int port = 12021;
const char *device_path = nullptr;

void usage(const char *e)
{
    fprintf(stderr, "Usage: %s [-p port] [-d device_path]\n\n", e);
    fprintf(stderr, "GridConnect CAN HUB.\nListens to a specific TCP port, "
                    "reads CAN packets from the incoming connections using "
                    "the GridConnect protocol, and forwards all incoming "
                    "packets to all other participants.\n\nArguments:\n");
    fprintf(stderr, "\t-p port     specifies the port number to listen on, "
                    "default is 12021.\n");
    fprintf(stderr, "\t-d device   is a path to a physical device doing "
                    "serial-CAN or USB-CAN. If specified, opens device and "
                    "adds it to the hub.\n");
    exit(1);
}

void parse_args(int argc, char *argv[])
{
    int opt;
    while ((opt = getopt(argc, argv, "hp:d:n:a:s:f:c:")) >= 0)
    {
        switch (opt)
        {
            case 'h':
                usage(argv[0]);
                break;
            case 'd':
                device_path = optarg;
                break;
            case 'p':
                port = atoi(optarg);
                break;
            default:
                fprintf(stderr, "Unknown option %c\n", opt);
                usage(argv[0]);
        }
    }
}

class JSHubPort : public HubPortInterface
{
public:
    JSHubPort(HubFlow *parent, emscripten::val send_fn)
        : parent_(parent)
        , sendFn_(send_fn)
    {
        HASSERT(sendFn_.typeof().as<std::string>() == "function");
        parent_->register_port(this);
    }

    ~JSHubPort()
    {
        parent_->unregister_port(this);
    }

    void send(HubPortInterface::message_type *buffer, unsigned priority = UINT_MAX) OVERRIDE
    {
        sendFn_((string &)*buffer->data());
        buffer->unref();
    }

    void recv(string s)
    {
        auto *b = parent_->alloc();
        b->data()->assign(s);
        b->data()->skipMember_ = this;
        parent_->send(b);
    }

private:
    HubFlow *parent_;
    emscripten::val sendFn_;
};

class JSTcpHub
{
public:
    JSTcpHub(CanHubFlow *hflow, int port)
        : canHub_(hflow)
        , gcHub_(canHub_->service())
        , gcAdapter_(
              GCAdapterBase::CreateGridConnectAdapter(&gcHub_, canHub_, false))
    {
        EM_ASM_(
        {
            var net = require('net');
            var server = net.createServer(function(c)
            {
                console.log('client connected');
                c.setEncoding('utf-8');
                var client_port = new Module.JSHubPort($1, function(data)
                { c.write(data); });
                c.on('close', function()
                {
                    console.log('client disconnected');
                    client_port.delete ();
                });
                c.on('data', function(data)
                { client_port.recv(data); });
            });
            server.listen($0, function()
            { console.log('listening on port ' + $0); });
        },
            port, &gcHub_);
    }

private:
    CanHubFlow *canHub_;
    HubFlow gcHub_;
    std::unique_ptr<GCAdapterBase> gcAdapter_;
};

/** Entry point to application.
 * @param argc number of command line arguments
 * @param argv array of command line arguments
 * @return 0, should never return
 */
int appl_main(int argc, char *argv[])
{
    parse_args(argc, argv);
    JSTcpHub hub(&can_hub0, port);
    /*    int dev_fd = 0;
    while (1)
    {
        if (device_path && !dev_fd)
        {
            dev_fd = ::open(device_path, O_RDWR);
            if (dev_fd > 0)
            {
                // Sets up the terminal in raw mode. Otherwise linux might echo
                // characters coming in from the device and that will make
                // packets go back to where they came from.
                HASSERT(!tcflush(dev_fd, TCIOFLUSH));
                struct termios settings;
                HASSERT(!tcgetattr(dev_fd, &settings));
                cfmakeraw(&settings);
                HASSERT(!tcsetattr(dev_fd, TCSANOW, &settings));
                LOG(INFO, "Opened device %s.\n", device_path);
                create_gc_port_for_can_hub(&can_hub0, dev_fd);
            }
            else
            {
                LOG(ERROR, "Failed to open device %s: %s\n", device_path,
                    strerror(errno));
            }
        }
        sleep(1);
        }*/
    g_executor.thread_body();
    return 0;
}

EMSCRIPTEN_BINDINGS(js_hub_module)
{
    emscripten::class_<JSHubPort>("JSHubPort")
        .constructor<HubFlow *, emscripten::val>()
        .function("recv", &JSHubPort::recv);
}