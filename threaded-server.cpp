/***********************************************************************
 threaded-server.cpp - Implements a simple Winsock server that accepts
    connections and spins each one off into its own thread, where it's
    treated as a blocking socket.

    Each connection thread reads data off the socket and echoes it
    back verbatim.

 Compiling:
    VC++: cl -GX threaded-server.cpp main.cpp ws-util.cpp wsock32.lib
    BC++: bcc32 threaded-server.cpp main.cpp ws-util.cpp
    
 This program is hereby released into the public domain.  There is
 ABSOLUTELY NO WARRANTY WHATSOEVER for this product.  Caveat hacker.
***********************************************************************/

#include "Proxy.hpp"
#include "ThreadPool.hpp"
#include "Logger.hpp"

#include "ws-util.h"
#include <Ws2tcpip.h>

#include <atomic>
#include <iostream>
#include <sstream>
using namespace std;


//// Global variables //////////////////////////////////////////////////

atomic_int g_numThreads;


//// SetUpListener /////////////////////////////////////////////////////
// Sets up a listener on the given interface and port, returning the
// listening socket if successful; if not, returns INVALID_SOCKET.

SOCKET SetUpListener(const char *pcHost, const char *pcPort) {
	addrinfo *result = NULL;
	addrinfo hints;

	ZeroMemory(&hints, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = AI_PASSIVE;

	// Resolve the server address and port.
	int iResult = getaddrinfo(NULL, pcPort, &hints, &result);
	if (iResult != 0) {
		cerr << WSAGetLastErrorMessage("getaddrinfo() failed") << endl;
		return INVALID_SOCKET;
	}

	// Create a SOCKET for connecting to server.
	SOCKET ListenSocket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
	if (ListenSocket == INVALID_SOCKET) {
		cerr << WSAGetLastErrorMessage("socket() failed") << endl;
		freeaddrinfo(result);
		return INVALID_SOCKET;
	}

	int opt = 1;
	setsockopt(ListenSocket, SOL_SOCKET, SO_REUSEADDR, (const char *) &opt, sizeof(opt));

	// Setup the TCP listening socket.
	iResult = bind(ListenSocket, result->ai_addr, (int) result->ai_addrlen);
	if (iResult == SOCKET_ERROR) {
		cerr << WSAGetLastErrorMessage("bind() failed") << endl;
		freeaddrinfo(result);
		closesocket(ListenSocket);
		return INVALID_SOCKET;
	}

	freeaddrinfo(result);

	iResult = listen(ListenSocket, SOMAXCONN);
	if (iResult == SOCKET_ERROR) {
		cerr << WSAGetLastErrorMessage("listen() failed") << endl;
		closesocket(ListenSocket);
		WSACleanup();
		return INVALID_SOCKET;
	}

	return ListenSocket;
}


//// EchoHandler ///////////////////////////////////////////////////////
// Handles the incoming data by reflecting it back to the sender.
VOID CALLBACK ProxyHandler(PTP_CALLBACK_INSTANCE, PVOID sd_) {
    ++g_numThreads;

    if (true) {
        SOCKET sd = (SOCKET) sd_;
        MyProxy proxy(sd);

        if (!proxy.HandleBrowser()) {
            Logger::LogError(__FUNC__ "Handling browser request failed");
        }

        if (!ShutdownConnection(sd)) {
            proxy.PrintRequest(Logger::OL_ERROR);
            Logger::LogError(__FUNC__ "Connection shutdown failed");
        }
    }

    Logger::LogInfo("Threadpool work done!");

    --g_numThreads;
}


//// AcceptConnections /////////////////////////////////////////////////
// Spins forever waiting for connections.  For each one that comes in, 
// we create a thread to handle it and go back to waiting for
// connections.  If an error occurs, we return.

void AcceptConnections(SOCKET ListeningSocket) {
    Logger::LEVEL = Logger::OL_ERROR;
    Logger::CONSOLE = false;

    ThreadPool pool;

    ostringstream ss;
    char szIPv4[24] = {};

    sockaddr_in sinRemote;
    int nAddrSize = sizeof(sinRemote);

    while (true) {
        SOCKET sd = accept
            (ListeningSocket, (sockaddr *) &sinRemote, &nAddrSize);

        if (sd != INVALID_SOCKET) {
            inet_ntop(AF_INET, &sinRemote.sin_addr, szIPv4, sizeof(szIPv4));

            cout << "Accepted connection from " << szIPv4 << ":" <<
                     ntohs(sinRemote.sin_port) << endl <<
                    "-- Threads count: " << (g_numThreads + 1) << endl;

            pool.SetThreadMinimum(g_numThreads + 1);
            if (!pool.CreateWork(ProxyHandler, (void *) sd)) {
                pool.SetThreadMinimum(g_numThreads);

                ss << "ThreadPool::CreateWork() failed --" << GetLastError();
                Logger::LogError(ss.str());
                ss.str(string());
            }
        }
        else {
            Logger::LogError(WSAGetLastErrorMessage("accept() failed"));
            return;
        }
    }
}


//// DoWinsock /////////////////////////////////////////////////////////
// The module's driver function -- we just call other functions and
// interpret their results.

int DoWinsock(const char *pcAddr, const char *pcPort) {
    cout << "Establishing the listener on port " << pcPort << "..." << endl;
    SOCKET ListeningSocket = SetUpListener(pcAddr, pcPort);
    if (ListeningSocket == INVALID_SOCKET) {
        cout << endl << WSAGetLastErrorMessage("establish listener") << endl;
        return 3;
    }

    cout << "Waiting for connections..." << endl;
    AcceptConnections(ListeningSocket);

    return 0;
}
